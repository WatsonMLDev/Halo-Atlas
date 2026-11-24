#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_font_dogica.h"
#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_MAX_FB_W 260u
#define PEBBLE_MAX_FB_H 260u
#define PEBBLE_MAX_GPATH_PTS 64u

struct pebble_gcontext {
    pebble_gcolor8_t stroke_color;
    pebble_gcolor8_t fill_color;
    pebble_gcolor8_t text_color;
    uint8_t stroke_width;
    bool antialiased;
    uint8_t compositing_mode;
    pebble_gpoint_t origin;
    pebble_grect_t clip;
};

static uint8_t g_shadow_fb[PEBBLE_MAX_FB_W * PEBBLE_MAX_FB_H];
static uint8_t g_halo_rgb[PEBBLE_HAL_CANVAS_WIDTH * PEBBLE_HAL_CANVAS_HEIGHT * 3u];
static int16_t g_fb_w = 180;
static int16_t g_fb_h = 180;
static struct pebble_gcontext g_ctx;

uint8_t pebble_gcolor2_to_8(int32_t c2) {
    if (c2 == 0) {
        return PEBBLE_GCOLOR_BLACK;
    }
    if (c2 == 1) {
        return PEBBLE_GCOLOR_WHITE;
    }
    return PEBBLE_GCOLOR_CLEAR;
}

void pebble_gfx_reset(pebble_platform_t platform) {
    switch (platform) {
        case PEBBLE_PLATFORM_GABBRO:
            g_fb_w = 260;
            g_fb_h = 260;
            break;
        case PEBBLE_PLATFORM_BASALT:
        case PEBBLE_PLATFORM_APLITE:
            g_fb_w = 144;
            g_fb_h = 168;
            break;
        case PEBBLE_PLATFORM_CHALK:
        default:
            g_fb_w = 180;
            g_fb_h = 180;
            break;
    }
    size_t count = (size_t)g_fb_w * (size_t)g_fb_h;
    for (size_t i = 0u; i < count; ++i) {
        g_shadow_fb[i] = PEBBLE_GCOLOR_BLACK;
    }
    g_ctx.stroke_color.argb = PEBBLE_GCOLOR_BLACK;
    g_ctx.fill_color.argb = PEBBLE_GCOLOR_WHITE;
    g_ctx.text_color.argb = PEBBLE_GCOLOR_BLACK;
    g_ctx.stroke_width = 1u;
    g_ctx.antialiased = true;
    g_ctx.compositing_mode = 0u;
    g_ctx.origin.x = 0;
    g_ctx.origin.y = 0;
    g_ctx.clip.origin.x = 0;
    g_ctx.clip.origin.y = 0;
    g_ctx.clip.size.w = g_fb_w;
    g_ctx.clip.size.h = g_fb_h;
}

void pebble_gfx_get_display_size(int16_t *w_out, int16_t *h_out) {
    if (w_out != NULL) *w_out = g_fb_w;
    if (h_out != NULL) *h_out = g_fb_h;
}

pebble_gcontext_t *pebble_gfx_begin_frame(pebble_gcolor8_t bg_color) {
    uint8_t fill = ((bg_color.argb >> 6) & 3u) == 0u ? PEBBLE_GCOLOR_BLACK : bg_color.argb;
    size_t count = (size_t)g_fb_w * (size_t)g_fb_h;
    for (size_t i = 0u; i < count; ++i) {
        g_shadow_fb[i] = fill;
    }
    g_ctx.stroke_color.argb = PEBBLE_GCOLOR_BLACK;
    g_ctx.fill_color.argb = PEBBLE_GCOLOR_WHITE;
    g_ctx.text_color.argb = PEBBLE_GCOLOR_BLACK;
    g_ctx.stroke_width = 1u;
    g_ctx.origin.x = 0;
    g_ctx.origin.y = 0;
    g_ctx.clip.origin.x = 0;
    g_ctx.clip.origin.y = 0;
    g_ctx.clip.size.w = g_fb_w;
    g_ctx.clip.size.h = g_fb_h;
    return &g_ctx;
}

void pebble_gfx_set_layer_transform(pebble_gcontext_t *ctx, pebble_gpoint_t origin, pebble_grect_t clip) {
    if (ctx == NULL) {
        return;
    }
    ctx->origin = origin;
    ctx->clip = clip;
}

