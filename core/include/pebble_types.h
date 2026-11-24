#ifndef PEBBLE_TYPES_H
#define PEBBLE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PEBBLE_TRIG_MAX_ANGLE 0x10000
#define PEBBLE_TRIG_MAX_RATIO 0x10000

#define PEBBLE_GCOLOR_BLACK 0xC0u
#define PEBBLE_GCOLOR_WHITE 0xFFu
#define PEBBLE_GCOLOR_CLEAR 0x00u

typedef struct {
    uint8_t argb;
} pebble_gcolor8_t;

static inline uint8_t pebble_gcolor8_alpha(pebble_gcolor8_t c) {
    return (uint8_t)((c.argb >> 6) & 3u);
}

static inline uint8_t pebble_gcolor8_r8(pebble_gcolor8_t c) {
    return (uint8_t)(((c.argb >> 4) & 3u) * 0x55u);
}

static inline uint8_t pebble_gcolor8_g8(pebble_gcolor8_t c) {
    return (uint8_t)(((c.argb >> 2) & 3u) * 0x55u);
}

static inline uint8_t pebble_gcolor8_b8(pebble_gcolor8_t c) {
    return (uint8_t)((c.argb & 3u) * 0x55u);
}

typedef struct {
    int16_t x;
    int16_t y;
} pebble_gpoint_t;

typedef struct {
    int16_t w;
    int16_t h;
} pebble_gsize_t;

typedef struct {
    pebble_gpoint_t origin;
    pebble_gsize_t size;
} pebble_grect_t;

typedef struct {
    uint32_t num_points;
    pebble_gpoint_t *points;
} pebble_gpath_info_t;

typedef struct {
    uint32_t num_points;
    pebble_gpoint_t *points;
    int32_t rotation;
    pebble_gpoint_t offset;
} pebble_gpath_t;

typedef enum {
    PEBBLE_CORNER_NONE = 0,
    PEBBLE_CORNER_TOP_LEFT = 1 << 0,
    PEBBLE_CORNER_TOP_RIGHT = 1 << 1,
    PEBBLE_CORNER_BOTTOM_LEFT = 1 << 2,
    PEBBLE_CORNER_BOTTOM_RIGHT = 1 << 3,
    PEBBLE_CORNERS_ALL = 0x0F,
} pebble_corner_mask_t;

typedef enum {
    PEBBLE_TEXT_ALIGN_LEFT = 0,
    PEBBLE_TEXT_ALIGN_CENTER = 1,
    PEBBLE_TEXT_ALIGN_RIGHT = 2,
} pebble_text_alignment_t;

typedef enum {
    PEBBLE_TEXT_OVERFLOW_WORD_WRAP = 0,
    PEBBLE_TEXT_OVERFLOW_TRAILING_ELLIPSIS = 1,
    PEBBLE_TEXT_OVERFLOW_FILL = 2,
} pebble_text_overflow_t;

typedef enum {
    PEBBLE_SECOND_UNIT = 1 << 0,
    PEBBLE_MINUTE_UNIT = 1 << 1,
    PEBBLE_HOUR_UNIT = 1 << 2,
    PEBBLE_DAY_UNIT = 1 << 3,
    PEBBLE_MONTH_UNIT = 1 << 4,
    PEBBLE_YEAR_UNIT = 1 << 5,
} pebble_time_units_t;

typedef struct {
    int32_t tm_sec;
    int32_t tm_min;
    int32_t tm_hour;
    int32_t tm_mday;
    int32_t tm_mon;
    int32_t tm_year;
    int32_t tm_wday;
    int32_t tm_yday;
    int32_t tm_isdst;
    int32_t tm_gmtoff;
    char tm_zone[8];
} pebble_tm_t;

typedef struct pebble_window pebble_window_t;
typedef struct pebble_layer pebble_layer_t;
typedef struct pebble_text_layer pebble_text_layer_t;
typedef struct pebble_gcontext pebble_gcontext_t;
typedef void *pebble_gfont_t;

typedef void (*pebble_window_handler_t)(pebble_window_t *window);
typedef void (*pebble_layer_update_proc_t)(pebble_layer_t *layer, pebble_gcontext_t *ctx);
typedef void (*pebble_tick_handler_t)(pebble_tm_t *tick_time, uint32_t units_changed);

typedef struct {
    pebble_window_handler_t load;
    pebble_window_handler_t appear;
    pebble_window_handler_t disappear;
    pebble_window_handler_t unload;
} pebble_window_handlers_t;

_Static_assert(sizeof(pebble_gcolor8_t) == 1u, "GColor8 must be 1 byte");
_Static_assert(sizeof(pebble_gpoint_t) == 4u, "GPoint must be 4 bytes");
_Static_assert(sizeof(pebble_gsize_t) == 4u, "GSize must be 4 bytes");
_Static_assert(sizeof(pebble_grect_t) == 8u, "GRect must be 8 bytes");
_Static_assert(sizeof(pebble_gpath_t) == 16u, "GPath must be 16 bytes");
_Static_assert(sizeof(pebble_window_handlers_t) == 16u, "WindowHandlers must be 16 bytes");

#endif /* PEBBLE_TYPES_H */
