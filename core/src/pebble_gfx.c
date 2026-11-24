#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
static uint8_t g_fb_1bit_buf[(((PEBBLE_MAX_FB_W + 31u) / 32u) * 4u) * PEBBLE_MAX_FB_H];
static uint8_t g_halo_rgb[PEBBLE_HAL_CANVAS_WIDTH * PEBBLE_HAL_CANVAS_HEIGHT * 3u];
static int16_t g_fb_w = 180;
static int16_t g_fb_h = 180;
static pebble_platform_t g_fb_platform = PEBBLE_PLATFORM_CHALK;
static struct pebble_gcontext g_ctx;
static pebble_gbitmap_t g_fb_bitmap;
static bool g_fb_captured;
static bool g_in_render_pass;
static uint8_t g_fb_capture_format;

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
    g_fb_platform = platform;
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
    g_fb_captured = false;
    g_in_render_pass = false;
    g_fb_capture_format = PEBBLE_GBITMAP_FORMAT_8BIT;
}

void pebble_gfx_get_display_size(int16_t *w_out, int16_t *h_out) {
    if (w_out != NULL) *w_out = g_fb_w;
    if (h_out != NULL) *h_out = g_fb_h;
}

pebble_grect_t pebble_rect_intersect(pebble_grect_t a, pebble_grect_t b) {
    int32_t x0 = (a.origin.x > b.origin.x) ? a.origin.x : b.origin.x;
    int32_t y0 = (a.origin.y > b.origin.y) ? a.origin.y : b.origin.y;
    int32_t ax1 = a.origin.x + a.size.w;
    int32_t ay1 = a.origin.y + a.size.h;
    int32_t bx1 = b.origin.x + b.size.w;
    int32_t by1 = b.origin.y + b.size.h;
    int32_t x1 = (ax1 < bx1) ? ax1 : bx1;
    int32_t y1 = (ay1 < by1) ? ay1 : by1;
    pebble_grect_t out = {
        .origin = {(int16_t)x0, (int16_t)y0},
        .size = {(int16_t)(x1 > x0 ? (x1 - x0) : 0), (int16_t)(y1 > y0 ? (y1 - y0) : 0)},
    };
    return out;
}