static void plot_screen_pixel(const pebble_gcontext_t *ctx, int32_t sx, int32_t sy, uint8_t argb) {
    uint8_t a = (uint8_t)((argb >> 6) & 3u);
    if (a == 0u) {
        return;
    }
    if (sx < 0 || sy < 0 || sx >= g_fb_w || sy >= g_fb_h) {
        return;
    }
    int32_t cx0 = ctx->clip.origin.x;
    int32_t cy0 = ctx->clip.origin.y;
    int32_t cx1 = cx0 + ctx->clip.size.w;
    int32_t cy1 = cy0 + ctx->clip.size.h;
    if (sx < cx0 || sy < cy0 || sx >= cx1 || sy >= cy1) {
        return;
    }
    size_t idx = (size_t)sy * (size_t)g_fb_w + (size_t)sx;
    if (a == 3u) {
        g_shadow_fb[idx] = argb;
        return;
    }
    uint8_t dst = g_shadow_fb[idx];
    uint8_t inv = (uint8_t)(3u - a);
    uint8_t r = (uint8_t)((((argb >> 4) & 3u) * a + ((dst >> 4) & 3u) * inv + 1u) / 3u);
    uint8_t g = (uint8_t)((((argb >> 2) & 3u) * a + ((dst >> 2) & 3u) * inv + 1u) / 3u);
    uint8_t b = (uint8_t)(((argb & 3u) * a + (dst & 3u) * inv + 1u) / 3u);
    g_shadow_fb[idx] = (uint8_t)(0xC0u | (r << 4) | (g << 2) | b);
}

static void plot_local(const pebble_gcontext_t *ctx, int32_t x, int32_t y, uint8_t argb) {
    plot_screen_pixel(ctx, x + ctx->origin.x, y + ctx->origin.y, argb);
}

static void fill_circle_raw(const pebble_gcontext_t *ctx, int32_t cx, int32_t cy, int32_t r, uint8_t argb) {
    if (r <= 0) {
        plot_local(ctx, cx, cy, argb);
        return;
    }
    int32_t r2 = r * r;
    for (int32_t dy = -r; dy <= r; ++dy) {
        int32_t dy2 = dy * dy;
        for (int32_t dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy2 <= r2) {
                plot_local(ctx, cx + dx, cy + dy, argb);
            }
        }
    }
}

static void draw_circle_outline_raw(const pebble_gcontext_t *ctx, int32_t cx, int32_t cy, int32_t r, uint8_t argb) {
    if (r <= 0) {
        plot_local(ctx, cx, cy, argb);
        return;
    }
    int32_t x = r;
    int32_t y = 0;
    int32_t err = 1 - r;
    int32_t brush = (ctx->stroke_width > 1u) ? (int32_t)(ctx->stroke_width / 2u) : 0;
    while (x >= y) {
        int32_t pts[8][2] = {
            {cx + x, cy + y}, {cx + y, cy + x}, {cx - y, cy + x}, {cx - x, cy + y},
            {cx - x, cy - y}, {cx - y, cy - x}, {cx + y, cy - x}, {cx + x, cy - y},
        };
        for (int i = 0; i < 8; ++i) {
            if (brush > 0) {
                fill_circle_raw(ctx, pts[i][0], pts[i][1], brush, argb);
            } else {
                plot_local(ctx, pts[i][0], pts[i][1], argb);
            }
        }
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x + 1);
        }
    }
}

