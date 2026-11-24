#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_PNG_MAX_DIM 512u
#define PEBBLE_PNG_MAX_ROW_BYTES (PEBBLE_PNG_MAX_DIM * 4u)
#define PEBBLE_DEFLATE_WIN_SIZE 32768u
#define PEBBLE_DEFLATE_WIN_MASK (PEBBLE_DEFLATE_WIN_SIZE - 1u)
#define PEBBLE_HUFF_MAX_BITS 15u
#define PEBBLE_HUFF_MAX_SYMS 288u

/* Static scratch buffers for streaming PNG decode (keeps app heap free for bitmaps). */
static uint8_t g_inflate_win[PEBBLE_DEFLATE_WIN_SIZE];
static uint8_t g_png_row_cur[PEBBLE_PNG_MAX_ROW_BYTES + 1u];
static uint8_t g_png_row_prev[PEBBLE_PNG_MAX_ROW_BYTES];

static uint16_t read_u16_be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint8_t quantize_2bit(uint8_t v) {
    return (uint8_t)(((uint32_t)v * 3u + 127u) / 255u);
}

static uint8_t rgba8_to_gcolor8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t a2 = quantize_2bit(a);
    if (a2 == 0u) {
        return PEBBLE_GCOLOR_CLEAR;
    }
    return (uint8_t)((a2 << 6) | (quantize_2bit(r) << 4) | (quantize_2bit(g) << 2) | quantize_2bit(b));
}

static uint8_t format_from_info_flags(uint16_t info_flags) {
    uint16_t ver = (uint16_t)((info_flags >> 12) & 0x0Fu);
    if (ver == 0u) {
        return PEBBLE_GBITMAP_FORMAT_1BIT;
    }
    return (uint8_t)((info_flags >> 1) & 0x07u);
}

uint16_t pebble_gbitmap_make_info_flags(uint8_t format, bool heap_data, bool heap_palette) {
    uint16_t flags = (uint16_t)(1u << 12) | (uint16_t)(((uint16_t)format & 0x07u) << 1);
    if (heap_data) flags |= 1u << 0;
    if (heap_palette) flags |= 1u << 4;
    return flags;
}

static uint16_t palette_count_for_format(uint8_t format) {
    switch (format) {
        case PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE: return 2u;
        case PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE: return 4u;
        case PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE: return 16u;
        default: return 0u;
    }
}

uint16_t pebble_gbitmap_row_bytes_for_format(uint16_t width, uint8_t format) {
    switch (format) {
        case PEBBLE_GBITMAP_FORMAT_1BIT:
            return (uint16_t)(((width + 31u) / 32u) * 4u);
        case PEBBLE_GBITMAP_FORMAT_8BIT:
        case PEBBLE_GBITMAP_FORMAT_8BIT_CIRCULAR:
            return width;
        case PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE:
            return (uint16_t)((width + 7u) / 8u);
        case PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE:
            return (uint16_t)((width + 3u) / 4u);
        case PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE:
            return (uint16_t)((width + 1u) / 2u);
        default:
            return 0u;
    }
}

/* --- Freestanding RFC 1951 DEFLATE + RFC 1950 zlib decoder --- */

typedef struct {
    const uint8_t *src;
    size_t src_len;
    size_t byte_pos;
    uint32_t bit_buf;
    uint32_t bit_cnt;
    bool error;
} bit_reader_t;

static void br_fill(bit_reader_t *br, uint32_t need) {
    while (br->bit_cnt < need && br->byte_pos < br->src_len) {
        br->bit_buf |= (uint32_t)br->src[br->byte_pos++] << br->bit_cnt;
        br->bit_cnt += 8u;
    }
}

static uint32_t br_read_bits(bit_reader_t *br, uint32_t n) {
    if (n == 0u) {
        return 0u;
    }
    br_fill(br, n);
    if (br->bit_cnt < n) {
        br->error = true;
        return 0u;
    }
    uint32_t val = br->bit_buf & ((1u << n) - 1u);
    br->bit_buf >>= n;
    br->bit_cnt -= n;
    return val;
}

static void br_align_byte(bit_reader_t *br) {
    uint32_t drop = br->bit_cnt & 7u;
    br->bit_buf >>= drop;
    br->bit_cnt -= drop;
}

static uint8_t br_read_aligned_byte(bit_reader_t *br) {
    if (br->bit_cnt >= 8u) {
        uint8_t b = (uint8_t)(br->bit_buf & 0xFFu);
        br->bit_buf >>= 8;
        br->bit_cnt -= 8u;
        return b;
    }
    if (br->byte_pos >= br->src_len) {
        br->error = true;
        return 0u;
    }
    return br->src[br->byte_pos++];
}

typedef struct {
    uint16_t count[PEBBLE_HUFF_MAX_BITS + 1u];
    uint16_t first_code[PEBBLE_HUFF_MAX_BITS + 1u];
    uint16_t first_sym[PEBBLE_HUFF_MAX_BITS + 1u];
    uint16_t symbols[PEBBLE_HUFF_MAX_SYMS];
} huff_tree_t;

static bool huff_build(huff_tree_t *t, const uint8_t *lengths, uint32_t num_syms) {
    if (num_syms > PEBBLE_HUFF_MAX_SYMS) {
        return false;
    }
    for (uint32_t i = 0u; i <= PEBBLE_HUFF_MAX_BITS; ++i) {
        t->count[i] = 0u;
        t->first_code[i] = 0u;
        t->first_sym[i] = 0u;
    }
    for (uint32_t i = 0u; i < num_syms; ++i) {
        if (lengths[i] > PEBBLE_HUFF_MAX_BITS) {
            return false;
        }
        if (lengths[i] > 0u) {
            t->count[lengths[i]]++;
        }
    }
    uint32_t code = 0u;
    uint32_t sym_idx = 0u;
    uint16_t offs[PEBBLE_HUFF_MAX_BITS + 1u];
    offs[0] = 0u;
    for (uint32_t bits = 1u; bits <= PEBBLE_HUFF_MAX_BITS; ++bits) {
        code = (code + t->count[bits - 1u]) << 1;
        t->first_code[bits] = (uint16_t)code;
        t->first_sym[bits] = (uint16_t)sym_idx;
        offs[bits] = (uint16_t)sym_idx;
        sym_idx += t->count[bits];
    }
    for (uint32_t i = 0u; i < num_syms; ++i) {
        uint8_t len = lengths[i];
        if (len > 0u) {
            t->symbols[offs[len]++] = (uint16_t)i;
        }
    }
    return true;
}