pebble_gcontext_t *pebble_gfx_begin_frame(pebble_gcolor8_t bg_color) {
    uint8_t fill = (pebble_gcolor8_alpha(bg_color) == 0u) ? PEBBLE_GCOLOR_BLACK : bg_color.argb;
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
    g_fb_captured = false;
    g_in_render_pass = true;
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
    if (g_fb_captured) {
        return;
    }
    pebble_gcolor8_t src = {.argb = argb};
    uint8_t a = pebble_gcolor8_alpha(src);
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

void pebble_gfx_plot_local(const pebble_gcontext_t *ctx, int32_t x, int32_t y, uint8_t argb) {
    if (ctx != NULL) {
        plot_local(ctx, x, y, argb);
    }
}

uint8_t pebble_gfx_get_compositing_mode(const pebble_gcontext_t *ctx) {
    return (ctx != NULL) ? ctx->compositing_mode : PEBBLE_GCOMP_OP_ASSIGN;
}

void pebble_gfx_set_compositing_mode_internal(pebble_gcontext_t *ctx, uint8_t mode) {
    if (ctx != NULL) {
        ctx->compositing_mode = mode;
    }
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

pebble_gcolor8_t pebble_gfx_get_text_color(const pebble_gcontext_t *ctx) {
    pebble_gcolor8_t black = {.argb = PEBBLE_GCOLOR_BLACK};
    return (ctx != NULL) ? ctx->text_color : black;
}

pebble_grect_t pebble_gfx_push_box_clip(pebble_gcontext_t *ctx, pebble_grect_t local_box) {
    pebble_grect_t saved = ctx->clip;
    pebble_grect_t box_screen = {
        .origin = {
            (int16_t)(ctx->origin.x + local_box.origin.x),
            (int16_t)(ctx->origin.y + local_box.origin.y),
        },
        .size = local_box.size,
    };
    ctx->clip = pebble_rect_intersect(saved, box_screen);
    return saved;
}

void pebble_gfx_restore_clip(pebble_gcontext_t *ctx, pebble_grect_t saved_clip) {
    if (ctx != NULL) {
        ctx->clip = saved_clip;
    }
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
    int32_t outer_r2 = outer_r * outer_r;
    int32_t inner_r2 = inner_r * inner_r;
    int32_t span = angle_end - angle_start;
    bool full_circle = (span >= PEBBLE_TRIG_MAX_ANGLE);
    int32_t norm_start = angle_start & 0xFFFF;

    /* Exact pixel-by-pixel annular sector fill — zero Moire gaps at any resolution (incl. 260x260). */
    for (int32_t dy = -outer_r; dy <= outer_r; ++dy) {
        for (int32_t dx = -outer_r; dx <= outer_r; ++dx) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > outer_r2 || d2 < inner_r2) {
                continue;
            }
            if (!full_circle) {
                /* Pebble angles: 0 at 12 o'clock (-y), clockwise (+x => +0x4000). */
                int32_t ang = pebble_atan2_lookup_raw((int16_t)dx, (int16_t)(-dy));
                int32_t rel = (ang - norm_start) & 0xFFFF;
                if (rel > span) {
                    continue;
                }
            }
            plot_local(ctx, cx + dx, cy + dy, ctx->fill_color.argb);
        }
    }
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
    pebble_font_draw_text(ctx, text, font, box, overflow_mode, alignment);
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

void pebble_grect_align_internal(
    pebble_grect_t *rect,
    const pebble_grect_t *inside_rect,
    uint8_t alignment,
    bool clip) {
    if (rect == NULL || inside_rect == NULL) {
        return;
    }
    int32_t ix = inside_rect->origin.x;
    int32_t iy = inside_rect->origin.y;
    int32_t iw = inside_rect->size.w;
    int32_t ih = inside_rect->size.h;
    int32_t rw = rect->size.w;
    int32_t rh = rect->size.h;

    int32_t x = ix;
    int32_t y = iy;

    switch (alignment) {
        case PEBBLE_GALIGN_CENTER:
            x = ix + (iw - rw) / 2;
            y = iy + (ih - rh) / 2;
            break;
        case PEBBLE_GALIGN_TOP_LEFT:
            x = ix;
            y = iy;
            break;
        case PEBBLE_GALIGN_TOP_RIGHT:
            x = ix + iw - rw;
            y = iy;
            break;
        case PEBBLE_GALIGN_TOP:
            x = ix + (iw - rw) / 2;
            y = iy;
            break;
        case PEBBLE_GALIGN_LEFT:
            x = ix;
            y = iy + (ih - rh) / 2;
            break;
        case PEBBLE_GALIGN_BOTTOM:
            x = ix + (iw - rw) / 2;
            y = iy + ih - rh;
            break;
        case PEBBLE_GALIGN_RIGHT:
            x = ix + iw - rw;
            y = iy + (ih - rh) / 2;
            break;
        case PEBBLE_GALIGN_BOTTOM_RIGHT:
            x = ix + iw - rw;
            y = iy + ih - rh;
            break;
        case PEBBLE_GALIGN_BOTTOM_LEFT:
            x = ix;
            y = iy + ih - rh;
            break;
        default:
            break;
    }

    rect->origin.x = (int16_t)x;
    rect->origin.y = (int16_t)y;
    if (clip) {
        *rect = pebble_rect_intersect(*rect, *inside_rect);
    }
}

static void shim_grect_align(
    pebble_grect_t *rect,
    const pebble_grect_t *inside_rect,
    uint8_t alignment,
    bool clip) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRECT_ALIGN, true);
    pebble_grect_align_internal(rect, inside_rect, alignment, clip);
}

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

/* --- Direct Framebuffer Capture & Release --- */

static pebble_gbitmap_t *capture_fb_internal(pebble_gcontext_t *ctx, uint8_t format) {
    if (ctx == NULL || g_fb_captured) {
        return NULL;
    }
    if (format == PEBBLE_GBITMAP_FORMAT_8BIT || format == PEBBLE_GBITMAP_FORMAT_8BIT_CIRCULAR) {
        g_fb_bitmap.addr = g_shadow_fb;
        g_fb_bitmap.row_size_bytes = pebble_gbitmap_row_bytes_for_format((uint16_t)g_fb_w, format);
        g_fb_bitmap.info_flags = pebble_gbitmap_make_info_flags(format, false, false);
    } else if (format == PEBBLE_GBITMAP_FORMAT_1BIT) {
        uint16_t bpr = pebble_gbitmap_row_bytes_for_format((uint16_t)g_fb_w, PEBBLE_GBITMAP_FORMAT_1BIT);
        size_t total_bytes = (size_t)bpr * (size_t)g_fb_h;
        for (size_t i = 0u; i < total_bytes; ++i) {
            g_fb_1bit_buf[i] = 0u;
        }
        for (int32_t y = 0; y < g_fb_h; ++y) {
            const uint8_t *srow = &g_shadow_fb[(size_t)y * (size_t)g_fb_w];
            uint8_t *drow = &g_fb_1bit_buf[(size_t)y * (size_t)bpr];
            for (int32_t x = 0; x < g_fb_w; ++x) {
                uint8_t c = srow[x];
                uint32_t lum = (uint32_t)((c >> 4) & 3u) + (uint32_t)((c >> 2) & 3u) + (uint32_t)(c & 3u);
                if (lum >= 5u) {
                    drow[x >> 3] |= (uint8_t)(1u << (x & 7));
                }
            }
        }
        g_fb_bitmap.addr = g_fb_1bit_buf;
        g_fb_bitmap.row_size_bytes = bpr;
        g_fb_bitmap.info_flags = pebble_gbitmap_make_info_flags(PEBBLE_GBITMAP_FORMAT_1BIT, false, false);
    } else {
        return NULL;
    }

    g_fb_bitmap.bounds.origin.x = 0;
    g_fb_bitmap.bounds.origin.y = 0;
    g_fb_bitmap.bounds.size.w = g_fb_w;
    g_fb_bitmap.bounds.size.h = g_fb_h;
    g_fb_bitmap.palette = NULL;
    g_fb_bitmap.data_width = (uint16_t)g_fb_w;
    g_fb_bitmap.data_height = (uint16_t)g_fb_h;
    g_fb_bitmap.owns_data = false;
    g_fb_bitmap.owns_palette = false;
    g_fb_capture_format = format;
    g_fb_captured = true;
    return &g_fb_bitmap;
}