static void draw_line_raw(
    const pebble_gcontext_t *ctx,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint8_t argb) {
    int32_t dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;
    int32_t brush = (ctx->stroke_width > 1u) ? (int32_t)(ctx->stroke_width / 2u) : 0;

    for (int32_t guard = 0; guard < 2048; ++guard) {
        if (brush > 0) {
            fill_circle_raw(ctx, x0, y0, brush, argb);
        } else {
            plot_local(ctx, x0, y0, argb);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static bool point_in_rounded_rect(
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    int32_t r,
    uint8_t mask) {
    if (r <= 0 || mask == 0u) {
        return true;
    }
    int32_t r2 = r * r;
    if ((mask & PEBBLE_CORNER_TOP_LEFT) && x < r && y < r) {
        int32_t dx = r - x;
        int32_t dy = r - y;
        if (dx * dx + dy * dy > r2) return false;
    }
    if ((mask & PEBBLE_CORNER_TOP_RIGHT) && x >= w - r && y < r) {
        int32_t dx = x - (w - 1 - r);
        int32_t dy = r - y;
        if (dx * dx + dy * dy > r2) return false;
    }
    if ((mask & PEBBLE_CORNER_BOTTOM_LEFT) && x < r && y >= h - r) {
        int32_t dx = r - x;
        int32_t dy = y - (h - 1 - r);
        if (dx * dx + dy * dy > r2) return false;
    }
    if ((mask & PEBBLE_CORNER_BOTTOM_RIGHT) && x >= w - r && y >= h - r) {
        int32_t dx = x - (w - 1 - r);
        int32_t dy = y - (h - 1 - r);
        if (dx * dx + dy * dy > r2) return false;
    }
    return true;
}

static void fill_rect_raw(
    const pebble_gcontext_t *ctx,
    pebble_grect_t rect,
    uint16_t corner_radius,
    uint8_t corner_mask,
    uint8_t argb) {
    int32_t w = rect.size.w;
    int32_t h = rect.size.h;
    if (w <= 0 || h <= 0) {
        return;
    }
    int32_t r = (int32_t)corner_radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            if (point_in_rounded_rect(x, y, w, h, r, corner_mask)) {
                plot_local(ctx, rect.origin.x + x, rect.origin.y + y, argb);
            }
        }
    }
}

void pebble_gfx_fill_box_internal(pebble_gcontext_t *ctx, pebble_grect_t box, pebble_gcolor8_t color) {
    if (ctx != NULL) {
        fill_rect_raw(ctx, box, 0u, PEBBLE_CORNER_NONE, color.argb);
    }
}

void pebble_gfx_set_text_color_internal(pebble_gcontext_t *ctx, pebble_gcolor8_t color) {
    if (ctx != NULL) {
        ctx->text_color = color;
    }
}

/* --- Text rendering using Halo's built-in Dogica 8px pixel font --- */

static uint8_t font_scale_for_handle(pebble_gfont_t font) {
    uintptr_t tag = (uintptr_t)font;
    if (tag == 2u) {
        return 2u;
    }
    if (tag == 3u) {
        return 3u;
    }
    return 1u;
}

static int32_t measure_line_px(const char *s, size_t len, uint8_t scale) {
    int32_t total = 0;
    for (size_t i = 0u; i < len; ++i) {
        uint8_t ch = (uint8_t)s[i];
        if (ch < PEBBLE_DOGICA_FIRST || ch > PEBBLE_DOGICA_LAST) {
            ch = (uint8_t)' ';
        }
        const pebble_dogica_glyph_t *g = &pebble_dogica_glyphs[ch - PEBBLE_DOGICA_FIRST];
        total += (int32_t)g->x_advance * (int32_t)scale;
    }
    return total;
}

static void draw_glyph(
    const pebble_gcontext_t *ctx,
    uint8_t ch,
    int32_t pen_x,
    int32_t baseline_y,
    uint8_t scale,
    uint8_t argb) {
    if (ch < PEBBLE_DOGICA_FIRST || ch > PEBBLE_DOGICA_LAST) {
        return;
    }
    const pebble_dogica_glyph_t *g = &pebble_dogica_glyphs[ch - PEBBLE_DOGICA_FIRST];
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
                        plot_local(ctx, px + (int32_t)sx, py + (int32_t)sy, argb);
                    }
                }
            }
            bit >>= 1;
        }
    }
}

void pebble_gfx_draw_text_internal(
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
    uint8_t scale = font_scale_for_handle(font);
    int32_t line_h = (int32_t)PEBBLE_DOGICA_Y_ADVANCE * (int32_t)scale;
    int32_t ascent = 7 * (int32_t)scale;

    /* Temporarily intersect ctx clip with box in screen coordinates. */
    pebble_grect_t saved_clip = ctx->clip;
    int32_t bx0 = ctx->origin.x + box.origin.x;
    int32_t by0 = ctx->origin.y + box.origin.y;
    int32_t bx1 = bx0 + box.size.w;
    int32_t by1 = by0 + box.size.h;
    int32_t cx0 = saved_clip.origin.x;
    int32_t cy0 = saved_clip.origin.y;
    int32_t cx1 = cx0 + saved_clip.size.w;
    int32_t cy1 = cy0 + saved_clip.size.h;
    if (bx0 > cx0) cx0 = bx0;
    if (by0 > cy0) cy0 = by0;
    if (bx1 < cx1) cx1 = bx1;
    if (by1 < cy1) cy1 = by1;
    ctx->clip.origin.x = (int16_t)cx0;
    ctx->clip.origin.y = (int16_t)cy0;
    ctx->clip.size.w = (int16_t)(cx1 > cx0 ? (cx1 - cx0) : 0);
    ctx->clip.size.h = (int16_t)(cy1 > cy0 ? (cy1 - cy0) : 0);

    int32_t cur_y = box.origin.y + ascent;
    const char *p = text;
    while (*p != '\0' && (cur_y - ascent) < (box.origin.y + box.size.h)) {
        /* Break line at newline or box width (preferring whitespace boundaries). */
        size_t len = 0u;
        size_t last_space = 0u;
        int32_t width_px = 0;
        while (p[len] != '\0' && p[len] != '\n') {
            uint8_t ch = (uint8_t)p[len];
            int32_t adv = measure_line_px(&p[len], 1u, scale);
            if (len > 0u && width_px + adv > box.size.w) {
                if (last_space > 0u) {
                    len = last_space;
                }
                break;
            }
            if (ch == ' ') {
                last_space = len;
            }
            width_px += adv;
            ++len;
        }

        int32_t line_w = measure_line_px(p, len, scale);
        int32_t pen_x = box.origin.x;
        if (alignment == PEBBLE_TEXT_ALIGN_CENTER) {
            pen_x = box.origin.x + (box.size.w - line_w) / 2;
        } else if (alignment == PEBBLE_TEXT_ALIGN_RIGHT) {
            pen_x = box.origin.x + (box.size.w - line_w);
        }

        for (size_t i = 0u; i < len; ++i) {
            uint8_t ch = (uint8_t)p[i];
            draw_glyph(ctx, ch, pen_x, cur_y, scale, ctx->text_color.argb);
            pen_x += measure_line_px(&p[i], 1u, scale);
        }

        p += len;
        if (*p == '\n' || *p == ' ') {
            ++p;
        }
        cur_y += line_h;
    }

    ctx->clip = saved_clip;
}