static int32_t huff_decode(bit_reader_t *br, const huff_tree_t *t) {
    uint32_t code = 0u;
    for (uint32_t bits = 1u; bits <= PEBBLE_HUFF_MAX_BITS; ++bits) {
        code = (code << 1) | br_read_bits(br, 1u);
        if (br->error) {
            return -1;
        }
        uint32_t fc = t->first_code[bits];
        uint32_t cnt = t->count[bits];
        if (code >= fc && (code - fc) < cnt) {
            return (int32_t)t->symbols[t->first_sym[bits] + (code - fc)];
        }
    }
    br->error = true;
    return -1;
}

/* --- Streaming PNG scanline unfilter + GBitmap row writer --- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint32_t raw_row_bytes;
    uint32_t filter_bpp;
    uint32_t row_pos;
    uint32_t cur_y;
    uint32_t win_pos;
    pebble_gbitmap_t *dst_bmp;
    uint8_t plte_gcolor8[256];
    bool has_trns_key;
    uint16_t trns_r;
    uint16_t trns_g;
    uint16_t trns_b;
    bool error;
} png_sink_t;

static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int32_t p = (int32_t)a + (int32_t)b - (int32_t)c;
    int32_t pa = p - (int32_t)a;
    if (pa < 0) pa = -pa;
    int32_t pb = p - (int32_t)b;
    if (pb < 0) pb = -pb;
    int32_t pc = p - (int32_t)c;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static void png_finish_scanline(png_sink_t *s) {
    if (s->cur_y >= s->height) {
        return;
    }
    uint8_t filter = g_png_row_cur[0];
    uint8_t *cur = &g_png_row_cur[1];
    uint8_t *prev = g_png_row_prev;
    uint32_t n = s->raw_row_bytes;
    uint32_t bpp = s->filter_bpp;

    if (filter == 1u) {
        for (uint32_t i = bpp; i < n; ++i) {
            cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        }
    } else if (filter == 2u) {
        for (uint32_t i = 0u; i < n; ++i) {
            cur[i] = (uint8_t)(cur[i] + prev[i]);
        }
    } else if (filter == 3u) {
        for (uint32_t i = 0u; i < n; ++i) {
            uint8_t a = (i >= bpp) ? cur[i - bpp] : 0u;
            uint8_t b = prev[i];
            cur[i] = (uint8_t)(cur[i] + (((uint32_t)a + (uint32_t)b) >> 1));
        }
    } else if (filter == 4u) {
        for (uint32_t i = 0u; i < n; ++i) {
            uint8_t a = (i >= bpp) ? cur[i - bpp] : 0u;
            uint8_t b = prev[i];
            uint8_t c = (i >= bpp) ? prev[i - bpp] : 0u;
            cur[i] = (uint8_t)(cur[i] + paeth_predictor(a, b, c));
        }
    } else if (filter != 0u) {
        s->error = true;
        return;
    }

    uint8_t *dst_row = s->dst_bmp->addr + (size_t)s->cur_y * (size_t)s->dst_bmp->row_size_bytes;
    uint8_t dst_fmt = format_from_info_flags(s->dst_bmp->info_flags);

    if (s->color_type == 3u && (dst_fmt == PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE ||
                                dst_fmt == PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE ||
                                dst_fmt == PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE)) {
        /* Both PNG indexed (1/2/4-bit) and Pebble palettized bitmaps pack pixels MSB-first. */
        for (uint32_t i = 0u; i < n && i < (uint32_t)s->dst_bmp->row_size_bytes; ++i) {
            dst_row[i] = cur[i];
        }
    } else if (s->color_type == 3u && s->bit_depth == 8u) {
        for (uint32_t x = 0u; x < s->width; ++x) {
            dst_row[x] = s->plte_gcolor8[cur[x]];
        }
    } else if (s->color_type == 0u) {
        /* Grayscale (1, 2, 4, or 8 bit) */
        uint32_t max_val = (1u << s->bit_depth) - 1u;
        uint32_t ppb = 8u / s->bit_depth;
        for (uint32_t x = 0u; x < s->width; ++x) {
            uint32_t shift = (ppb - 1u - (x % ppb)) * s->bit_depth;
            uint32_t raw_g = (cur[x / ppb] >> shift) & max_val;
            uint8_t g8 = (uint8_t)((raw_g * 255u) / max_val);
            uint8_t a8 = (s->has_trns_key && raw_g == s->trns_r) ? 0u : 255u;
            dst_row[x] = rgba8_to_gcolor8(g8, g8, g8, a8);
        }
    } else if (s->color_type == 2u && s->bit_depth == 8u) {
        /* RGB888 */
        for (uint32_t x = 0u; x < s->width; ++x) {
            uint8_t r = cur[x * 3u + 0u];
            uint8_t g = cur[x * 3u + 1u];
            uint8_t b = cur[x * 3u + 2u];
            uint8_t a = (s->has_trns_key && r == s->trns_r && g == s->trns_g && b == s->trns_b) ? 0u : 255u;
            dst_row[x] = rgba8_to_gcolor8(r, g, b, a);
        }
    } else if (s->color_type == 4u && s->bit_depth == 8u) {
        /* Gray + Alpha */
        for (uint32_t x = 0u; x < s->width; ++x) {
            uint8_t g = cur[x * 2u + 0u];
            uint8_t a = cur[x * 2u + 1u];
            dst_row[x] = rgba8_to_gcolor8(g, g, g, a);
        }
    } else if (s->color_type == 6u && s->bit_depth == 8u) {
        /* RGBA8888 */
        for (uint32_t x = 0u; x < s->width; ++x) {
            dst_row[x] = rgba8_to_gcolor8(
                cur[x * 4u + 0u], cur[x * 4u + 1u], cur[x * 4u + 2u], cur[x * 4u + 3u]);
        }
    } else {
        s->error = true;
        return;
    }

    for (uint32_t i = 0u; i < n; ++i) {
        prev[i] = cur[i];
    }
    s->cur_y++;
}

static void png_emit_byte(png_sink_t *s, uint8_t b) {
    g_inflate_win[s->win_pos & PEBBLE_DEFLATE_WIN_MASK] = b;
    s->win_pos++;
    if (s->cur_y >= s->height || s->error) {
        return;
    }
    g_png_row_cur[s->row_pos++] = b;
    if (s->row_pos == 1u + s->raw_row_bytes) {
        png_finish_scanline(s);
        s->row_pos = 0u;
    }
}