static pebble_gbitmap_t *shim_graphics_capture_frame_buffer(pebble_gcontext_t *ctx) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER, true);
    uint8_t native_fmt = (g_fb_platform == PEBBLE_PLATFORM_APLITE)
                             ? PEBBLE_GBITMAP_FORMAT_1BIT
                             : PEBBLE_GBITMAP_FORMAT_8BIT;
    return capture_fb_internal(ctx, native_fmt);
}

static pebble_gbitmap_t *shim_graphics_capture_frame_buffer_format(pebble_gcontext_t *ctx, uint8_t format) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER_FORMAT, true);
    return capture_fb_internal(ctx, format);
}

static pebble_gbitmap_t *shim_graphics_capture_frame_buffer_2bit(pebble_gcontext_t *ctx) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER_2BIT, true);
    return capture_fb_internal(ctx, PEBBLE_GBITMAP_FORMAT_1BIT);
}

static bool shim_graphics_frame_buffer_is_captured(pebble_gcontext_t *ctx) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_FRAME_BUFFER_IS_CAPTURED, true);
    return (ctx != NULL) && g_fb_captured;
}

static bool shim_graphics_release_frame_buffer(pebble_gcontext_t *ctx, pebble_gbitmap_t *buffer) {
    pebble_rt_record_syscall(PEBBLE_SYS_GRAPHICS_RELEASE_FRAME_BUFFER, true);
    if (ctx == NULL || !g_fb_captured || buffer != &g_fb_bitmap) {
        return false;
    }
    if (g_fb_capture_format == PEBBLE_GBITMAP_FORMAT_1BIT) {
        uint16_t bpr = g_fb_bitmap.row_size_bytes;
        for (int32_t y = 0; y < g_fb_h; ++y) {
            const uint8_t *srow = &g_fb_1bit_buf[(size_t)y * (size_t)bpr];
            uint8_t *drow = &g_shadow_fb[(size_t)y * (size_t)g_fb_w];
            for (int32_t x = 0; x < g_fb_w; ++x) {
                uint8_t bit = (uint8_t)((srow[x >> 3] >> (x & 7)) & 1u);
                drow[x] = bit ? PEBBLE_GCOLOR_WHITE : PEBBLE_GCOLOR_BLACK;
            }
        }
    }
    g_fb_captured = false;
    if (!g_in_render_pass) {
        pebble_gfx_flush_to_halo(pebble_rt_hal());
    }
    return true;
}

/* --- Flush shadow GColor8 framebuffer -> 256x256 RGB888 Halo canvas --- */

void pebble_gfx_flush_to_halo(const pebble_hal_t *hal) {
    g_in_render_pass = false;
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
            pebble_gcolor8_t px = {.argb = src_row[x]};
            dst_row[x * 3 + 0] = pebble_gcolor8_r8(px);
            dst_row[x * 3 + 1] = pebble_gcolor8_g8(px);
            dst_row[x * 3 + 2] = pebble_gcolor8_b8(px);
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
    table[PEBBLE_SYS_GRECT_ALIGN] = (void *)shim_grect_align;
    table[PEBBLE_SYS_GRECT_CENTER_POINT] = (void *)shim_grect_center_point;
    table[PEBBLE_SYS_GRECT_CROP] = (void *)shim_grect_crop;
    table[PEBBLE_SYS_GRECT_EQUAL] = (void *)shim_grect_equal;
    table[PEBBLE_SYS_GRECT_IS_EMPTY] = (void *)shim_grect_is_empty;
    table[PEBBLE_SYS_GRECT_CONTAINS_POINT] = (void *)shim_grect_contains_point;
    table[PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER] = (void *)shim_graphics_capture_frame_buffer;
    table[PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER_FORMAT] = (void *)shim_graphics_capture_frame_buffer_format;
    table[PEBBLE_SYS_GRAPHICS_CAPTURE_FRAME_BUFFER_2BIT] = (void *)shim_graphics_capture_frame_buffer_2bit;
    table[PEBBLE_SYS_GRAPHICS_FRAME_BUFFER_IS_CAPTURED] = (void *)shim_graphics_frame_buffer_is_captured;
    table[PEBBLE_SYS_GRAPHICS_RELEASE_FRAME_BUFFER] = (void *)shim_graphics_release_frame_buffer;
}