/* --- PebbleOS Graphics Syscall Shims --- */

static void shim_set_fill_color(pebble_gcontext_t *ctx, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_FILL_COLOR, true);
    if (ctx != NULL) ctx->fill_color = color;
}

static void shim_set_stroke_color(pebble_gcontext_t *ctx, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_COLOR, true);
    if (ctx != NULL) ctx->stroke_color = color;
}

static void shim_set_text_color(pebble_gcontext_t *ctx, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_TEXT_COLOR, true);
    if (ctx != NULL) ctx->text_color = color;
}

static void shim_set_fill_color_2bit(pebble_gcontext_t *ctx, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_FILL_COLOR_2BIT, true);
    if (ctx != NULL) ctx->fill_color.argb = pebble_gcolor2_to_8(color);
}

static void shim_set_stroke_color_2bit(pebble_gcontext_t *ctx, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_COLOR_2BIT, true);
    if (ctx != NULL) ctx->stroke_color.argb = pebble_gcolor2_to_8(color);
}

static void shim_set_text_color_2bit(pebble_gcontext_t *ctx, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_TEXT_COLOR_2BIT, true);
    if (ctx != NULL) ctx->text_color.argb = pebble_gcolor2_to_8(color);
}

static void shim_set_stroke_width(pebble_gcontext_t *ctx, uint8_t stroke_width) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_WIDTH, true);
    if (ctx != NULL) ctx->stroke_width = (stroke_width == 0u) ? 1u : stroke_width;
}

static void shim_set_antialiased(pebble_gcontext_t *ctx, bool enable) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_ANTIALIASED, true);
    if (ctx != NULL) ctx->antialiased = enable;
}

static void shim_set_compositing_mode(pebble_gcontext_t *ctx, uint8_t mode) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CONTEXT_SET_COMPOSITING_MODE, true);
    if (ctx != NULL) ctx->compositing_mode = mode;
}

static void shim_draw_pixel(pebble_gcontext_t *ctx, pebble_gpoint_t p) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_PIXEL, true);
    if (ctx != NULL) plot_local(ctx, p.x, p.y, ctx->stroke_color.argb);
}

static void shim_draw_line(pebble_gcontext_t *ctx, pebble_gpoint_t p0, pebble_gpoint_t p1) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_LINE, true);
    if (ctx != NULL) draw_line_raw(ctx, p0.x, p0.y, p1.x, p1.y, ctx->stroke_color.argb);
}

static void shim_draw_rect(pebble_gcontext_t *ctx, pebble_grect_t rect) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_RECT, true);
    if (ctx == NULL || rect.size.w <= 0 || rect.size.h <= 0) return;
    int32_t x0 = rect.origin.x;
    int32_t y0 = rect.origin.y;
    int32_t x1 = x0 + rect.size.w - 1;
    int32_t y1 = y0 + rect.size.h - 1;
    uint8_t c = ctx->stroke_color.argb;
    draw_line_raw(ctx, x0, y0, x1, y0, c);
    draw_line_raw(ctx, x1, y0, x1, y1, c);
    draw_line_raw(ctx, x1, y1, x0, y1, c);
    draw_line_raw(ctx, x0, y1, x0, y0, c);
}

static void shim_draw_round_rect(pebble_gcontext_t *ctx, pebble_grect_t rect, uint16_t radius) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_ROUND_RECT, true);
    if (ctx == NULL || rect.size.w <= 0 || rect.size.h <= 0) return;
    int32_t w = rect.size.w;
    int32_t h = rect.size.h;
    int32_t r = (int32_t)radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    uint8_t c = ctx->stroke_color.argb;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            if (!point_in_rounded_rect(x, y, w, h, r, PEBBLE_CORNERS_ALL)) {
                continue;
            }
            bool on_edge = (x == 0 || y == 0 || x == w - 1 || y == h - 1 ||
                            !point_in_rounded_rect(x - 1, y, w, h, r, PEBBLE_CORNERS_ALL) ||
                            !point_in_rounded_rect(x + 1, y, w, h, r, PEBBLE_CORNERS_ALL) ||
                            !point_in_rounded_rect(x, y - 1, w, h, r, PEBBLE_CORNERS_ALL) ||
                            !point_in_rounded_rect(x, y + 1, w, h, r, PEBBLE_CORNERS_ALL));
            if (on_edge) {
                plot_local(ctx, rect.origin.x + x, rect.origin.y + y, c);
            }
        }
    }
}