static bool inflate_zlib_to_png_sink(const uint8_t *zdata, size_t zlen, png_sink_t *sink) {
    if (zdata == NULL || zlen < 6u) {
        return false;
    }
    uint8_t cmf = zdata[0];
    uint8_t flg = zdata[1];
    if ((cmf & 0x0Fu) != 8u || (((uint32_t)cmf * 256u + (uint32_t)flg) % 31u) != 0u) {
        return false;
    }
    if ((flg & 0x20u) != 0u) {
        return false; /* Preset dictionary unsupported in PNG */
    }

    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static const uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    bit_reader_t br = {
        .src = zdata + 2u,
        .src_len = zlen - 2u,
        .byte_pos = 0u,
        .bit_buf = 0u,
        .bit_cnt = 0u,
        .error = false,
    };

    huff_tree_t litlen_tree;
    huff_tree_t dist_tree;
    huff_tree_t cl_tree;
    uint8_t lengths[288u + 32u];

    bool bfinal = false;
    while (!bfinal && !br.error && !sink->error) {
        bfinal = (br_read_bits(&br, 1u) != 0u);
        uint32_t btype = br_read_bits(&br, 2u);
        if (br.error) {
            return false;
        }
        if (btype == 0u) {
            br_align_byte(&br);
            uint16_t len = (uint16_t)br_read_aligned_byte(&br) |
                           ((uint16_t)br_read_aligned_byte(&br) << 8);
            uint16_t nlen = (uint16_t)br_read_aligned_byte(&br) |
                            ((uint16_t)br_read_aligned_byte(&br) << 8);
            if (br.error || (uint16_t)(len ^ nlen) != 0xFFFFu) {
                return false;
            }
            for (uint16_t i = 0u; i < len; ++i) {
                uint8_t b = br_read_aligned_byte(&br);
                if (br.error) return false;
                png_emit_byte(sink, b);
            }
        } else if (btype == 1u || btype == 2u) {
            if (btype == 1u) {
                for (uint32_t i = 0u; i <= 143u; ++i) lengths[i] = 8u;
                for (uint32_t i = 144u; i <= 255u; ++i) lengths[i] = 9u;
                for (uint32_t i = 256u; i <= 279u; ++i) lengths[i] = 7u;
                for (uint32_t i = 280u; i <= 287u; ++i) lengths[i] = 8u;
                if (!huff_build(&litlen_tree, lengths, 288u)) return false;
                for (uint32_t i = 0u; i < 32u; ++i) lengths[i] = 5u;
                if (!huff_build(&dist_tree, lengths, 32u)) return false;
            } else {
                uint32_t hlit = br_read_bits(&br, 5u) + 257u;
                uint32_t hdist = br_read_bits(&br, 5u) + 1u;
                uint32_t hclen = br_read_bits(&br, 4u) + 4u;
                if (br.error || hlit > 286u || hdist > 32u) {
                    return false;
                }
                uint8_t cl_lens[19];
                for (uint32_t i = 0u; i < 19u; ++i) cl_lens[i] = 0u;
                for (uint32_t i = 0u; i < hclen; ++i) {
                    cl_lens[kCodeLenOrder[i]] = (uint8_t)br_read_bits(&br, 3u);
                }
                if (br.error || !huff_build(&cl_tree, cl_lens, 19u)) {
                    return false;
                }
                uint32_t total = hlit + hdist;
                uint32_t idx = 0u;
                while (idx < total) {
                    int32_t sym = huff_decode(&br, &cl_tree);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lengths[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (idx == 0u) return false;
                        uint32_t rep = br_read_bits(&br, 2u) + 3u;
                        uint8_t prev_l = lengths[idx - 1u];
                        while (rep-- > 0u && idx < total) lengths[idx++] = prev_l;
                    } else if (sym == 17) {
                        uint32_t rep = br_read_bits(&br, 3u) + 3u;
                        while (rep-- > 0u && idx < total) lengths[idx++] = 0u;
                    } else if (sym == 18) {
                        uint32_t rep = br_read_bits(&br, 7u) + 11u;
                        while (rep-- > 0u && idx < total) lengths[idx++] = 0u;
                    } else {
                        return false;
                    }
                }
                if (!huff_build(&litlen_tree, lengths, hlit)) return false;
                if (!huff_build(&dist_tree, lengths + hlit, hdist)) return false;
            }

            while (!br.error && !sink->error) {
                int32_t sym = huff_decode(&br, &litlen_tree);
                if (sym < 0) return false;
                if (sym < 256) {
                    png_emit_byte(sink, (uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    uint32_t lidx = (uint32_t)(sym - 257);
                    if (lidx >= 29u) return false;
                    uint32_t match_len = kLenBase[lidx] + br_read_bits(&br, kLenExtra[lidx]);
                    int32_t dsym = huff_decode(&br, &dist_tree);
                    if (dsym < 0 || dsym >= 30) return false;
                    uint32_t dist = kDistBase[dsym] + br_read_bits(&br, kDistExtra[dsym]);
                    if (br.error || dist == 0u || dist > sink->win_pos) return false;
                    for (uint32_t k = 0u; k < match_len; ++k) {
                        uint8_t b = g_inflate_win[(sink->win_pos - dist) & PEBBLE_DEFLATE_WIN_MASK];
                        png_emit_byte(sink, b);
                    }
                }
            }
        } else {
            return false;
        }
    }

    return !br.error && !sink->error && sink->cur_y == sink->height;
}

/* --- GBitmap core helpers --- */

static void destroy_gbitmap_internal(pebble_gbitmap_t *bmp) {
    if (bmp == NULL) {
        return;
    }
    if (bmp->owns_data && bmp->addr != NULL) {
        pebble_app_free(bmp->addr);
    }
    if (bmp->owns_palette && bmp->palette != NULL) {
        pebble_app_free(bmp->palette);
    }
    pebble_app_free(bmp);
}

static pebble_gbitmap_t *create_blank_internal(
    pebble_gsize_t size,
    uint8_t format,
    pebble_gcolor8_t *palette,
    bool free_palette_on_destroy,
    bool alloc_default_palette) {
    if (size.w <= 0 || size.h <= 0) {
        return NULL;
    }
    uint16_t row_bytes = pebble_gbitmap_row_bytes_for_format((uint16_t)size.w, format);
    if (row_bytes == 0u) {
        return NULL;
    }
    size_t data_bytes = (size_t)row_bytes * (size_t)size.h;
    pebble_gbitmap_t *bmp = (pebble_gbitmap_t *)pebble_app_calloc(1u, sizeof(pebble_gbitmap_t));
    if (bmp == NULL) {
        return NULL;
    }
    bmp->addr = (uint8_t *)pebble_app_calloc(1u, data_bytes);
    if (bmp->addr == NULL) {
        pebble_app_free(bmp);
        return NULL;
    }
    bmp->row_size_bytes = row_bytes;
    bmp->bounds.origin.x = 0;
    bmp->bounds.origin.y = 0;
    bmp->bounds.size = size;
    bmp->data_width = (uint16_t)size.w;
    bmp->data_height = (uint16_t)size.h;
    bmp->owns_data = true;

    uint16_t pal_n = palette_count_for_format(format);
    if (pal_n > 0u) {
        if (palette != NULL) {
            bmp->palette = palette;
            bmp->owns_palette = free_palette_on_destroy;
        } else if (alloc_default_palette) {
            bmp->palette = (pebble_gcolor8_t *)pebble_app_calloc(pal_n, sizeof(pebble_gcolor8_t));
            if (bmp->palette == NULL) {
                pebble_app_free(bmp->addr);
                pebble_app_free(bmp);
                return NULL;
            }
            bmp->owns_palette = true;
        }
    }
    bmp->info_flags = pebble_gbitmap_make_info_flags(format, bmp->owns_data, bmp->owns_palette);
    return bmp;
}

static bool is_png_signature(const uint8_t *buf, size_t len) {
    static const uint8_t kSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (buf == NULL || len < 8u) {
        return false;
    }
    for (size_t i = 0u; i < 8u; ++i) {
        if (buf[i] != kSig[i]) {
            return false;
        }
    }
    return true;
}

static pebble_gbitmap_t *decode_png_bytes(const uint8_t *png_data, size_t png_len) {
    if (!is_png_signature(png_data, png_len)) {
        return NULL;
    }
    bool have_ihdr = false;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint8_t bit_depth = 0u;
    uint8_t color_type = 0u;
    uint8_t plte_rgb[256u * 3u];
    uint32_t plte_count = 0u;
    uint8_t trns_alpha[256];
    for (uint32_t i = 0u; i < 256u; ++i) {
        trns_alpha[i] = 255u;
    }
    bool has_trns_key = false;
    uint16_t trns_r = 0u;
    uint16_t trns_g = 0u;
    uint16_t trns_b = 0u;

    const uint8_t *single_idat = NULL;
    size_t single_idat_len = 0u;
    uint32_t idat_chunks = 0u;
    size_t total_idat_len = 0u;

    size_t pos = 8u;
    while (pos + 12u <= png_len) {
        uint32_t clen = read_u32_be(png_data + pos);
        const uint8_t *ctype = png_data + pos + 4u;
        if (clen > png_len - (pos + 12u)) {
            return NULL;
        }
        const uint8_t *cdata = png_data + pos + 8u;
        pos += 12u + (size_t)clen;

        if (ctype[0] == 'I' && ctype[1] == 'H' && ctype[2] == 'D' && ctype[3] == 'R') {
            if (clen < 13u) return NULL;
            width = read_u32_be(cdata + 0u);
            height = read_u32_be(cdata + 4u);
            bit_depth = cdata[8];
            color_type = cdata[9];
            uint8_t comp = cdata[10];
            uint8_t flt = cdata[11];
            uint8_t interlace = cdata[12];
            if (width == 0u || height == 0u || width > PEBBLE_PNG_MAX_DIM || height > PEBBLE_PNG_MAX_DIM ||
                comp != 0u || flt != 0u || interlace != 0u) {
                return NULL;
            }
            have_ihdr = true;
        } else if (ctype[0] == 'P' && ctype[1] == 'L' && ctype[2] == 'T' && ctype[3] == 'E') {
            if ((clen % 3u) != 0u) return NULL;
            plte_count = clen / 3u;
            if (plte_count > 256u) plte_count = 256u;
            for (uint32_t i = 0u; i < plte_count * 3u; ++i) {
                plte_rgb[i] = cdata[i];
            }
        } else if (ctype[0] == 't' && ctype[1] == 'R' && ctype[2] == 'N' && ctype[3] == 'S') {
            if (color_type == 3u) {
                uint32_t n = (clen < 256u) ? clen : 256u;
                for (uint32_t i = 0u; i < n; ++i) {
                    trns_alpha[i] = cdata[i];
                }
            } else if (color_type == 0u && clen >= 2u) {
                has_trns_key = true;
                trns_r = read_u16_be(cdata);
            } else if (color_type == 2u && clen >= 6u) {
                has_trns_key = true;
                trns_r = read_u16_be(cdata + 0u);
                trns_g = read_u16_be(cdata + 2u);
                trns_b = read_u16_be(cdata + 4u);
            }
        } else if (ctype[0] == 'I' && ctype[1] == 'D' && ctype[2] == 'A' && ctype[3] == 'T') {
            if (idat_chunks == 0u) {
                single_idat = cdata;
                single_idat_len = clen;
            }
            idat_chunks++;
            total_idat_len += clen;
        } else if (ctype[0] == 'I' && ctype[1] == 'E' && ctype[2] == 'N' && ctype[3] == 'D') {
            break;
        }
    }

    if (!have_ihdr || idat_chunks == 0u || total_idat_len < 6u) {
        return NULL;
    }

    uint32_t channels = 0u;
    uint8_t dst_fmt = PEBBLE_GBITMAP_FORMAT_8BIT;
    if (color_type == 3u) {
        if (bit_depth != 1u && bit_depth != 2u && bit_depth != 4u && bit_depth != 8u) return NULL;
        channels = 1u;
        if (bit_depth == 1u) dst_fmt = PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE;
        else if (bit_depth == 2u) dst_fmt = PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE;
        else if (bit_depth == 4u) dst_fmt = PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE;
        else dst_fmt = PEBBLE_GBITMAP_FORMAT_8BIT;
    } else if (color_type == 0u) {
        if (bit_depth != 1u && bit_depth != 2u && bit_depth != 4u && bit_depth != 8u) return NULL;
        channels = 1u;
    } else if (color_type == 2u && bit_depth == 8u) {
        channels = 3u;
    } else if (color_type == 4u && bit_depth == 8u) {
        channels = 2u;
    } else if (color_type == 6u && bit_depth == 8u) {
        channels = 4u;
    } else {
        return NULL;
    }

    uint32_t raw_row_bytes = (width * channels * (uint32_t)bit_depth + 7u) / 8u;
    if (raw_row_bytes == 0u || raw_row_bytes > PEBBLE_PNG_MAX_ROW_BYTES) {
        return NULL;
    }

    pebble_gsize_t sz = {.w = (int16_t)width, .h = (int16_t)height};
    pebble_gbitmap_t *bmp = create_blank_internal(sz, dst_fmt, NULL, false, true);
    if (bmp == NULL) {
        return NULL;
    }

    png_sink_t sink;
    sink.width = width;
    sink.height = height;
    sink.bit_depth = bit_depth;
    sink.color_type = color_type;
    sink.raw_row_bytes = raw_row_bytes;
    sink.filter_bpp = (channels * (uint32_t)bit_depth + 7u) / 8u;
    if (sink.filter_bpp == 0u) sink.filter_bpp = 1u;
    sink.row_pos = 0u;
    sink.cur_y = 0u;
    sink.win_pos = 0u;
    sink.dst_bmp = bmp;
    sink.has_trns_key = has_trns_key;
    sink.trns_r = trns_r;
    sink.trns_g = trns_g;
    sink.trns_b = trns_b;
    sink.error = false;

    for (uint32_t i = 0u; i < 256u; ++i) {
        if (i < plte_count) {
            sink.plte_gcolor8[i] = rgba8_to_gcolor8(
                plte_rgb[i * 3u + 0u], plte_rgb[i * 3u + 1u], plte_rgb[i * 3u + 2u], trns_alpha[i]);
        } else {
            sink.plte_gcolor8[i] = PEBBLE_GCOLOR_CLEAR;
        }
    }
    uint16_t pal_n = palette_count_for_format(dst_fmt);
    if (pal_n > 0u && bmp->palette != NULL) {
        for (uint16_t i = 0u; i < pal_n; ++i) {
            bmp->palette[i].argb = sink.plte_gcolor8[i];
        }
    }

    for (uint32_t i = 0u; i < raw_row_bytes; ++i) {
        g_png_row_prev[i] = 0u;
    }

    const uint8_t *zbuf = single_idat;
    size_t zlen = single_idat_len;
    uint8_t *concat_idat = NULL;
    if (idat_chunks > 1u) {
        concat_idat = (uint8_t *)pebble_app_malloc(total_idat_len);
        if (concat_idat == NULL) {
            destroy_gbitmap_internal(bmp);
            return NULL;
        }
        size_t off = 0u;
        size_t p2 = 8u;
        while (p2 + 12u <= png_len) {
            uint32_t clen = read_u32_be(png_data + p2);
            const uint8_t *ctype = png_data + p2 + 4u;
            if (clen > png_len - (p2 + 12u)) {
                pebble_app_free(concat_idat);
                destroy_gbitmap_internal(bmp);
                return NULL;
            }
            const uint8_t *cdata = png_data + p2 + 8u;
            p2 += 12u + (size_t)clen;
            if (ctype[0] == 'I' && ctype[1] == 'D' && ctype[2] == 'A' && ctype[3] == 'T') {
                if (clen > total_idat_len - off) {
                    pebble_app_free(concat_idat);
                    destroy_gbitmap_internal(bmp);
                    return NULL;
                }
                for (uint32_t k = 0u; k < clen; ++k) {
                    concat_idat[off++] = cdata[k];
                }
            } else if (ctype[0] == 'I' && ctype[1] == 'E' && ctype[2] == 'N' && ctype[3] == 'D') {
                break;
            }
        }
        zbuf = concat_idat;
        zlen = off;
    }

    bool ok = inflate_zlib_to_png_sink(zbuf, zlen, &sink);
    if (concat_idat != NULL) {
        pebble_app_free(concat_idat);
    }
    if (!ok) {
        destroy_gbitmap_internal(bmp);
        return NULL;
    }
    return bmp;
}

typedef struct {
    uint16_t row_bytes;
    uint8_t format;
    pebble_grect_t bounds;
    uint16_t data_width;
    uint16_t data_height;
    uint32_t data_bytes;
    uint16_t palette_count;
} parsed_pbi_header_t;

static bool parse_pbi_header(const uint8_t hdr[12], parsed_pbi_header_t *out) {
    uint16_t row_bytes = pebble_read_u16_le(hdr + 0u);
    uint16_t info_flags = pebble_read_u16_le(hdr + 2u);
    int16_t ox = (int16_t)pebble_read_u16_le(hdr + 4u);
    int16_t oy = (int16_t)pebble_read_u16_le(hdr + 6u);
    int16_t w = (int16_t)pebble_read_u16_le(hdr + 8u);
    int16_t h = (int16_t)pebble_read_u16_le(hdr + 10u);
    if (row_bytes == 0u || ox < 0 || oy < 0 || w <= 0 || h <= 0) {
        return false;
    }
    uint32_t total_w = (uint32_t)ox + (uint32_t)w;
    uint32_t total_h = (uint32_t)oy + (uint32_t)h;
    if (total_w > 0xFFFFu || total_h > 0xFFFFu) {
        return false;
    }
    uint8_t fmt = format_from_info_flags(info_flags);
    uint16_t min_row = pebble_gbitmap_row_bytes_for_format((uint16_t)total_w, fmt);
    if (min_row == 0u || row_bytes < min_row) {
        return false;
    }
    if ((uint32_t)row_bytes > 0xFFFFFFFFu / total_h) {
        return false;
    }
    out->row_bytes = row_bytes;
    out->format = fmt;
    out->bounds.origin.x = ox;
    out->bounds.origin.y = oy;
    out->bounds.size.w = w;
    out->bounds.size.h = h;
    out->data_width = (uint16_t)total_w;
    out->data_height = (uint16_t)total_h;
    out->data_bytes = (uint32_t)row_bytes * total_h;
    out->palette_count = palette_count_for_format(fmt);
    return true;
}

static pebble_gbitmap_t *decode_pbi_from_resource(uint32_t abs_offset, uint32_t length) {
    if (length < 12u) {
        return NULL;
    }
    uint8_t hdr[12];
    if (pebble_res_read_abs(abs_offset, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return NULL;
    }
    parsed_pbi_header_t ph;
    if (!parse_pbi_header(hdr, &ph)) {
        return NULL;
    }
    if (ph.data_bytes > length - 12u || (uint32_t)ph.palette_count > (length - 12u) - ph.data_bytes) {
        return NULL;
    }

    pebble_gbitmap_t *bmp = (pebble_gbitmap_t *)pebble_app_calloc(1u, sizeof(pebble_gbitmap_t));
    if (bmp == NULL) {
        return NULL;
    }
    bmp->addr = (uint8_t *)pebble_app_malloc(ph.data_bytes);
    if (bmp->addr == NULL) {
        pebble_app_free(bmp);
        return NULL;
    }
    if (pebble_res_read_abs(abs_offset + 12u, bmp->addr, ph.data_bytes) != ph.data_bytes) {
        pebble_app_free(bmp->addr);
        pebble_app_free(bmp);
        return NULL;
    }
    bmp->row_size_bytes = ph.row_bytes;
    bmp->bounds = ph.bounds;
    bmp->data_width = ph.data_width;
    bmp->data_height = ph.data_height;
    bmp->owns_data = true;

    if (ph.palette_count > 0u) {
        bmp->palette = (pebble_gcolor8_t *)pebble_app_malloc(ph.palette_count);
        if (bmp->palette == NULL) {
            pebble_app_free(bmp->addr);
            pebble_app_free(bmp);
            return NULL;
        }
        if (pebble_res_read_abs(abs_offset + 12u + ph.data_bytes, bmp->palette, ph.palette_count) !=
            ph.palette_count) {
            pebble_app_free(bmp->palette);
            pebble_app_free(bmp->addr);
            pebble_app_free(bmp);
            return NULL;
        }
        bmp->owns_palette = true;
    }
    bmp->info_flags = pebble_gbitmap_make_info_flags(ph.format, bmp->owns_data, bmp->owns_palette);
    return bmp;
}

/* --- GBitmap blitting --- */

void pebble_bitmap_draw_in_rect(
    pebble_gcontext_t *ctx,
    const pebble_gbitmap_t *bitmap,
    pebble_grect_t rect) {
    if (ctx == NULL || bitmap == NULL || bitmap->addr == NULL ||
        rect.size.w <= 0 || rect.size.h <= 0 ||
        bitmap->bounds.size.w <= 0 || bitmap->bounds.size.h <= 0) {
        return;
    }

    uint8_t fmt = format_from_info_flags(bitmap->info_flags);
    uint8_t comp = pebble_gfx_get_compositing_mode(ctx);
    int32_t bx = bitmap->bounds.origin.x;
    int32_t by = bitmap->bounds.origin.y;
    int32_t bw = bitmap->bounds.size.w;
    int32_t bh = bitmap->bounds.size.h;

    for (int32_t dy = 0; dy < rect.size.h; ++dy) {
        int32_t sy = by + (dy % bh);
        if (sy < 0 || sy >= (int32_t)bitmap->data_height) {
            continue;
        }
        const uint8_t *row = bitmap->addr + (size_t)sy * (size_t)bitmap->row_size_bytes;
        for (int32_t dx = 0; dx < rect.size.w; ++dx) {
            int32_t sx = bx + (dx % bw);
            if (sx < 0 || sx >= (int32_t)bitmap->data_width) {
                continue;
            }
            int32_t px = rect.origin.x + dx;
            int32_t py = rect.origin.y + dy;

            if (fmt == PEBBLE_GBITMAP_FORMAT_1BIT) {
                uint8_t bit = (uint8_t)((row[sx >> 3] >> (sx & 7)) & 1u);
                switch (comp) {
                    case PEBBLE_GCOMP_OP_ASSIGN:
                        pebble_gfx_plot_local(ctx, px, py, bit ? PEBBLE_GCOLOR_WHITE : PEBBLE_GCOLOR_BLACK);
                        break;
                    case PEBBLE_GCOMP_OP_ASSIGN_INVERTED:
                        pebble_gfx_plot_local(ctx, px, py, bit ? PEBBLE_GCOLOR_BLACK : PEBBLE_GCOLOR_WHITE);
                        break;
                    case PEBBLE_GCOMP_OP_OR:
                        if (bit) pebble_gfx_plot_local(ctx, px, py, PEBBLE_GCOLOR_WHITE);
                        break;
                    case PEBBLE_GCOMP_OP_AND:
                        if (!bit) pebble_gfx_plot_local(ctx, px, py, PEBBLE_GCOLOR_BLACK);
                        break;
                    case PEBBLE_GCOMP_OP_CLEAR:
                        if (bit) pebble_gfx_plot_local(ctx, px, py, PEBBLE_GCOLOR_BLACK);
                        break;
                    case PEBBLE_GCOMP_OP_SET:
                        if (!bit) pebble_gfx_plot_local(ctx, px, py, PEBBLE_GCOLOR_WHITE);
                        break;
                    default:
                        pebble_gfx_plot_local(ctx, px, py, bit ? PEBBLE_GCOLOR_WHITE : PEBBLE_GCOLOR_BLACK);
                        break;
                }
            } else {
                uint8_t argb = PEBBLE_GCOLOR_CLEAR;
                if (fmt == PEBBLE_GBITMAP_FORMAT_8BIT || fmt == PEBBLE_GBITMAP_FORMAT_8BIT_CIRCULAR) {
                    argb = row[sx];
                } else if (fmt == PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE) {
                    uint8_t idx = (uint8_t)((row[sx >> 3] >> (7 - (sx & 7))) & 1u);
                    argb = (bitmap->palette != NULL)
                               ? bitmap->palette[idx].argb
                               : (idx ? PEBBLE_GCOLOR_WHITE : PEBBLE_GCOLOR_BLACK);
                } else if (fmt == PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE) {
                    uint8_t idx = (uint8_t)((row[sx >> 2] >> (6 - 2 * (sx & 3))) & 3u);
                    argb = (bitmap->palette != NULL) ? bitmap->palette[idx].argb : PEBBLE_GCOLOR_CLEAR;
                } else if (fmt == PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE) {
                    uint8_t idx = (uint8_t)((row[sx >> 1] >> (4 - 4 * (sx & 1))) & 0x0Fu);
                    argb = (bitmap->palette != NULL) ? bitmap->palette[idx].argb : PEBBLE_GCOLOR_CLEAR;
                }

                if (comp == PEBBLE_GCOMP_OP_ASSIGN) {
                    /* GCompOpAssign ignores alpha on color bitmaps and writes opaque pixel. */
                    pebble_gfx_plot_local(ctx, px, py, (uint8_t)(0xC0u | (argb & 0x3Fu)));
                } else {
                    /* GCompOpSet respects 2-bit alpha transparency and blending. */
                    pebble_gfx_plot_local(ctx, px, py, argb);
                }
            }
        }
    }
}

/* --- PebbleOS GBitmap & Color Syscall Shims --- */

static pebble_gbitmap_t *shim_gbitmap_create_with_resource(uint32_t resource_id) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_WITH_RESOURCE, true);
    uint32_t abs_off = 0u;
    uint32_t length = 0u;
    if (!pebble_res_lookup_id(resource_id, &abs_off, &length) || length < 8u) {
        return NULL;
    }
    uint8_t magic[8];
    if (pebble_res_read_abs(abs_off, magic, sizeof(magic)) != sizeof(magic)) {
        return NULL;
    }
    if (is_png_signature(magic, sizeof(magic))) {
        uint8_t *buf = (uint8_t *)pebble_app_malloc(length);
        if (buf == NULL) {
            return NULL;
        }
        pebble_gbitmap_t *bmp = NULL;
        if (pebble_res_read_abs(abs_off, buf, length) == length) {
            bmp = decode_png_bytes(buf, length);
        }
        pebble_app_free(buf);
        return bmp;
    }
    return decode_pbi_from_resource(abs_off, length);
}

static pebble_gbitmap_t *shim_gbitmap_create_from_png_data(const uint8_t *png_data, size_t png_data_size) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_FROM_PNG_DATA, true);
    return decode_png_bytes(png_data, png_data_size);
}

