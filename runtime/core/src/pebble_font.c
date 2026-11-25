#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_font_dogica.h"
#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_FONT_MAGIC 0x50464F46u
#define PEBBLE_MAX_CUSTOM_FONTS 16u
#define PEBBLE_PFO_FEAT_OFFSET_16 0x01u
#define PEBBLE_PFO_FEAT_RLE4 0x02u
#define PEBBLE_MAX_GLYPH_BYTES 1024u

typedef struct {
    uint32_t magic;
    bool in_use;
    uint32_t res_abs_offset;
    uint32_t res_length;
    uint8_t version;
    uint8_t max_height;
    uint16_t num_glyphs;
    uint16_t wildcard_cp;
    uint8_t hash_table_size;
    uint8_t codepoint_bytes;
    uint8_t offset_bytes;
    uint8_t features;
    uint32_t ht_offset;
    uint32_t ot_offset;
    uint32_t gt_offset;
    uint32_t entry_size;
} pebble_pfo_font_t;

typedef struct {
    uint8_t width;
    uint8_t height_or_units;
    int8_t offset_left;
    int8_t offset_top;
    int8_t horiz_advance;
    uint32_t data_abs_offset;
    uint32_t data_avail;
} pfo_glyph_info_t;

static pebble_pfo_font_t g_custom_fonts[PEBBLE_MAX_CUSTOM_FONTS];

void pebble_font_reset(void) {
    for (uint32_t i = 0u; i < PEBBLE_MAX_CUSTOM_FONTS; ++i) {
        g_custom_fonts[i].magic = 0u;
        g_custom_fonts[i].in_use = false;
    }
}

static const pebble_pfo_font_t *as_custom_font(pebble_gfont_t font) {
    uintptr_t p = (uintptr_t)font;
    uintptr_t lo = (uintptr_t)&g_custom_fonts[0];
    uintptr_t hi = (uintptr_t)&g_custom_fonts[PEBBLE_MAX_CUSTOM_FONTS];
    if (p < lo || p >= hi) {
        return NULL;
    }
    if (((p - lo) % sizeof(pebble_pfo_font_t)) != 0u) {
        return NULL;
    }
    const pebble_pfo_font_t *f = (const pebble_pfo_font_t *)font;
    if (!f->in_use || f->magic != PEBBLE_FONT_MAGIC) {
        return NULL;
    }
    return f;
}

static uint8_t dogica_scale_for_handle(pebble_gfont_t font) {
    uintptr_t tag = (uintptr_t)font;
    if (tag == (uintptr_t)PEBBLE_FONT_SCALE_2X) {
        return 2u;
    }
    if (tag == (uintptr_t)PEBBLE_FONT_SCALE_3X) {
        return 3u;
    }
    return 1u;
}

