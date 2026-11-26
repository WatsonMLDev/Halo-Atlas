#ifndef PEBBLE_TYPES_H
#define PEBBLE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_abi.h"

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

typedef enum {
    PEBBLE_GBITMAP_FORMAT_1BIT = 0,
    PEBBLE_GBITMAP_FORMAT_8BIT = 1,
    PEBBLE_GBITMAP_FORMAT_1BIT_PALETTE = 2,
    PEBBLE_GBITMAP_FORMAT_2BIT_PALETTE = 3,
    PEBBLE_GBITMAP_FORMAT_4BIT_PALETTE = 4,
    PEBBLE_GBITMAP_FORMAT_8BIT_CIRCULAR = 5,
} pebble_gbitmap_format_t;

typedef enum {
    PEBBLE_GCOMP_OP_ASSIGN = 0,
    PEBBLE_GCOMP_OP_ASSIGN_INVERTED = 1,
    PEBBLE_GCOMP_OP_OR = 2,
    PEBBLE_GCOMP_OP_AND = 3,
    PEBBLE_GCOMP_OP_CLEAR = 4,
    PEBBLE_GCOMP_OP_SET = 5,
} pebble_gcomp_op_t;

typedef enum {
    PEBBLE_GALIGN_CENTER = 0,
    PEBBLE_GALIGN_TOP_LEFT = 1,
    PEBBLE_GALIGN_TOP_RIGHT = 2,
    PEBBLE_GALIGN_TOP = 3,
    PEBBLE_GALIGN_LEFT = 4,
    PEBBLE_GALIGN_BOTTOM = 5,
    PEBBLE_GALIGN_RIGHT = 6,
    PEBBLE_GALIGN_BOTTOM_RIGHT = 7,
    PEBBLE_GALIGN_BOTTOM_LEFT = 8,
} pebble_galign_t;

typedef struct {
    uint8_t *data;
    int16_t min_x;
    int16_t max_x;
} pebble_gbitmap_data_row_info_t;

/*
 * First 16 bytes match legacy Pebble SDK 2 GBitmap layout (addr, row_size_bytes,
 * info_flags, bounds) while SDK 3+ apps use opaque gbitmap_* accessors.
 */
typedef struct pebble_gbitmap {
    uint8_t *addr;
    uint16_t row_size_bytes;
    uint16_t info_flags;
    pebble_grect_t bounds;
    pebble_gcolor8_t *palette;
    uint16_t data_width;
    uint16_t data_height;
    bool owns_data;
    bool owns_palette;
} pebble_gbitmap_t;

typedef struct pebble_window pebble_window_t;
typedef struct pebble_layer pebble_layer_t;
typedef struct pebble_text_layer pebble_text_layer_t;
typedef struct pebble_bitmap_layer pebble_bitmap_layer_t;
typedef struct pebble_gcontext pebble_gcontext_t;
typedef struct pebble_app_timer pebble_app_timer_t;
typedef void *pebble_gfont_t;
typedef void *pebble_click_recognizer_ref_t;

typedef enum {
    PEBBLE_BUTTON_ID_BACK = 0,
    PEBBLE_BUTTON_ID_UP = 1,
    PEBBLE_BUTTON_ID_SELECT = 2,
    PEBBLE_BUTTON_ID_DOWN = 3,
    PEBBLE_NUM_BUTTONS = 4,
} pebble_button_id_t;

typedef enum {
    PEBBLE_ACCEL_AXIS_X = 0,
    PEBBLE_ACCEL_AXIS_Y = 1,
    PEBBLE_ACCEL_AXIS_Z = 2,
} pebble_accel_axis_t;

typedef enum {
    PEBBLE_S_SUCCESS = 0,
    PEBBLE_S_TRUE = 1,
    PEBBLE_E_ERROR = -1,
    PEBBLE_E_INVALID_ARGUMENT = -4,
    PEBBLE_E_OUT_OF_MEMORY = -5,
    PEBBLE_E_OUT_OF_STORAGE = -6,
    PEBBLE_E_RANGE = -8,
    PEBBLE_E_DOES_NOT_EXIST = -9,
} pebble_status_code_t;

typedef void (*pebble_window_handler_t)(pebble_window_t *window);
typedef void (*pebble_layer_update_proc_t)(pebble_layer_t *layer, pebble_gcontext_t *ctx);
typedef void (*pebble_tick_handler_t)(pebble_tm_t *tick_time, uint32_t units_changed);
typedef void (*pebble_click_handler_t)(pebble_click_recognizer_ref_t recognizer, void *context);
typedef void (*pebble_click_config_provider_t)(void *context);
typedef void (*pebble_accel_tap_handler_t)(uint8_t axis, int32_t direction);
typedef void (*pebble_app_timer_callback_t)(void *data);

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
_Static_assert(sizeof(pebble_gbitmap_data_row_info_t) == 8u, "GBitmapDataRowInfo must be 8 bytes");
_Static_assert(offsetof(pebble_gbitmap_t, bounds) == 8u, "GBitmap.bounds legacy offset must be 8");