static pebble_gbitmap_t *shim_gbitmap_create_with_data(const uint8_t *data) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_WITH_DATA, true);
    if (data == NULL) {
        return NULL;
    }
    parsed_pbi_header_t ph;
    if (!parse_pbi_header(data, &ph)) {
        return NULL;
    }
    pebble_gbitmap_t *bmp = (pebble_gbitmap_t *)pebble_app_calloc(1u, sizeof(pebble_gbitmap_t));
    if (bmp == NULL) {
        return NULL;
    }
    bmp->addr = (uint8_t *)(data + 12u);
    bmp->row_size_bytes = ph.row_bytes;
    bmp->bounds = ph.bounds;
    bmp->data_width = ph.data_width;
    bmp->data_height = ph.data_height;
    bmp->owns_data = false;
    bmp->owns_palette = false;
    if (ph.palette_count > 0u) {
        bmp->palette = (pebble_gcolor8_t *)(data + 12u + ph.data_bytes);
    }
    bmp->info_flags = pebble_gbitmap_make_info_flags(ph.format, false, false);
    return bmp;
}

static pebble_gbitmap_t *shim_gbitmap_create_as_sub_bitmap(
    const pebble_gbitmap_t *base_bitmap,
    pebble_grect_t sub_rect) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_AS_SUB_BITMAP, true);
    if (base_bitmap == NULL) {
        return NULL;
    }
    pebble_gbitmap_t *sub = (pebble_gbitmap_t *)pebble_app_calloc(1u, sizeof(pebble_gbitmap_t));
    if (sub == NULL) {
        return NULL;
    }
    uint8_t fmt = format_from_info_flags(base_bitmap->info_flags);
    sub->addr = base_bitmap->addr;
    sub->row_size_bytes = base_bitmap->row_size_bytes;
    sub->bounds = pebble_rect_intersect(base_bitmap->bounds, sub_rect);
    sub->palette = base_bitmap->palette;
    sub->data_width = base_bitmap->data_width;
    sub->data_height = base_bitmap->data_height;
    sub->owns_data = false;
    sub->owns_palette = false;
    sub->info_flags = pebble_gbitmap_make_info_flags(fmt, false, false);
    return sub;
}