/* Decode one UTF-8 codepoint and advance *pp; returns 0 at NUL terminator. */
static uint32_t utf8_next(const char **pp) {
    const uint8_t *s = (const uint8_t *)(*pp);
    uint8_t b0 = s[0];
    if (b0 == 0u) {
        return 0u;
    }
    if (b0 < 0x80u) {
        *pp += 1;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *pp += 2;
        return ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((b0 & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *pp += 3;
        return ((uint32_t)(b0 & 0x0Fu) << 12) |
               ((uint32_t)(s[1] & 0x3Fu) << 6) |
               (uint32_t)(s[2] & 0x3Fu);
    }
    if ((b0 & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *pp += 4;
        return ((uint32_t)(b0 & 0x07u) << 18) |
               ((uint32_t)(s[1] & 0x3Fu) << 12) |
               ((uint32_t)(s[2] & 0x3Fu) << 6) |
               (uint32_t)(s[3] & 0x3Fu);
    }
    *pp += 1;
    return 0xFFFDu;
}

/* --- Pebble .pfo lookup & glyph decoding --- */

static bool pfo_lookup_glyph_offset(const pebble_pfo_font_t *f, uint32_t cp, uint32_t *glyph_rel_out) {
    if (f->version == 1u) {
        uint32_t lo = 0u;
        uint32_t hi = f->num_glyphs;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            uint8_t ent[4];
            if (pebble_res_read_abs(f->res_abs_offset + f->ot_offset + mid * 4u, ent, 4u) != 4u) {
                return false;
            }
            uint32_t ecp = pebble_read_u16_le(ent + 0u);
            if (ecp == cp) {
                *glyph_rel_out = (uint32_t)pebble_read_u16_le(ent + 2u) * 4u;
                return true;
            }
            if (ecp < cp) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        return false;
    }

    if (f->hash_table_size == 0u) {
        return false;
    }
    uint32_t bucket_idx = cp % (uint32_t)f->hash_table_size;
    uint8_t hraw[4];
    if (pebble_res_read_abs(f->res_abs_offset + f->ht_offset + bucket_idx * 4u, hraw, 4u) != 4u) {
        return false;
    }
    uint8_t count = hraw[1];
    uint16_t bucket_off = pebble_read_u16_le(hraw + 2u);
    if (count == 0u) {
        return false;
    }

    uint8_t ent[8];
    if (f->entry_size > sizeof(ent)) {
        return false;
    }
    for (uint32_t i = 0u; i < (uint32_t)count; ++i) {
        uint32_t rel = f->ot_offset + (uint32_t)bucket_off + i * f->entry_size;
        if (rel > f->res_length || f->entry_size > f->res_length - rel) {
            return false;
        }
        if (pebble_res_read_abs(f->res_abs_offset + rel, ent, f->entry_size) != f->entry_size) {
            return false;
        }
        uint32_t ecp = (f->codepoint_bytes == 2u) ? (uint32_t)pebble_read_u16_le(ent) : pebble_read_u32_le(ent);
        if (ecp == cp) {
            const uint8_t *op = ent + f->codepoint_bytes;
            *glyph_rel_out = (f->offset_bytes == 2u) ? (uint32_t)pebble_read_u16_le(op) : pebble_read_u32_le(op);
            return true;
        }
    }
    return false;
}

static bool pfo_read_glyph(const pebble_pfo_font_t *f, uint32_t cp, pfo_glyph_info_t *out) {
    uint32_t glyph_rel = 0u;
    if (!pfo_lookup_glyph_offset(f, cp, &glyph_rel)) {
        if (cp == f->wildcard_cp || !pfo_lookup_glyph_offset(f, f->wildcard_cp, &glyph_rel)) {
            return false;
        }
    }
    uint32_t hdr_bytes = (f->version == 1u) ? 8u : 5u;
    if (glyph_rel > f->res_length ||
        f->gt_offset > f->res_length - glyph_rel ||
        hdr_bytes > f->res_length - f->gt_offset - glyph_rel) {
        return false;
    }
    uint32_t pos = f->gt_offset + glyph_rel;
    uint8_t ghdr[8];
    if (pebble_res_read_abs(f->res_abs_offset + pos, ghdr, hdr_bytes) != hdr_bytes) {
        return false;
    }
    out->width = ghdr[0];
    out->height_or_units = ghdr[1];
    out->offset_left = (int8_t)ghdr[2];
    out->offset_top = (int8_t)ghdr[3];
    out->horiz_advance = (int8_t)ghdr[4];
    out->data_abs_offset = f->res_abs_offset + pos + hdr_bytes;
    out->data_avail = f->res_length - (pos + hdr_bytes);
    return true;
}

static int32_t font_line_height(pebble_gfont_t font) {
    const pebble_pfo_font_t *pfo = as_custom_font(font);
    if (pfo != NULL) {
        return (pfo->max_height > 0u) ? (int32_t)pfo->max_height : 1;
    }
    return (int32_t)PEBBLE_DOGICA_Y_ADVANCE * (int32_t)dogica_scale_for_handle(font);
}

static int32_t font_glyph_advance(pebble_gfont_t font, uint32_t cp) {
    const pebble_pfo_font_t *pfo = as_custom_font(font);
    if (pfo != NULL) {
        pfo_glyph_info_t g;
        if (pfo_read_glyph(pfo, cp, &g)) {
            return (int32_t)g.horiz_advance;
        }
        return (int32_t)(pfo->max_height / 2u);
    }
    uint8_t scale = dogica_scale_for_handle(font);
    uint8_t ch = (cp >= PEBBLE_DOGICA_FIRST && cp <= PEBBLE_DOGICA_LAST) ? (uint8_t)cp : (uint8_t)' ';
    const pebble_dogica_glyph_t *dg = &pebble_dogica_glyphs[ch - PEBBLE_DOGICA_FIRST];
    return (int32_t)dg->x_advance * (int32_t)scale;
}

static void draw_pfo_glyph(
    const pebble_gcontext_t *ctx,
    const pebble_pfo_font_t *pfo,
    uint32_t cp,
    int32_t pen_x,
    int32_t line_y,
    uint8_t argb) {
    pfo_glyph_info_t g;
    if (!pfo_read_glyph(pfo, cp, &g) || g.width == 0u || g.height_or_units == 0u) {
        return;
    }

    int32_t base_x = pen_x + (int32_t)g.offset_left;
    int32_t base_y = line_y + (int32_t)g.offset_top;
    uint8_t buf[PEBBLE_MAX_GLYPH_BYTES];

    if ((pfo->features & PEBBLE_PFO_FEAT_RLE4) != 0u) {
        uint32_t units = g.height_or_units;
        uint32_t need = (units + 1u) / 2u;
        if (need > sizeof(buf) || need > g.data_avail) {
            return;
        }
        if (pebble_res_read_abs(g.data_abs_offset, buf, need) != need) {
            return;
        }
        uint32_t px_idx = 0u;
        for (uint32_t u = 0u; u < units; ++u) {
            uint8_t byte = buf[u >> 1];
            uint8_t nib = ((u & 1u) == 0u) ? (uint8_t)(byte & 0x0Fu) : (uint8_t)((byte >> 4) & 0x0Fu);
            uint8_t sym = (uint8_t)((nib >> 3) & 1u);
            uint32_t run = (uint32_t)(nib & 0x07u) + 1u;
            for (uint32_t r = 0u; r < run; ++r) {
                if (sym != 0u) {
                    int32_t xx = (int32_t)(px_idx % g.width);
                    int32_t yy = (int32_t)(px_idx / g.width);
                    pebble_gfx_plot_local(ctx, base_x + xx, base_y + yy, argb);
                }
                px_idx++;
            }
        }
        return;
    }

    uint32_t total_bits = (uint32_t)g.width * (uint32_t)g.height_or_units;
    uint32_t need = (total_bits + 7u) / 8u;
    if (need > sizeof(buf) || need > g.data_avail) {
        return;
    }
    if (pebble_res_read_abs(g.data_abs_offset, buf, need) != need) {
        return;
    }
    for (uint32_t yy = 0u; yy < g.height_or_units; ++yy) {
        for (uint32_t xx = 0u; xx < g.width; ++xx) {
            uint32_t bit_idx = yy * (uint32_t)g.width + xx;
            if (((buf[bit_idx >> 3] >> (bit_idx & 7u)) & 1u) != 0u) {
                pebble_gfx_plot_local(ctx, base_x + (int32_t)xx, base_y + (int32_t)yy, argb);
            }
        }
    }
}

static void draw_dogica_glyph(
    const pebble_gcontext_t *ctx,
    uint32_t cp,
    int32_t pen_x,
    int32_t line_y,
    uint8_t scale,
    uint8_t argb) {
    if (cp < PEBBLE_DOGICA_FIRST || cp > PEBBLE_DOGICA_LAST) {
        return;
    }
    int32_t baseline_y = line_y + 7 * (int32_t)scale;
    const pebble_dogica_glyph_t *g = &pebble_dogica_glyphs[cp - PEBBLE_DOGICA_FIRST];
    uint32_t bo = g->bitmap_offset;
    uint8_t bits = 0u;
    uint8_t bit = 0u;
    for (uint8_t yy = 0u; yy < g->height; ++yy) {
        for (uint8_t xx = 0u; xx < g->width; ++xx) {
            if (bit == 0u) {
                bits = pebble_dogica_bitmaps[bo++];
                bit = 0x80u;
            }
            if ((bits & bit) != 0u) {
                int32_t px = pen_x + ((int32_t)g->x_offset + (int32_t)xx) * (int32_t)scale;
                int32_t py = baseline_y + ((int32_t)g->y_offset + (int32_t)yy) * (int32_t)scale;
                for (uint8_t sy = 0u; sy < scale; ++sy) {
                    for (uint8_t sx = 0u; sx < scale; ++sx) {
                        pebble_gfx_plot_local(ctx, px + (int32_t)sx, py + (int32_t)sy, argb);
                    }
                }
            }
            bit >>= 1;
        }
    }
}

/* Shared UTF-8 word-wrapping line breaker for both drawing and layout measurement. */
static const char *next_line_span(
    const char *start,
    pebble_gfont_t font,
    int32_t max_w,
    int32_t *line_w_out,
    const char **next_start_out) {
    const char *p = start;
    const char *last_space_end = NULL;
    int32_t width_px = 0;
    int32_t width_at_space = 0;

    while (*p != '\0' && *p != '\n') {
        const char *cp_start = p;
        uint32_t cp = utf8_next(&p);
        int32_t adv = font_glyph_advance(font, cp);
        if (cp_start > start && width_px + adv > max_w) {
            if (last_space_end != NULL) {
                *line_w_out = width_at_space;
                *next_start_out = last_space_end + 1; /* skip space */
                return last_space_end;
            }
            *line_w_out = width_px;
            *next_start_out = cp_start;
            return cp_start;
        }
        if (cp == ' ') {
            last_space_end = cp_start;
            width_at_space = width_px;
        }
        width_px += adv;
    }

    *line_w_out = width_px;
    if (*p == '\n') {
        *next_start_out = p + 1;
    } else {
        *next_start_out = p;
    }
    return p;
}

void pebble_font_draw_text(
    pebble_gcontext_t *ctx,
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment) {
    (void)overflow_mode;
    if (ctx == NULL || text == NULL || box.size.w <= 0 || box.size.h <= 0) {
        return;
    }

    const pebble_pfo_font_t *pfo = as_custom_font(font);
    uint8_t scale = dogica_scale_for_handle(font);
    int32_t line_h = font_line_height(font);
    uint8_t text_argb = pebble_gfx_get_text_color(ctx).argb;

    pebble_grect_t saved_clip = pebble_gfx_push_box_clip(ctx, box);

    int32_t line_y = box.origin.y;
    const char *p = text;
    while (*p != '\0' && line_y < (box.origin.y + box.size.h)) {
        int32_t line_w = 0;
        const char *next_p = NULL;
        const char *line_end = next_line_span(p, font, box.size.w, &line_w, &next_p);

        int32_t pen_x = box.origin.x;
        if (alignment == PEBBLE_TEXT_ALIGN_CENTER) {
            pen_x = box.origin.x + (box.size.w - line_w) / 2;
        } else if (alignment == PEBBLE_TEXT_ALIGN_RIGHT) {
            pen_x = box.origin.x + (box.size.w - line_w);
        }

        const char *cur = p;
        while (cur < line_end) {
            uint32_t cp = utf8_next(&cur);
            if (pfo != NULL) {
                draw_pfo_glyph(ctx, pfo, cp, pen_x, line_y, text_argb);
            } else {
                draw_dogica_glyph(ctx, cp, pen_x, line_y, scale, text_argb);
            }
            pen_x += font_glyph_advance(font, cp);
        }

        if (next_p == p) {
            break;
        }
        p = next_p;
        line_y += line_h;
    }

    pebble_gfx_restore_clip(ctx, saved_clip);
}

pebble_gsize_t pebble_font_measure_text(const char *text, pebble_gfont_t font, pebble_grect_t box) {
    pebble_gsize_t out = {0, 0};
    if (text == NULL || box.size.w <= 0 || box.size.h <= 0) {
        return out;
    }
    int32_t line_h = font_line_height(font);
    int32_t max_w = 0;
    int32_t total_h = 0;
    const char *p = text;
    while (*p != '\0' && total_h < box.size.h) {
        int32_t lw = 0;
        const char *next_p = NULL;
        (void)next_line_span(p, font, box.size.w, &lw, &next_p);
        if (lw > max_w) {
            max_w = lw;
        }
        total_h += line_h;
        if (next_p == p) {
            break;
        }
        p = next_p;
    }
    out.w = (int16_t)(max_w > box.size.w ? box.size.w : max_w);
    out.h = (int16_t)(total_h > box.size.h ? box.size.h : total_h);
    return out;
}

/* --- PebbleOS Font & Text Layout Syscalls --- */

static pebble_font_scale_tag_t parse_font_scale_from_key(const char *key) {
    if (key == NULL) {
        return PEBBLE_FONT_SCALE_1X;
    }
    uint32_t num = 0u;
    uint32_t best = 0u;
    for (const char *p = key; *p != '\0'; ++p) {
        if (*p >= '0' && *p <= '9') {
            num = num * 10u + (uint32_t)(*p - '0');
            if (num > best) best = num;
        } else {
            num = 0u;
        }
    }
    if (best >= 34u) return PEBBLE_FONT_SCALE_3X;
    if (best >= 20u) return PEBBLE_FONT_SCALE_2X;
    return PEBBLE_FONT_SCALE_1X;
}

static pebble_gfont_t shim_fonts_get_system_font(const char *font_key) {
    pebble_rt_record_syscall(PEBBLE_SYS_FONTS_GET_SYSTEM_FONT, true);
    return (pebble_gfont_t)(uintptr_t)parse_font_scale_from_key(font_key);
}

static pebble_gfont_t shim_fonts_load_custom_font(void *res_handle) {
    pebble_rt_record_syscall(PEBBLE_SYS_FONTS_LOAD_CUSTOM_FONT, true);
    uint32_t abs_off = 0u;
    uint32_t length = 0u;
    if (!pebble_res_lookup_handle(res_handle, &abs_off, &length) || length < 6u) {
        return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
    }

    uint8_t hdr[10];
    size_t hdr_read = (length < sizeof(hdr)) ? length : sizeof(hdr);
    if (pebble_res_read_abs(abs_off, hdr, hdr_read) != hdr_read) {
        return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
    }

    uint8_t version = hdr[0];
    uint8_t max_height = hdr[1];
    uint16_t num_glyphs = pebble_read_u16_le(hdr + 2u);
    uint16_t wildcard_cp = pebble_read_u16_le(hdr + 4u);

    pebble_pfo_font_t parsed;
    parsed.magic = PEBBLE_FONT_MAGIC;
    parsed.in_use = true;
    parsed.res_abs_offset = abs_off;
    parsed.res_length = length;
    parsed.version = version;
    parsed.max_height = max_height;
    parsed.num_glyphs = num_glyphs;
    parsed.wildcard_cp = wildcard_cp;
    parsed.hash_table_size = 0u;
    parsed.codepoint_bytes = 2u;
    parsed.offset_bytes = 4u;
    parsed.features = 0u;

    if (version == 1u) {
        parsed.ht_offset = 6u;
        parsed.ot_offset = 6u;
        parsed.entry_size = 4u;
        parsed.gt_offset = 6u + (uint32_t)num_glyphs * 4u;
    } else if (version == 2u && hdr_read >= 8u) {
        parsed.hash_table_size = hdr[6];
        parsed.codepoint_bytes = hdr[7];
        if (parsed.hash_table_size == 0u ||
            (parsed.codepoint_bytes != 2u && parsed.codepoint_bytes != 4u)) {
            return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
        }
        parsed.offset_bytes = 4u;
        parsed.entry_size = (uint32_t)parsed.codepoint_bytes + 4u;
        parsed.ht_offset = 8u;
        parsed.ot_offset = 8u + (uint32_t)parsed.hash_table_size * 4u;
        parsed.gt_offset = parsed.ot_offset + (uint32_t)num_glyphs * parsed.entry_size;
    } else if (version == 3u && hdr_read >= 10u) {
        parsed.hash_table_size = hdr[6];
        parsed.codepoint_bytes = hdr[7];
        uint8_t hdr_size = hdr[8];
        parsed.features = hdr[9];
        if (hdr_size < 10u || parsed.hash_table_size == 0u ||
            (parsed.codepoint_bytes != 2u && parsed.codepoint_bytes != 4u)) {
            return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
        }
        parsed.offset_bytes = ((parsed.features & PEBBLE_PFO_FEAT_OFFSET_16) != 0u) ? 2u : 4u;
        parsed.entry_size = (uint32_t)parsed.codepoint_bytes + (uint32_t)parsed.offset_bytes;
        parsed.ht_offset = hdr_size;
        parsed.ot_offset = (uint32_t)hdr_size + (uint32_t)parsed.hash_table_size * 4u;
        parsed.gt_offset = parsed.ot_offset + (uint32_t)num_glyphs * parsed.entry_size;
    } else {
        return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
    }

    if (parsed.gt_offset > length) {
        return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
    }

    for (uint32_t i = 0u; i < PEBBLE_MAX_CUSTOM_FONTS; ++i) {
        if (!g_custom_fonts[i].in_use) {
            g_custom_fonts[i] = parsed;
            return (pebble_gfont_t)&g_custom_fonts[i];
        }
    }
    return (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
}

static void shim_fonts_unload_custom_font(pebble_gfont_t font) {
    pebble_rt_record_syscall(PEBBLE_SYS_FONTS_UNLOAD_CUSTOM_FONT, true);
    pebble_pfo_font_t *f = (pebble_pfo_font_t *)(uintptr_t)as_custom_font(font);
    if (f != NULL) {
        f->in_use = false;
        f->magic = 0u;
    }
}

static pebble_gsize_t shim_text_layout_get_content_size(
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment) {
    (void)overflow_mode;
    (void)alignment;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_CONTENT_SIZE, true);
    return pebble_font_measure_text(text, font, box);
}

static pebble_gsize_t shim_text_layout_get_content_size_with_attrs(
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment,
    void *text_attributes) {
    (void)overflow_mode;
    (void)alignment;
    (void)text_attributes;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_CONTENT_SIZE_WITH_ATTRIBUTES, true);
    return pebble_font_measure_text(text, font, box);
}

static pebble_gsize_t shim_text_layout_get_max_used_size(
    pebble_gcontext_t *ctx,
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment,
    void *layout) {
    (void)ctx;
    (void)overflow_mode;
    (void)alignment;
    (void)layout;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_MAX_USED_SIZE, true);
    return pebble_font_measure_text(text, font, box);
}

void pebble_font_register_syscalls(void **table) {
    table[PEBBLE_SYS_FONTS_GET_SYSTEM_FONT] = (void *)shim_fonts_get_system_font;
    table[PEBBLE_SYS_FONTS_LOAD_CUSTOM_FONT] = (void *)shim_fonts_load_custom_font;
    table[PEBBLE_SYS_FONTS_UNLOAD_CUSTOM_FONT] = (void *)shim_fonts_unload_custom_font;
    table[PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_CONTENT_SIZE] = (void *)shim_text_layout_get_content_size;
    table[PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_CONTENT_SIZE_WITH_ATTRIBUTES] =
        (void *)shim_text_layout_get_content_size_with_attrs;
    table[PEBBLE_SYS_GRAPHICS_TEXT_LAYOUT_GET_MAX_USED_SIZE] = (void *)shim_text_layout_get_max_used_size;
}