static void shim_fill_rect(
    pebble_gcontext_t *ctx,
    pebble_grect_t rect,
    uint16_t corner_radius,
    uint8_t corner_mask) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_FILL_RECT, true);
    if (ctx != NULL) {
        fill_rect_raw(ctx, rect, corner_radius, corner_mask, ctx->fill_color.argb);
    }
}

static void shim_draw_circle(pebble_gcontext_t *ctx, pebble_gpoint_t p, uint16_t radius) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_CIRCLE, true);
    if (ctx != NULL) draw_circle_outline_raw(ctx, p.x, p.y, (int32_t)radius, ctx->stroke_color.argb);
}

static void shim_fill_circle(pebble_gcontext_t *ctx, pebble_gpoint_t p, uint16_t radius) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_FILL_CIRCLE, true);
    if (ctx != NULL) fill_circle_raw(ctx, p.x, p.y, (int32_t)radius, ctx->fill_color.argb);
}

static void shim_draw_arc(
    pebble_gcontext_t *ctx,
    pebble_grect_t rect,
    uint8_t scale_mode,
    int32_t angle_start,
    int32_t angle_end) {
    (void)scale_mode;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_ARC, true);
    if (ctx == NULL || rect.size.w <= 0 || rect.size.h <= 0 || angle_end <= angle_start) {
        return;
    }
    int32_t cx = rect.origin.x + rect.size.w / 2;
    int32_t cy = rect.origin.y + rect.size.h / 2;
    int32_t rx = rect.size.w / 2;
    int32_t ry = rect.size.h / 2;
    int32_t steps = (angle_end - angle_start) / 128;
    if (steps < 16) steps = 16;
    if (steps > 512) steps = 512;
    int32_t prev_x = cx + (pebble_sin_lookup_raw(angle_start) * rx) / PEBBLE_TRIG_MAX_RATIO;
    int32_t prev_y = cy - (pebble_cos_lookup_raw(angle_start) * ry) / PEBBLE_TRIG_MAX_RATIO;
    for (int32_t i = 1; i <= steps; ++i) {
        int32_t span = angle_end - angle_start;
        int32_t a = angle_start + (span / steps) * i + ((span % steps) * i) / steps;
        int32_t nx = cx + (pebble_sin_lookup_raw(a) * rx) / PEBBLE_TRIG_MAX_RATIO;
        int32_t ny = cy - (pebble_cos_lookup_raw(a) * ry) / PEBBLE_TRIG_MAX_RATIO;
        draw_line_raw(ctx, prev_x, prev_y, nx, ny, ctx->stroke_color.argb);
        prev_x = nx;
        prev_y = ny;
    }
}

static void shim_fill_radial(
    pebble_gcontext_t *ctx,
    pebble_grect_t rect,
    uint8_t scale_mode,
    uint16_t inset_thickness,
    int32_t angle_start,
    int32_t angle_end) {
    (void)scale_mode;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_FILL_RADIAL, true);
    if (ctx == NULL || rect.size.w <= 0 || rect.size.h <= 0 || angle_end <= angle_start) {
        return;
    }
    int32_t cx = rect.origin.x + rect.size.w / 2;
    int32_t cy = rect.origin.y + rect.size.h / 2;
    int32_t outer_r = (rect.size.w < rect.size.h ? rect.size.w : rect.size.h) / 2;
    int32_t inner_r = outer_r - (int32_t)inset_thickness;
    if (inner_r < 0) inner_r = 0;
    int32_t steps = (angle_end - angle_start) / 96;
    if (steps < 24) steps = 24;
    if (steps > 720) steps = 720;
    uint8_t saved_w = ctx->stroke_width;
    ctx->stroke_width = 1u;
    for (int32_t i = 0; i <= steps; ++i) {
        int32_t span = angle_end - angle_start;
        int32_t a = angle_start + (span / steps) * i + ((span % steps) * i) / steps;
        int32_t s = pebble_sin_lookup_raw(a);
        int32_t c = pebble_cos_lookup_raw(a);
        int32_t x0 = cx + (s * inner_r) / PEBBLE_TRIG_MAX_RATIO;
        int32_t y0 = cy - (c * inner_r) / PEBBLE_TRIG_MAX_RATIO;
        int32_t x1 = cx + (s * outer_r) / PEBBLE_TRIG_MAX_RATIO;
        int32_t y1 = cy - (c * outer_r) / PEBBLE_TRIG_MAX_RATIO;
        draw_line_raw(ctx, x0, y0, x1, y1, ctx->fill_color.argb);
    }
    ctx->stroke_width = saved_w;
}