static pebble_gbitmap_t *shim_gbitmap_create_blank(pebble_gsize_t size, uint8_t format) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_BLANK, true);
    return create_blank_internal(size, format, NULL, false, true);
}

static pebble_gbitmap_t *shim_gbitmap_create_blank_2bit(pebble_gsize_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_BLANK_2BIT, true);
    return create_blank_internal(size, PEBBLE_GBITMAP_FORMAT_1BIT, NULL, false, false);
}

static pebble_gbitmap_t *shim_gbitmap_create_blank_with_palette(
    pebble_gsize_t size,
    uint8_t format,
    pebble_gcolor8_t *palette,
    bool free_on_destroy) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_BLANK_WITH_PALETTE, true);
    return create_blank_internal(size, format, palette, free_on_destroy, false);
}

static pebble_gbitmap_t *shim_gbitmap_create_palettized_from_1bit(const pebble_gbitmap_t *src) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_CREATE_PALETTIZED_FROM_1BIT, true);
    if (src == NULL || src->addr == NULL ||
        format_from_info_flags(src->info_flags) != PEBBLE_GBITMAP_FORMAT_1BIT) {
        return NULL;
    }
    pebble_gbitmap_t *dst =
        create_blank_internal(src->bounds.size, PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE, NULL, false, true);
    if (dst == NULL) {
        return NULL;
    }
    dst->palette[0].argb = PEBBLE_GCOLOR_BLACK;
    dst->palette[1].argb = PEBBLE_GCOLOR_WHITE;
    for (int32_t y = 0; y < src->bounds.size.h; ++y) {
        int32_t sy = src->bounds.origin.y + y;
        if (sy < 0 || sy >= (int32_t)src->data_height) {
            continue;
        }
        const uint8_t *srow = src->addr + (size_t)sy * (size_t)src->row_size_bytes;
        uint8_t *drow = dst->addr + (size_t)y * (size_t)dst->row_size_bytes;
        for (int32_t x = 0; x < src->bounds.size.w; ++x) {
            int32_t sx = src->bounds.origin.x + x;
            if (sx < 0 || sx >= (int32_t)src->data_width) {
                continue;
            }
            uint8_t bit = (uint8_t)((srow[sx >> 3] >> (sx & 7)) & 1u);
            if (bit) {
                drow[x >> 3] |= (uint8_t)(1u << (7 - (x & 7)));
            }
        }
    }
    return dst;
}