/* --- Dictionary, AppMessage, and AppSync ABI types --- */

enum {
    PEBBLE_DICT_OK = 0u,
    PEBBLE_DICT_NOT_ENOUGH_STORAGE = (1u << 1),
    PEBBLE_DICT_INVALID_ARGS = (1u << 2),
    PEBBLE_DICT_INTERNAL_INCONSISTENCY = (1u << 3),
    PEBBLE_DICT_MALLOC_FAILED = (1u << 4),
};

enum {
    PEBBLE_APP_MSG_OK = 0u,
    PEBBLE_APP_MSG_SEND_TIMEOUT = (1u << 1),
    PEBBLE_APP_MSG_SEND_REJECTED = (1u << 2),
    PEBBLE_APP_MSG_NOT_CONNECTED = (1u << 3),
    PEBBLE_APP_MSG_APP_NOT_RUNNING = (1u << 4),
    PEBBLE_APP_MSG_INVALID_ARGS = (1u << 5),
    PEBBLE_APP_MSG_BUSY = (1u << 6),
    PEBBLE_APP_MSG_BUFFER_OVERFLOW = (1u << 7),
    PEBBLE_APP_MSG_ALREADY_RELEASED = (1u << 9),
    PEBBLE_APP_MSG_CALLBACK_ALREADY_REGISTERED = (1u << 10),
    PEBBLE_APP_MSG_CALLBACK_NOT_REGISTERED = (1u << 11),
    PEBBLE_APP_MSG_OUT_OF_MEMORY = (1u << 12),
    PEBBLE_APP_MSG_CLOSED = (1u << 13),
    PEBBLE_APP_MSG_INTERNAL_ERROR = (1u << 14),
    PEBBLE_APP_MSG_INVALID_STATE = (1u << 15),
};

#define PEBBLE_TUPLE_HEADER_BYTES 7u

typedef struct PEBBLE_PACKED {
    uint32_t key;
    uint8_t type; /* TupleType (-fshort-enums) */
    uint16_t length;
    union {
        uint8_t data[0];
        char cstring[0];
    } value;
} pebble_tuple_t;

typedef struct PEBBLE_PACKED {
    uint8_t count;
    pebble_tuple_t head[0];
} pebble_dict_t;

typedef struct {
    pebble_dict_t *dictionary;
    const void *end;
    pebble_tuple_t *cursor;
} pebble_dict_iterator_t;

typedef void (*pebble_dict_serialize_cb_t)(const uint8_t *data, uint16_t size, void *context);
typedef void (*pebble_dict_key_updated_cb_t)(
    uint32_t key,
    const pebble_tuple_t *new_tuple,
    const pebble_tuple_t *old_tuple,
    void *context);

typedef void (*pebble_app_msg_inbox_received_t)(pebble_dict_iterator_t *iterator, void *context);
typedef void (*pebble_app_msg_inbox_dropped_t)(uint32_t reason, void *context);
typedef void (*pebble_app_msg_outbox_sent_t)(pebble_dict_iterator_t *iterator, void *context);
typedef void (*pebble_app_msg_outbox_failed_t)(
    pebble_dict_iterator_t *iterator,
    uint32_t reason,
    void *context);

typedef void (*pebble_app_sync_tuple_changed_cb_t)(
    uint32_t key,
    const pebble_tuple_t *new_tuple,
    const pebble_tuple_t *old_tuple,
    void *context);
typedef void (*pebble_app_sync_error_cb_t)(
    uint32_t dict_error,
    uint32_t app_message_error,
    void *context);

typedef struct pebble_app_sync {
    pebble_dict_iterator_t current_iter;
    union {
        pebble_dict_t *current;
        uint8_t *buffer;
    };
    uint16_t buffer_size;
    struct {
        pebble_app_sync_tuple_changed_cb_t value_changed;
        pebble_app_sync_error_cb_t error;
        void *context;
    } callback;
    bool is_initialized;
} pebble_app_sync_t;

_Static_assert(sizeof(pebble_tuple_t) == PEBBLE_TUPLE_HEADER_BYTES, "Tuple header must be 7 bytes");
_Static_assert(sizeof(pebble_dict_t) == 1u, "Dictionary header must be 1 byte");
_Static_assert(sizeof(pebble_dict_iterator_t) == 3u * sizeof(void *), "DictionaryIterator size");

#endif /* PEBBLE_TYPES_H */
