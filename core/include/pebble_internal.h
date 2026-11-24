#ifndef PEBBLE_INTERNAL_H
#define PEBBLE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_compat.h"
#include "pebble_types.h"

/* Runtime hooks shared across core modules */
const pebble_hal_t *pebble_rt_hal(void);

static inline uint16_t pebble_read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t pebble_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
const pebble_run_opts_t *pebble_rt_opts(void);
uint64_t pebble_rt_mono_ms(void);
void pebble_rt_record_syscall(uint32_t slot, bool implemented);
void pebble_rt_note_frame_flushed(void);
int64_t pebble_rt_current_epoch(void);
void pebble_rt_advance_sim_epoch(int32_t delta_sec);
void pebble_rt_epoch_to_tm(int64_t epoch_sec, pebble_tm_t *out);

/* Loader execution entry point */
pebble_status_t pebble_loader_exec(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    const pebble_image_info_t *info,
    void **syscall_table,
    int32_t *app_return_out);

typedef enum {
    PEBBLE_FONT_SCALE_1X = 1u,
    PEBBLE_FONT_SCALE_2X = 2u,
    PEBBLE_FONT_SCALE_3X = 3u,
} pebble_font_scale_tag_t;

/* App heap & stdlib/math/time subsystem */
void pebble_stdlib_reset(void);
void pebble_stdlib_register_syscalls(void **table);
void *pebble_app_malloc(size_t size);
void *pebble_app_calloc(size_t count, size_t size);
void pebble_app_free(void *ptr);
int32_t pebble_sin_lookup_raw(int32_t angle);
int32_t pebble_cos_lookup_raw(int32_t angle);
int32_t pebble_atan2_lookup_raw(int16_t y, int16_t x);

/* Graphics & shadow framebuffer subsystem */
void pebble_gfx_reset(pebble_platform_t platform);
void pebble_gfx_register_syscalls(void **table);
void pebble_gfx_get_display_size(int16_t *w_out, int16_t *h_out);
pebble_grect_t pebble_rect_intersect(pebble_grect_t a, pebble_grect_t b);
void pebble_grect_align_internal(
    pebble_grect_t *rect,
    const pebble_grect_t *inside_rect,
    uint8_t alignment,
    bool clip);
pebble_gcontext_t *pebble_gfx_begin_frame(pebble_gcolor8_t bg_color);
void pebble_gfx_set_layer_transform(pebble_gcontext_t *ctx, pebble_gpoint_t origin, pebble_grect_t clip);
void pebble_gfx_flush_to_halo(const pebble_hal_t *hal);
void pebble_gfx_fill_box_internal(pebble_gcontext_t *ctx, pebble_grect_t box, pebble_gcolor8_t color);
void pebble_gfx_set_text_color_internal(pebble_gcontext_t *ctx, pebble_gcolor8_t color);
pebble_gcolor8_t pebble_gfx_get_text_color(const pebble_gcontext_t *ctx);
pebble_grect_t pebble_gfx_push_box_clip(pebble_gcontext_t *ctx, pebble_grect_t local_box);
void pebble_gfx_restore_clip(pebble_gcontext_t *ctx, pebble_grect_t saved_clip);
void pebble_gfx_plot_local(const pebble_gcontext_t *ctx, int32_t x, int32_t y, uint8_t argb);
uint8_t pebble_gfx_get_compositing_mode(const pebble_gcontext_t *ctx);
void pebble_gfx_set_compositing_mode_internal(pebble_gcontext_t *ctx, uint8_t mode);
uint8_t pebble_gcolor2_to_8(int32_t c2);

/* Font (.pfo custom fonts + Dogica fallback) subsystem */
void pebble_font_reset(void);
void pebble_font_register_syscalls(void **table);
void pebble_font_draw_text(
    pebble_gcontext_t *ctx,
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment);
pebble_gsize_t pebble_font_measure_text(const char *text, pebble_gfont_t font, pebble_grect_t box);

/* Bitmap (PBI/PNG decoding, GBitmap, blitting) subsystem */
void pebble_bitmap_register_syscalls(void **table);
uint16_t pebble_gbitmap_make_info_flags(uint8_t format, bool heap_data, bool heap_palette);
uint16_t pebble_gbitmap_row_bytes_for_format(uint16_t width, uint8_t format);
void pebble_bitmap_draw_in_rect(pebble_gcontext_t *ctx, const pebble_gbitmap_t *bitmap, pebble_grect_t rect);

/* Resource pack (.pbpack) subsystem */
void pebble_res_reset(const pebble_hal_t *hal);
void pebble_res_register_syscalls(void **table);
bool pebble_res_lookup_id(uint32_t resource_id, uint32_t *abs_offset_out, uint32_t *length_out);
bool pebble_res_lookup_handle(void *handle, uint32_t *abs_offset_out, uint32_t *length_out);
size_t pebble_res_read_abs(uint32_t abs_offset, void *dst, size_t len);

/* App timers (app_timer_register/reschedule/cancel, psleep) subsystem */
void pebble_timer_reset(void);
void pebble_timer_register_syscalls(void **table);
void pebble_timer_process(uint64_t now_ms);

/* Persistent key-value storage (persist_*) subsystem */
void pebble_persist_reset(const pebble_hal_t *hal);
void pebble_persist_register_syscalls(void **table);

/* UI (Window, Layer, TextLayer, TickTimer, Event Loop) subsystem */
void pebble_ui_reset(void);
void pebble_ui_register_syscalls(void **table);
bool pebble_ui_has_windows(void);

#endif /* PEBBLE_INTERNAL_H */
