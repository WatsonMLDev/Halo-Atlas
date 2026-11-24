#ifndef PEBBLE_HAL_POSIX_ARM_H
#define PEBBLE_HAL_POSIX_ARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_hal.h"

typedef struct {
    bool stream_frames;
    bool live_sleep_on_poll;
} posix_arm_hal_ctx_t;

void posix_arm_hal_init(pebble_hal_t *hal_out, posix_arm_hal_ctx_t *ctx);
uint8_t *posix_arm_read_file(const char *path, size_t *len_out);
void posix_arm_free_file(uint8_t *buf);
void posix_arm_write_str(uint32_t fd, const char *s);
void posix_arm_write_i32(uint32_t fd, int32_t v);
void posix_arm_write_u32(uint32_t fd, uint32_t v);

#endif /* PEBBLE_HAL_POSIX_ARM_H */