static void shim_gbitmap_destroy(pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_DESTROY, true);
    destroy_gbitmap_internal(bitmap);
}

static uint16_t shim_gbitmap_get_bytes_per_row(const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_BYTES_PER_ROW, true);
    return (bitmap != NULL) ? bitmap->row_size_bytes : 0u;
}

static uint8_t shim_gbitmap_get_format(const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_FORMAT, true);
    return (bitmap != NULL) ? format_from_info_flags(bitmap->info_flags) : PEBBLE_GBITMAP_FORMAT_1BIT;
}

static uint8_t *shim_gbitmap_get_data(const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_DATA, true);
    return (bitmap != NULL) ? bitmap->addr : NULL;
}

static void shim_gbitmap_set_data(
    pebble_gbitmap_t *bitmap,
    uint8_t *data,
    uint8_t format,
    uint16_t row_size_bytes,
    bool free_on_destroy) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_SET_DATA, true);
    if (bitmap == NULL) {
        return;
    }
    if (bitmap->owns_data && bitmap->addr != NULL && bitmap->addr != data) {
        pebble_app_free(bitmap->addr);
    }
    bitmap->addr = data;
    bitmap->row_size_bytes = row_size_bytes;
    bitmap->owns_data = free_on_destroy;
    bitmap->info_flags = pebble_gbitmap_make_info_flags(format, bitmap->owns_data, bitmap->owns_palette);
    int32_t end_x = (int32_t)bitmap->bounds.origin.x + (int32_t)bitmap->bounds.size.w;
    int32_t end_y = (int32_t)bitmap->bounds.origin.y + (int32_t)bitmap->bounds.size.h;
    if (end_x > 0 && end_x <= 0xFFFF) bitmap->data_width = (uint16_t)end_x;
    if (end_y > 0 && end_y <= 0xFFFF) bitmap->data_height = (uint16_t)end_y;
}

