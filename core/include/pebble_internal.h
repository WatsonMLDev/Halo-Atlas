#ifndef PEBBLE_INTERNAL_H
#define PEBBLE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_compat.h"
#include "pebble_types.h"

/* Runtime hooks shared across core modules */
const pebble_hal_t *pebble_rt_hal(void);
const pebble_run_opts_t *pebble_rt_opts(void);
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

/* App heap & stdlib/math/time subsystem */
void pebble_stdlib_reset(void);
void pebble_stdlib_register_syscalls(void **table);
void *pebble_app_malloc(size_t size);
void *pebble_app_calloc(size_t count, size_t size);
void pebble_app_free(void *ptr);
int32_t pebble_sin_lookup_raw(int32_t angle);
int32_t pebble_cos_lookup_raw(int32_t angle);

/* Graphics & shadow framebuffer subsystem */
void pebble_gfx_reset(pebble_platform_t platform);
void pebble_gfx_register_syscalls(void **table);
void pebble_gfx_get_display_size(int16_t *w_out, int16_t *h_out);
pebble_gcontext_t *pebble_gfx_begin_frame(pebble_gcolor8_t bg_color);
void pebble_gfx_set_layer_transform(pebble_gcontext_t *ctx, pebble_gpoint_t origin, pebble_grect_t clip);
void pebble_gfx_flush_to_halo(const pebble_hal_t *hal);
void pebble_gfx_fill_box_internal(pebble_gcontext_t *ctx, pebble_grect_t box, pebble_gcolor8_t color);
void pebble_gfx_set_text_color_internal(pebble_gcontext_t *ctx, pebble_gcolor8_t color);
void pebble_gfx_draw_text_internal(
    pebble_gcontext_t *ctx,
    const char *text,
    pebble_gfont_t font,
    pebble_grect_t box,
    uint8_t overflow_mode,
    uint8_t alignment);
uint8_t pebble_gcolor2_to_8(int32_t c2);

/* UI (Window, Layer, TextLayer, TickTimer, Event Loop) subsystem */
void pebble_ui_reset(void);
void pebble_ui_register_syscalls(void **table);

#endif /* PEBBLE_INTERNAL_H */