static void shim_draw_text(
    pebble_gcontext_t *ctx,
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment,
    void *text_attributes) {
    (void)text_attributes;
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_DRAW_TEXT, true);
    pebble_gfx_draw_text_internal(ctx, text, font, box, overflow_mode, alignment);
}

/* --- GPath --- */

static pebble_gpath_t *shim_gpath_create(const pebble_gpath_info_t *init) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_CREATE, true);
    if (init == NULL) {
        return NULL;
    }
    pebble_gpath_t *path = (pebble_gpath_t *)pebble_app_calloc(1u, sizeof(pebble_gpath_t));
    if (path == NULL) {
        return NULL;
    }
    path->num_points = init->num_points;
    path->points = init->points;
    path->rotation = 0;
    path->offset.x = 0;
    path->offset.y = 0;
    return path;
}

static void shim_gpath_destroy(pebble_gpath_t *path) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_DESTROY, true);
    pebble_app_free(path);
}

static void shim_gpath_move_to(pebble_gpath_t *path, pebble_gpoint_t point) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_MOVE_TO, true);
    if (path != NULL) path->offset = point;
}

static void shim_gpath_rotate_to(pebble_gpath_t *path, int32_t angle) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_ROTATE_TO, true);
    if (path != NULL) path->rotation = angle;
}

static uint32_t transform_gpath(const pebble_gpath_t *path, pebble_gpoint_t *out, uint32_t cap) {
    if (path == NULL || path->points == NULL || path->num_points == 0u) {
        return 0u;
    }
    uint32_t n = (path->num_points < cap) ? path->num_points : cap;
    int32_t s = pebble_sin_lookup_raw(path->rotation);
    int32_t c = pebble_cos_lookup_raw(path->rotation);
    for (uint32_t i = 0u; i < n; ++i) {
        int32_t px = path->points[i].x;
        int32_t py = path->points[i].y;
        int32_t rx = (px * c - py * s) / PEBBLE_TRIG_MAX_RATIO + path->offset.x;
        int32_t ry = (px * s + py * c) / PEBBLE_TRIG_MAX_RATIO + path->offset.y;
        out[i].x = (int16_t)rx;
        out[i].y = (int16_t)ry;
    }
    return n;
}

static void draw_gpath_outline_impl(pebble_gcontext_t *ctx, const pebble_gpath_t *path, bool closed) {
    pebble_gpoint_t pts[PEBBLE_MAX_GPATH_PTS];
    uint32_t n = transform_gpath(path, pts, PEBBLE_MAX_GPATH_PTS);
    if (ctx == NULL || n < 2u) {
        return;
    }
    for (uint32_t i = 0u; i + 1u < n; ++i) {
        draw_line_raw(ctx, pts[i].x, pts[i].y, pts[i + 1u].x, pts[i + 1u].y, ctx->stroke_color.argb);
    }
    if (closed && n >= 3u) {
        draw_line_raw(ctx, pts[n - 1u].x, pts[n - 1u].y, pts[0].x, pts[0].y, ctx->stroke_color.argb);
    }
}

static void shim_gpath_draw_outline(pebble_gcontext_t *ctx, const pebble_gpath_t *path) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_DRAW_OUTLINE, true);
    draw_gpath_outline_impl(ctx, path, true);
}

static void shim_gpath_draw_outline_open(pebble_gcontext_t *ctx, const pebble_gpath_t *path) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_DRAW_OUTLINE_OPEN, true);
    draw_gpath_outline_impl(ctx, path, false);
}