static pebble_grect_t shim_gbitmap_get_bounds(const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_BOUNDS, true);
    pebble_grect_t empty = {{0, 0}, {0, 0}};
    return (bitmap != NULL) ? bitmap->bounds : empty;
}

static void shim_gbitmap_set_bounds(pebble_gbitmap_t *bitmap, pebble_grect_t bounds) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_SET_BOUNDS, true);
    if (bitmap != NULL) {
        bitmap->bounds = bounds;
    }
}

static pebble_gcolor8_t *shim_gbitmap_get_palette(const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_PALETTE, true);
    return (bitmap != NULL) ? bitmap->palette : NULL;
}

static void shim_gbitmap_set_palette(
    pebble_gbitmap_t *bitmap,
    pebble_gcolor8_t *palette,
    bool free_on_destroy) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_SET_PALETTE, true);
    if (bitmap == NULL) {
        return;
    }
    if (bitmap->owns_palette && bitmap->palette != NULL && bitmap->palette != palette) {
        pebble_app_free(bitmap->palette);
    }
    bitmap->palette = palette;
    bitmap->owns_palette = free_on_destroy;
    bitmap->info_flags = pebble_gbitmap_make_info_flags(
        format_from_info_flags(bitmap->info_flags), bitmap->owns_data, bitmap->owns_palette);
}

