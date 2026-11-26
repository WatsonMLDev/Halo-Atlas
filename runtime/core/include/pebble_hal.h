#ifndef PEBBLE_HAL_H
#define PEBBLE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PEBBLE_HAL_CANVAS_WIDTH 256u
#define PEBBLE_HAL_CANVAS_HEIGHT 256u

typedef enum {
    PEBBLE_HAL_LOG_DEBUG = 0,
    PEBBLE_HAL_LOG_INFO = 1,
    PEBBLE_HAL_LOG_WARN = 2,
    PEBBLE_HAL_LOG_ERROR = 3,
} pebble_hal_log_level_t;

typedef enum {
    PEBBLE_HAL_EVENT_NONE = 0,
    PEBBLE_HAL_EVENT_BUTTON_SELECT = 1,
    PEBBLE_HAL_EVENT_BUTTON_BACK = 2,
    PEBBLE_HAL_EVENT_BUTTON_UP = 3,
    PEBBLE_HAL_EVENT_BUTTON_DOWN = 4,
    PEBBLE_HAL_EVENT_IMU_TAP_SINGLE = 5,
    PEBBLE_HAL_EVENT_IMU_TAP_DOUBLE = 6,
    PEBBLE_HAL_EVENT_QUIT = 7,
    PEBBLE_HAL_EVENT_IMU_TAP_TRIPLE = 8,
    PEBBLE_HAL_EVENT_PROTOCOL_RX = 9,
} pebble_hal_event_type_t;

typedef enum {
    PEBBLE_HAL_BTN_PRESS_SHORT = 0,
    PEBBLE_HAL_BTN_PRESS_LONG = 1,
} pebble_hal_btn_press_t;

typedef struct {
    pebble_hal_event_type_t type;
    int32_t value;
} pebble_hal_event_t;

typedef struct {
    int32_t sec;
    int32_t min;
    int32_t hour;
    int32_t mday;
    int32_t mon;   /* 0..11 */
    int32_t year;  /* years since 1900 */
    int32_t wday;  /* 0..6 (Sunday=0) */
    int32_t yday;  /* 0..365 */
    int32_t isdst;
} pebble_hal_wall_time_t;

typedef struct pebble_hal {
    void *ctx;

    /* Allocate RWX memory aligned to at least 4 bytes for native Thumb-2 execution. */
    void *(*alloc_exec)(void *ctx, size_t size);
    void (*free_exec)(void *ctx, void *ptr, size_t size);
    /* Optional: allocate/free transient RW working memory aligned to at least 8 bytes
     * (app heap, shadow FB, PNG scratch). Falls back to alloc_exec/free_exec when NULL. */
    void *(*alloc_data)(void *ctx, size_t size);
    void (*free_data)(void *ctx, void *ptr, size_t size);
    /* Optional: flush D-cache and invalidate I-cache after writing/relocating code. */
    void (*sync_icache)(void *ctx, void *ptr, size_t size);

    /* Time primitives: monotonic milliseconds and current wall-clock time. */
    uint64_t (*get_time_ms)(void *ctx);
    bool (*get_wall_time)(void *ctx, int64_t *unix_sec_out, pebble_hal_wall_time_t *tm_out);

    /* Flush a 256x256 RGB888 canvas buffer (width * height * 3 bytes). */
    void (*flush_canvas)(void *ctx, const uint8_t *rgb888, uint32_t width, uint32_t height);

    /* Non-blocking input event poll; returns true when out_event is populated. */
    bool (*poll_event)(void *ctx, pebble_hal_event_t *out_event);

    /* Read bytes from the loaded app's resource pack (offset + len). */
    size_t (*read_resource)(void *ctx, uint32_t offset, void *dst, size_t len);

    /* Optional: read/write serialized per-app persistent storage blob. */
    size_t (*read_persist)(void *ctx, void *dst, size_t max_len);
    bool (*write_persist)(void *ctx, const void *src, size_t len);

    /* Optional: Pebble Protocol 4-byte-framed packet seam ([u16_be len][u16_be ep][payload]). */
    bool (*send_protocol_packet)(void *ctx, const uint8_t *pkt, size_t len);
    size_t (*recv_protocol_packet)(void *ctx, uint8_t *dst, size_t max_len);

    /* Emit a null-terminated log line. */
    void (*log)(void *ctx, pebble_hal_log_level_t level, const char *msg);
} pebble_hal_t;

#endif /* PEBBLE_HAL_H */