static void shim_gpath_draw_filled(pebble_gcontext_t *ctx, const pebble_gpath_t *path) {
    pebble_rt_record_syscall(PEBBLE_SYS_GPATH_DRAW_FILLED, true);
    pebble_gpoint_t pts[PEBBLE_MAX_GPATH_PTS];
    uint32_t n = transform_gpath(path, pts, PEBBLE_MAX_GPATH_PTS);
    if (ctx == NULL || n < 3u) {
        return;
    }
    int32_t min_y = pts[0].y;
    int32_t max_y = pts[0].y;
    for (uint32_t i = 1u; i < n; ++i) {
        if (pts[i].y < min_y) min_y = pts[i].y;
        if (pts[i].y > max_y) max_y = pts[i].y;
    }
    int32_t nodes[PEBBLE_MAX_GPATH_PTS];
    for (int32_t y = min_y; y <= max_y; ++y) {
        uint32_t node_count = 0u;
        uint32_t j = n - 1u;
        for (uint32_t i = 0u; i < n; ++i) {
            int32_t yi = pts[i].y;
            int32_t yj = pts[j].y;
            if (((yi < y && yj >= y) || (yj < y && yi >= y)) && node_count < PEBBLE_MAX_GPATH_PTS) {
                int32_t xi = pts[i].x;
                int32_t xj = pts[j].x;
                nodes[node_count++] = xi + ((y - yi) * (xj - xi)) / (yj - yi);
            }
            j = i;
        }
        for (uint32_t a = 1u; a < node_count; ++a) {
            int32_t key = nodes[a];
            uint32_t b = a;
            while (b > 0u && nodes[b - 1u] > key) {
                nodes[b] = nodes[b - 1u];
                --b;
            }
            nodes[b] = key;
        }
        for (uint32_t k = 0u; k + 1u < node_count; k += 2u) {
            for (int32_t x = nodes[k]; x <= nodes[k + 1u]; ++x) {
                plot_local(ctx, x, y, ctx->fill_color.argb);
            }
        }
    }
}

/* --- GRect helpers --- */

static pebble_gpoint_t shim_grect_center_point(const pebble_grect_t *rect) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_CENTER_POINT, true);
    pebble_gpoint_t pt = {0, 0};
    if (rect != NULL) {
        pt.x = (int16_t)(rect->origin.x + rect->size.w / 2);
        pt.y = (int16_t)(rect->origin.y + rect->size.h / 2);
    }
    return pt;
}

static pebble_grect_t shim_grect_crop(pebble_grect_t rect, int32_t crop_size_px) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_CROP, true);
    pebble_grect_t out = rect;
    out.origin.x = (int16_t)(out.origin.x + crop_size_px);
    out.origin.y = (int16_t)(out.origin.y + crop_size_px);
    out.size.w = (int16_t)(out.size.w - 2 * crop_size_px);
    out.size.h = (int16_t)(out.size.h - 2 * crop_size_px);
    if (out.size.w < 0) out.size.w = 0;
    if (out.size.h < 0) out.size.h = 0;
    return out;
}

static bool shim_grect_equal(const pebble_grect_t *a, const pebble_grect_t *b) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_EQUAL, true);
    if (a == NULL || b == NULL) return false;
    return a->origin.x == b->origin.x && a->origin.y == b->origin.y &&
           a->size.w == b->size.w && a->size.h == b->size.h;
}

static bool shim_grect_is_empty(const pebble_grect_t *rect) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_IS_EMPTY, true);
    return rect == NULL || rect->size.w <= 0 || rect->size.h <= 0;
}

static bool shim_grect_contains_point(const pebble_grect_t *rect, const pebble_gpoint_t *pt) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_CONTAINS_POINT, true);
    if (rect == NULL || pt == NULL) return false;
    return pt->x >= rect->origin.x && pt->x < rect->origin.x + rect->size.w &&
           pt->y >= rect->origin.y && pt->y < rect->origin.y + rect->size.h;
}

/* --- Flush shadow GColor8 framebuffer -> 256x256 RGB888 Halo canvas --- */

void pebble_gfx_flush_to_halo(const pebble_hal_t *hal) {
    if (hal == NULL || hal->flush_canvas == NULL) {
        return;
    }

    size_t total_bytes = (size_t)PEBBLE_HAL_CANVAS_WIDTH * (size_t)PEBBLE_HAL_CANVAS_HEIGHT * 3u;
    for (size_t i = 0u; i < total_bytes; ++i) {
        g_halo_rgb[i] = 0u;
    }

    int32_t src_x0 = 0;
    int32_t src_y0 = 0;
    int32_t dst_x0 = 0;
    int32_t dst_y0 = 0;
    int32_t copy_w = g_fb_w;
    int32_t copy_h = g_fb_h;

    if (g_fb_w > (int16_t)PEBBLE_HAL_CANVAS_WIDTH) {
        /* Center-crop (e.g. gabbro 260x260 -> crop 2px per edge into 256x256). */
        src_x0 = (g_fb_w - (int32_t)PEBBLE_HAL_CANVAS_WIDTH) / 2;
        copy_w = (int32_t)PEBBLE_HAL_CANVAS_WIDTH;
    } else {
        /* Center inside 256x256 (e.g. chalk 180x180 -> +38, basalt/aplite 144x168 -> +56). */
        dst_x0 = ((int32_t)PEBBLE_HAL_CANVAS_WIDTH - g_fb_w) / 2;
    }

    if (g_fb_h > (int16_t)PEBBLE_HAL_CANVAS_HEIGHT) {
        src_y0 = (g_fb_h - (int32_t)PEBBLE_HAL_CANVAS_HEIGHT) / 2;
        copy_h = (int32_t)PEBBLE_HAL_CANVAS_HEIGHT;
    } else {
        dst_y0 = ((int32_t)PEBBLE_HAL_CANVAS_HEIGHT - g_fb_h) / 2;
    }

    for (int32_t y = 0; y < copy_h; ++y) {
        const uint8_t *src_row = &g_shadow_fb[(size_t)(src_y0 + y) * (size_t)g_fb_w + (size_t)src_x0];
        uint8_t *dst_row =
            &g_halo_rgb[((size_t)(dst_y0 + y) * (size_t)PEBBLE_HAL_CANVAS_WIDTH + (size_t)dst_x0) * 3u];
        for (int32_t x = 0; x < copy_w; ++x) {
            uint8_t argb = src_row[x];
            dst_row[x * 3 + 0] = (uint8_t)(((argb >> 4) & 3u) * 0x55u);
            dst_row[x * 3 + 1] = (uint8_t)(((argb >> 2) & 3u) * 0x55u);
            dst_row[x * 3 + 2] = (uint8_t)((argb & 3u) * 0x55u);
        }
    }

    hal->flush_canvas(hal->ctx, g_halo_rgb, PEBBLE_HAL_CANVAS_WIDTH, PEBBLE_HAL_CANVAS_HEIGHT);
    pebble_rt_note_frame_flushed();
}