static int32_t isqrt_i32(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v;
    int32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static pebble_gbitmap_data_row_info_t shim_gbitmap_get_data_row_info(
    const pebble_gbitmap_t *bitmap,
    uint16_t y) {
    pebble_rt_record_syscall(PEBBLE_SYS_GBITMAP_GET_DATA_ROW_INFO, true);
    pebble_gbitmap_data_row_info_t info = {.data = NULL, .min_x = 0, .max_x = -1};
    if (bitmap == NULL || bitmap->addr == NULL || y >= bitmap->data_height || bitmap->data_width == 0u) {
        return info;
    }
    info.data = bitmap->addr + (size_t)y * (size_t)bitmap->row_size_bytes;
    uint8_t fmt = format_from_info_flags(bitmap->info_flags);
    if (fmt == PEBBLE_GBITMAP_FORMAT_8BIT_CIRCULAR && bitmap->data_width == bitmap->data_height) {
        int32_t r2 = (int32_t)bitmap->data_width; /* diameter = 2*R; use doubled coords to stay integer */
        int32_t dy2 = 2 * (int32_t)y + 1 - r2;
        int32_t disc = r2 * r2 - dy2 * dy2;
        int32_t half_chord2 = isqrt_i32(disc);
        int32_t min_x = (r2 - half_chord2) / 2;
        int32_t max_x = (r2 + half_chord2) / 2 - 1;
        if (min_x < 0) min_x = 0;
        if (max_x >= (int32_t)bitmap->data_width) max_x = (int32_t)bitmap->data_width - 1;
        info.min_x = (int16_t)min_x;
        info.max_x = (int16_t)max_x;
        return info;
    }
    info.min_x = 0;
    info.max_x = (int16_t)(bitmap->data_width - 1u);
    return info;
}

static void shim_graphics_draw_bitmap_in_rect(
    pebble_gcontext_t *ctx,
    const pebble_gbitmap_t *bitmap,
    pebble_grect_t rect) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_BITMAP_IN_RECT, true);
    pebble_bitmap_draw_in_rect(ctx, bitmap, rect);
}

void pebble_bitmap_register_syscalls(void **table) {
    table[PEBBLE_SYS_GBITMAP_CREATE_WITH_RESOURCE] = (void *)shim_gbitmap_create_with_resource;
    table[PEBBLE_SYS_GBITMAP_CREATE_FROM_PNG_DATA] = (void *)shim_gbitmap_create_from_png_data;
    table[PEBBLE_SYS_GBITMAP_CREATE_WITH_DATA] = (void *)shim_gbitmap_create_with_data;
    table[PEBBLE_SYS_GBITMAP_CREATE_AS_SUB_BITMAP] = (void *)shim_gbitmap_create_as_sub_bitmap;
    table[PEBBLE_SYS_GBITMAP_CREATE_BLANK] = (void *)shim_gbitmap_create_blank;
    table[PEBBLE_SYS_GBITMAP_CREATE_BLANK_2BIT] = (void *)shim_gbitmap_create_blank_2bit;
    table[PEBBLE_SYS_GBITMAP_CREATE_BLANK_WITH_PALETTE] = (void *)shim_gbitmap_create_blank_with_palette;
    table[PEBBLE_SYS_GBITMAP_CREATE_PALETTIZED_FROM_1BIT] = (void *)shim_gbitmap_create_palettized_from_1bit;
    table[PEBBLE_SYS_GBITMAP_DESTROY] = (void *)shim_gbitmap_destroy;
    table[PEBBLE_SYS_GBITMAP_GET_BYTES_PER_ROW] = (void *)shim_gbitmap_get_bytes_per_row;
    table[PEBBLE_SYS_GBITMAP_GET_FORMAT] = (void *)shim_gbitmap_get_format;
    table[PEBBLE_SYS_GBITMAP_GET_DATA] = (void *)shim_gbitmap_get_data;
    table[PEBBLE_SYS_GBITMAP_SET_DATA] = (void *)shim_gbitmap_set_data;
    table[PEBBLE_SYS_GBITMAP_GET_BOUNDS] = (void *)shim_gbitmap_get_bounds;
    table[PEBBLE_SYS_GBITMAP_SET_BOUNDS] = (void *)shim_gbitmap_set_bounds;
    table[PEBBLE_SYS_GBITMAP_GET_PALETTE] = (void *)shim_gbitmap_get_palette;
    table[PEBBLE_SYS_GBITMAP_SET_PALETTE] = (void *)shim_gbitmap_set_palette;
    table[PEBBLE_SYS_GBITMAP_GET_DATA_ROW_INFO] = (void *)shim_gbitmap_get_data_row_info;
    table[PEBBLE_SYS_GRAPHICS_DRAW_BITMAP_IN_RECT] = (void *)shim_graphics_draw_bitmap_in_rect;
}