void pebble_gfx_register_syscalls(void **table) {
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_FILL_COLOR] = (void *)shim_set_fill_color;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_COLOR] = (void *)shim_set_stroke_color;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_TEXT_COLOR] = (void *)shim_set_text_color;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_FILL_COLOR_2BIT] = (void *)shim_set_fill_color_2bit;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_COLOR_2BIT] = (void *)shim_set_stroke_color_2bit;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_TEXT_COLOR_2BIT] = (void *)shim_set_text_color_2bit;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_STROKE_WIDTH] = (void *)shim_set_stroke_width;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_ANTIALIASED] = (void *)shim_set_antialiased;
    table[PEBBLE_SYS_GRAPHICS_CONTEXT_SET_COMPOSITING_MODE] = (void *)shim_set_compositing_mode;
    table[PEBBLE_SYS_GRAPHICS_DRAW_PIXEL] = (void *)shim_draw_pixel;
    table[PEBBLE_SYS_GRAPHICS_DRAW_LINE] = (void *)shim_draw_line;
    table[PEBBLE_SYS_GRAPHICS_DRAW_RECT] = (void *)shim_draw_rect;
    table[PEBBLE_SYS_GRAPHICS_DRAW_ROUND_RECT] = (void *)shim_draw_round_rect;
    table[PEBBLE_SYS_GRAPHICS_FILL_RECT] = (void *)shim_fill_rect;
    table[PEBBLE_SYS_GRAPHICS_DRAW_CIRCLE] = (void *)shim_draw_circle;
    table[PEBBLE_SYS_GRAPHICS_FILL_CIRCLE] = (void *)shim_fill_circle;
    table[PEBBLE_SYS_GRAPHICS_DRAW_ARC] = (void *)shim_draw_arc;
    table[PEBBLE_SYS_GRAPHICS_FILL_RADIAL] = (void *)shim_fill_radial;
    table[PEBBLE_SYS_GRAPHICS_DRAW_TEXT] = (void *)shim_draw_text;
    table[PEBBLE_SYS_GPATH_CREATE] = (void *)shim_gpath_create;
    table[PEBBLE_SYS_GPATH_DESTROY] = (void *)shim_gpath_destroy;
    table[PEBBLE_SYS_GPATH_MOVE_TO] = (void *)shim_gpath_move_to;
    table[PEBBLE_SYS_GPATH_ROTATE_TO] = (void *)shim_gpath_rotate_to;
    table[PEBBLE_SYS_GPATH_DRAW_OUTLINE] = (void *)shim_gpath_draw_outline;
    table[PEBBLE_SYS_GPATH_DRAW_OUTLINE_OPEN] = (void *)shim_gpath_draw_outline_open;
    table[PEBBLE_SYS_GPATH_DRAW_FILLED] = (void *)shim_gpath_draw_filled;
    table[PEBBLE_SYS_GPATH_DRAW_FILLED_LEGACY] = (void *)shim_gpath_draw_filled;
    table[PEBBLE_SYS_GRECT_CENTER_POINT] = (void *)shim_grect_center_point;
    table[PEBBLE_SYS_GRECT_CROP] = (void *)shim_grect_crop;
    table[PEBBLE_SYS_GRECT_EQUAL] = (void *)shim_grect_equal;
    table[PEBBLE_SYS_GRECT_IS_EMPTY] = (void *)shim_grect_is_empty;
    table[PEBBLE_SYS_GRECT_CONTAINS_POINT] = (void *)shim_grect_contains_point;
}
