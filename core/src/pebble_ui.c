#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_MAX_WINDOW_STACK 8u

struct pebble_layer {
    pebble_grect_t frame;
    pebble_grect_t bounds;
    bool clips;
    bool hidden;
    pebble_layer_update_proc_t update_proc;
    pebble_window_t *window;
    pebble_layer_t *parent;
    pebble_layer_t *first_child;
    pebble_layer_t *next_sibling;
    void *extra_data;
};

struct pebble_text_layer {
    struct pebble_layer layer;
    const char *text;
    pebble_gfont_t font;
    pebble_gcolor8_t text_color;
    pebble_gcolor8_t bg_color;
    uint8_t overflow_mode;
    uint8_t alignment;
};

struct pebble_bitmap_layer {
    struct pebble_layer layer;
    const pebble_gbitmap_t *bitmap;
    pebble_gcolor8_t bg_color;
    uint8_t alignment;
    uint8_t compositing_mode;
};

struct pebble_window {
    struct pebble_layer root_layer;
    pebble_gcolor8_t bg_color;
    pebble_window_handlers_t handlers;
    pebble_click_config_provider_t click_config_provider;
    void *click_config_context;
    bool has_click_config_context;
    void *user_data;
    bool is_loaded;
    bool fullscreen;
};

typedef struct {
    uint8_t button_id;
    uint8_t click_count;
    bool is_repeating;
} pebble_click_recognizer_t;

typedef struct {
    pebble_click_handler_t single_handler;
    pebble_click_handler_t multi_handler;
    uint8_t multi_min_clicks;
    uint8_t multi_max_clicks;
    uint16_t multi_timeout_ms;
    bool multi_last_click_only;
    pebble_click_handler_t long_down_handler;
    pebble_click_handler_t long_up_handler;
    pebble_click_handler_t raw_down_handler;
    pebble_click_handler_t raw_up_handler;
    void *context;
    uint8_t pending_clicks;
    uint64_t pending_deadline_ms;
} pebble_button_config_t;

static pebble_window_t *g_window_stack[PEBBLE_MAX_WINDOW_STACK];
static uint32_t g_window_count;
static bool g_display_dirty;
static uint32_t g_tick_units;
static pebble_tick_handler_t g_tick_handler;
static pebble_tm_t g_last_tick_tm;
static bool g_has_last_tick;
static pebble_accel_tap_handler_t g_accel_tap_handler;
static pebble_button_config_t g_button_cfg[PEBBLE_NUM_BUTTONS];
static bool g_in_click_config;
static uint32_t g_click_config_epoch;

static void clear_button_configs(void *default_ctx) {
    for (uint32_t i = 0u; i < PEBBLE_NUM_BUTTONS; ++i) {
        g_button_cfg[i].single_handler = NULL;
        g_button_cfg[i].multi_handler = NULL;
        g_button_cfg[i].multi_min_clicks = 0u;
        g_button_cfg[i].multi_max_clicks = 0u;
        g_button_cfg[i].multi_timeout_ms = 0u;
        g_button_cfg[i].multi_last_click_only = false;
        g_button_cfg[i].long_down_handler = NULL;
        g_button_cfg[i].long_up_handler = NULL;
        g_button_cfg[i].raw_down_handler = NULL;
        g_button_cfg[i].raw_up_handler = NULL;
        g_button_cfg[i].context = default_ctx;
        g_button_cfg[i].pending_clicks = 0u;
        g_button_cfg[i].pending_deadline_ms = 0u;
    }
}

static void reconfigure_clicks_for_top_window(void) {
    g_click_config_epoch++;
    if (g_window_count == 0u) {
        clear_button_configs(NULL);
        return;
    }
    pebble_window_t *top = g_window_stack[g_window_count - 1u];
    void *ctx = top->has_click_config_context ? top->click_config_context : (void *)top;
    clear_button_configs(ctx);
    if (top->click_config_provider != NULL) {
        g_in_click_config = true;
        top->click_config_provider(ctx);
        g_in_click_config = false;
    }
}

bool pebble_ui_has_windows(void) {
    return g_window_count > 0u;
}

void pebble_ui_reset(void) {
    g_window_count = 0u;
    g_display_dirty = false;
    g_tick_units = 0u;
    g_tick_handler = NULL;
    g_has_last_tick = false;
    g_accel_tap_handler = NULL;
    g_in_click_config = false;
    g_click_config_epoch = 0u;
    clear_button_configs(NULL);
}

static void init_layer(pebble_layer_t *layer, pebble_grect_t frame) {
    layer->frame = frame;
    layer->bounds.origin.x = 0;
    layer->bounds.origin.y = 0;
    layer->bounds.size = frame.size;
    layer->clips = true;
    layer->hidden = false;
    layer->update_proc = NULL;
    layer->window = NULL;
    layer->parent = NULL;
    layer->first_child = NULL;
    layer->next_sibling = NULL;
    layer->extra_data = NULL;
}

static void detach_from_parent(pebble_layer_t *layer) {
    if (layer == NULL || layer->parent == NULL) {
        return;
    }
    pebble_layer_t **pp = &layer->parent->first_child;
    while (*pp != NULL) {
        if (*pp == layer) {
            *pp = layer->next_sibling;
            break;
        }
        pp = &(*pp)->next_sibling;
    }
    layer->parent = NULL;
    layer->next_sibling = NULL;
}

static void propagate_window(pebble_layer_t *layer, pebble_window_t *win) {
    if (layer == NULL) {
        return;
    }
    layer->window = win;
    for (pebble_layer_t *c = layer->first_child; c != NULL; c = c->next_sibling) {
        propagate_window(c, win);
    }
}

static void render_layer_tree(
    pebble_layer_t *layer,
    pebble_gcontext_t *ctx,
    pebble_gpoint_t parent_origin,
    pebble_grect_t parent_clip) {
    if (layer == NULL || layer->hidden) {
        return;
    }

    pebble_gpoint_t frame_origin = {
        .x = (int16_t)(parent_origin.x + layer->frame.origin.x),
        .y = (int16_t)(parent_origin.y + layer->frame.origin.y),
    };
    pebble_grect_t frame_screen = {
        .origin = frame_origin,
        .size = layer->frame.size,
    };
    pebble_grect_t clip = layer->clips ? pebble_rect_intersect(parent_clip, frame_screen) : parent_clip;
    pebble_gpoint_t draw_origin = {
        .x = (int16_t)(frame_origin.x + layer->bounds.origin.x),
        .y = (int16_t)(frame_origin.y + layer->bounds.origin.y),
    };

    if (layer->update_proc != NULL && clip.size.w > 0 && clip.size.h > 0) {
        pebble_gfx_set_layer_transform(ctx, draw_origin, clip);
        layer->update_proc(layer, ctx);
    }

    for (pebble_layer_t *child = layer->first_child; child != NULL; child = child->next_sibling) {
        render_layer_tree(child, ctx, draw_origin, clip);
    }
}

static void render_top_window(const pebble_hal_t *hal) {
    if (g_window_count == 0u) {
        return;
    }
    pebble_window_t *win = g_window_stack[g_window_count - 1u];
    pebble_gcontext_t *ctx = pebble_gfx_begin_frame(win->bg_color);
    int16_t w = 180;
    int16_t h = 180;
    pebble_gfx_get_display_size(&w, &h);
    pebble_gpoint_t root_origin = {0, 0};
    pebble_grect_t full_clip = {.origin = {0, 0}, .size = {w, h}};
    render_layer_tree(&win->root_layer, ctx, root_origin, full_clip);
    pebble_gfx_flush_to_halo(hal);
}

/* --- Layer syscalls --- */

static pebble_layer_t *shim_layer_create(pebble_grect_t frame) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_CREATE, true);
    pebble_layer_t *layer = (pebble_layer_t *)pebble_app_calloc(1u, sizeof(pebble_layer_t));
    if (layer != NULL) {
        init_layer(layer, frame);
    }
    return layer;
}

static pebble_layer_t *shim_layer_create_with_data(pebble_grect_t frame, size_t data_size) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_CREATE_WITH_DATA, true);
    size_t total = sizeof(pebble_layer_t) + data_size;
    pebble_layer_t *layer = (pebble_layer_t *)pebble_app_calloc(1u, total);
    if (layer != NULL) {
        init_layer(layer, frame);
        layer->extra_data = (data_size > 0u) ? (void *)(layer + 1) : NULL;
    }
    return layer;
}

static void shim_layer_destroy(pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_DESTROY, true);
    if (layer == NULL) {
        return;
    }
    detach_from_parent(layer);
    pebble_app_free(layer);
    g_display_dirty = true;
}

static void shim_layer_add_child(pebble_layer_t *parent, pebble_layer_t *child) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_ADD_CHILD, true);
    if (parent == NULL || child == NULL || parent == child) {
        return;
    }
    detach_from_parent(child);
    child->parent = parent;
    pebble_layer_t **pp = &parent->first_child;
    while (*pp != NULL) {
        pp = &(*pp)->next_sibling;
    }
    *pp = child;
    propagate_window(child, parent->window);
    g_display_dirty = true;
}

static void shim_layer_insert_below_sibling(pebble_layer_t *layer, pebble_layer_t *below) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_INSERT_BELOW_SIBLING, true);
    if (layer == NULL || below == NULL || below->parent == NULL || layer == below) {
        return;
    }
    pebble_layer_t *parent = below->parent;
    detach_from_parent(layer);
    layer->parent = parent;
    pebble_layer_t **pp = &parent->first_child;
    while (*pp != NULL && *pp != below) {
        pp = &(*pp)->next_sibling;
    }
    layer->next_sibling = *pp;
    *pp = layer;
    propagate_window(layer, parent->window);
    g_display_dirty = true;
}

static void shim_layer_insert_above_sibling(pebble_layer_t *layer, pebble_layer_t *above) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_INSERT_ABOVE_SIBLING, true);
    if (layer == NULL || above == NULL || above->parent == NULL || layer == above) {
        return;
    }
    pebble_layer_t *parent = above->parent;
    detach_from_parent(layer);
    layer->parent = parent;
    layer->next_sibling = above->next_sibling;
    above->next_sibling = layer;
    propagate_window(layer, parent->window);
    g_display_dirty = true;
}

static void shim_layer_remove_from_parent(pebble_layer_t *child) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_REMOVE_FROM_PARENT, true);
    detach_from_parent(child);
    g_display_dirty = true;
}

static void shim_layer_remove_child_layers(pebble_layer_t *parent) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_REMOVE_CHILD_LAYERS, true);
    if (parent == NULL) {
        return;
    }
    pebble_layer_t *c = parent->first_child;
    while (c != NULL) {
        pebble_layer_t *next = c->next_sibling;
        c->parent = NULL;
        c->next_sibling = NULL;
        c = next;
    }
    parent->first_child = NULL;
    g_display_dirty = true;
}

static pebble_grect_t shim_layer_get_frame(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_FRAME, true);
    pebble_grect_t empty = {{0, 0}, {0, 0}};
    return (layer != NULL) ? layer->frame : empty;
}

static pebble_grect_t shim_layer_get_bounds(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_BOUNDS, true);
    pebble_grect_t empty = {{0, 0}, {0, 0}};
    return (layer != NULL) ? layer->bounds : empty;
}

static pebble_grect_t shim_layer_get_unobstructed_bounds(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_UNOBSTRUCTED_BOUNDS, true);
    pebble_grect_t empty = {{0, 0}, {0, 0}};
    return (layer != NULL) ? layer->bounds : empty;
}

static void shim_layer_set_frame(pebble_layer_t *layer, pebble_grect_t frame) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_SET_FRAME, true);
    if (layer == NULL) return;
    layer->frame = frame;
    layer->bounds.size = frame.size;
    g_display_dirty = true;
}

static void shim_layer_set_bounds(pebble_layer_t *layer, pebble_grect_t bounds) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_SET_BOUNDS, true);
    if (layer == NULL) return;
    layer->bounds = bounds;
    g_display_dirty = true;
}

static void shim_layer_set_update_proc(pebble_layer_t *layer, pebble_layer_update_proc_t proc) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_SET_UPDATE_PROC, true);
    if (layer == NULL) return;
    layer->update_proc = proc;
    g_display_dirty = true;
}

static void shim_layer_mark_dirty(pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_MARK_DIRTY, true);
    (void)layer;
    g_display_dirty = true;
}

static void shim_layer_set_hidden(pebble_layer_t *layer, bool hidden) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_SET_HIDDEN, true);
    if (layer == NULL) return;
    layer->hidden = hidden;
    g_display_dirty = true;
}

static bool shim_layer_get_hidden(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_HIDDEN, true);
    return (layer != NULL) ? layer->hidden : true;
}

static void shim_layer_set_clips(pebble_layer_t *layer, bool clips) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_SET_CLIPS, true);
    if (layer == NULL) return;
    layer->clips = clips;
    g_display_dirty = true;
}

static bool shim_layer_get_clips(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_CLIPS, true);
    return (layer != NULL) ? layer->clips : false;
}

static void *shim_layer_get_data(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_DATA, true);
    return (layer != NULL) ? layer->extra_data : NULL;
}

static pebble_window_t *shim_layer_get_window(const pebble_layer_t *layer) {
    pebble_rt_record_syscall(PEBBLE_SYS_LAYER_GET_WINDOW, true);
    return (layer != NULL) ? layer->window : NULL;
}

/* --- TextLayer & Fonts --- */

static void text_layer_render_proc(pebble_layer_t *layer, pebble_gcontext_t *ctx) {
    pebble_text_layer_t *tl = (pebble_text_layer_t *)layer;
    if (tl == NULL || ctx == NULL) {
        return;
    }
    if (pebble_gcolor8_alpha(tl->bg_color) != 0u) {
        pebble_gfx_fill_box_internal(ctx, layer->bounds, tl->bg_color);
    }
    if (tl->text != NULL) {
        pebble_gfx_set_text_color_internal(ctx, tl->text_color);
        pebble_font_draw_text(
            ctx, tl->text, tl->font, layer->bounds, tl->overflow_mode, tl->alignment);
    }
}

static pebble_text_layer_t *create_text_layer_common(pebble_grect_t frame) {
    pebble_text_layer_t *tl = (pebble_text_layer_t *)pebble_app_calloc(1u, sizeof(pebble_text_layer_t));
    if (tl == NULL) {
        return NULL;
    }
    init_layer(&tl->layer, frame);
    tl->layer.update_proc = text_layer_render_proc;
    tl->text = NULL;
    tl->font = (pebble_gfont_t)(uintptr_t)PEBBLE_FONT_SCALE_1X;
    tl->text_color.argb = PEBBLE_GCOLOR_BLACK;
    tl->bg_color.argb = PEBBLE_GCOLOR_WHITE;
    tl->overflow_mode = PEBBLE_TEXT_OVERFLOW_WORD_WRAP;
    tl->alignment = PEBBLE_TEXT_ALIGN_LEFT;
    return tl;
}

static pebble_gsize_t shim_text_layer_get_content_size(pebble_text_layer_t *tl) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_GET_CONTENT_SIZE, true);
    pebble_gsize_t empty = {0, 0};
    if (tl == NULL || tl->text == NULL) {
        return empty;
    }
    return pebble_font_measure_text(tl->text, tl->font, tl->layer.bounds);
}

static pebble_text_layer_t *shim_text_layer_create(pebble_grect_t frame) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_CREATE, true);
    return create_text_layer_common(frame);
}

static void shim_text_layer_destroy(pebble_text_layer_t *tl) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_DESTROY, true);
    if (tl == NULL) return;
    detach_from_parent(&tl->layer);
    pebble_app_free(tl);
    g_display_dirty = true;
}

static pebble_layer_t *shim_text_layer_get_layer(pebble_text_layer_t *tl) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_GET_LAYER, true);
    return (tl != NULL) ? &tl->layer : NULL;
}

static void shim_text_layer_set_text(pebble_text_layer_t *tl, const char *text) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_TEXT, true);
    if (tl == NULL) return;
    tl->text = text;
    g_display_dirty = true;
}

static const char *shim_text_layer_get_text(pebble_text_layer_t *tl) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_GET_TEXT, true);
    return (tl != NULL) ? tl->text : NULL;
}

static void shim_text_layer_set_bg_color(pebble_text_layer_t *tl, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_BACKGROUND_COLOR, true);
    if (tl == NULL) return;
    tl->bg_color = color;
    g_display_dirty = true;
}

static void shim_text_layer_set_bg_color_2bit(pebble_text_layer_t *tl, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_BACKGROUND_COLOR_2BIT, true);
    if (tl == NULL) return;
    tl->bg_color.argb = pebble_gcolor2_to_8(color);
    g_display_dirty = true;
}

static void shim_text_layer_set_text_color(pebble_text_layer_t *tl, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_TEXT_COLOR, true);
    if (tl == NULL) return;
    tl->text_color = color;
    g_display_dirty = true;
}

static void shim_text_layer_set_text_color_2bit(pebble_text_layer_t *tl, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_TEXT_COLOR_2BIT, true);
    if (tl == NULL) return;
    tl->text_color.argb = pebble_gcolor2_to_8(color);
    g_display_dirty = true;
}

static void shim_text_layer_set_font(pebble_text_layer_t *tl, pebble_gfont_t font) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_FONT, true);
    if (tl == NULL) return;
    tl->font = font;
    g_display_dirty = true;
}

static void shim_text_layer_set_alignment(pebble_text_layer_t *tl, uint8_t alignment) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_TEXT_ALIGNMENT, true);
    if (tl == NULL) return;
    tl->alignment = alignment;
    g_display_dirty = true;
}

static void shim_text_layer_set_overflow(pebble_text_layer_t *tl, uint8_t mode) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_OVERFLOW_MODE, true);
    if (tl == NULL) return;
    tl->overflow_mode = mode;
    g_display_dirty = true;
}

static void shim_text_layer_set_size(pebble_text_layer_t *tl, pebble_gsize_t max_size) {
    pebble_rt_record_syscall(PEBBLE_SYS_TEXT_LAYER_SET_SIZE, true);
    if (tl == NULL) return;
    tl->layer.frame.size = max_size;
    tl->layer.bounds.size = max_size;
    g_display_dirty = true;
}

/* --- BitmapLayer --- */

static void bitmap_layer_render_proc(pebble_layer_t *layer, pebble_gcontext_t *ctx) {
    pebble_bitmap_layer_t *bl = (pebble_bitmap_layer_t *)layer;
    if (bl == NULL || ctx == NULL) {
        return;
    }
    if (pebble_gcolor8_alpha(bl->bg_color) != 0u) {
        pebble_gfx_fill_box_internal(ctx, layer->bounds, bl->bg_color);
    }
    if (bl->bitmap != NULL) {
        pebble_grect_t dest = {.origin = {0, 0}, .size = bl->bitmap->bounds.size};
        pebble_grect_align_internal(&dest, &layer->bounds, bl->alignment, false);
        uint8_t saved_comp = pebble_gfx_get_compositing_mode(ctx);
        pebble_gfx_set_compositing_mode_internal(ctx, bl->compositing_mode);
        pebble_bitmap_draw_in_rect(ctx, bl->bitmap, dest);
        pebble_gfx_set_compositing_mode_internal(ctx, saved_comp);
    }
}

static pebble_bitmap_layer_t *shim_bitmap_layer_create(pebble_grect_t frame) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_CREATE, true);
    pebble_bitmap_layer_t *bl =
        (pebble_bitmap_layer_t *)pebble_app_calloc(1u, sizeof(pebble_bitmap_layer_t));
    if (bl == NULL) {
        return NULL;
    }
    init_layer(&bl->layer, frame);
    bl->layer.update_proc = bitmap_layer_render_proc;
    bl->bitmap = NULL;
    bl->bg_color.argb = PEBBLE_GCOLOR_CLEAR;
    bl->alignment = PEBBLE_GALIGN_CENTER;
    bl->compositing_mode = PEBBLE_GCOMP_OP_ASSIGN;
    return bl;
}

static void shim_bitmap_layer_destroy(pebble_bitmap_layer_t *bl) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_DESTROY, true);
    if (bl == NULL) {
        return;
    }
    detach_from_parent(&bl->layer);
    pebble_app_free(bl);
    g_display_dirty = true;
}

static pebble_layer_t *shim_bitmap_layer_get_layer(const pebble_bitmap_layer_t *bl) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_GET_LAYER, true);
    return (bl != NULL) ? (pebble_layer_t *)&bl->layer : NULL;
}

static const pebble_gbitmap_t *shim_bitmap_layer_get_bitmap(pebble_bitmap_layer_t *bl) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_GET_BITMAP, true);
    return (bl != NULL) ? bl->bitmap : NULL;
}

static void shim_bitmap_layer_set_bitmap(pebble_bitmap_layer_t *bl, const pebble_gbitmap_t *bitmap) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_SET_BITMAP, true);
    if (bl == NULL) {
        return;
    }
    bl->bitmap = bitmap;
    g_display_dirty = true;
}

static void shim_bitmap_layer_set_alignment(pebble_bitmap_layer_t *bl, uint8_t alignment) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_SET_ALIGNMENT, true);
    if (bl == NULL) {
        return;
    }
    bl->alignment = alignment;
    g_display_dirty = true;
}

static void shim_bitmap_layer_set_background_color(pebble_bitmap_layer_t *bl, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_SET_BACKGROUND_COLOR, true);
    if (bl == NULL) {
        return;
    }
    bl->bg_color = color;
    g_display_dirty = true;
}

static void shim_bitmap_layer_set_background_color_2bit(pebble_bitmap_layer_t *bl, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_SET_BACKGROUND_COLOR_2BIT, true);
    if (bl == NULL) {
        return;
    }
    bl->bg_color.argb = pebble_gcolor2_to_8(color);
    g_display_dirty = true;
}

static void shim_bitmap_layer_set_compositing_mode(pebble_bitmap_layer_t *bl, uint8_t mode) {
    pebble_rt_record_syscall(PEBBLE_SYS_BITMAP_LAYER_SET_COMPOSITING_MODE, true);
    if (bl == NULL) {
        return;
    }
    bl->compositing_mode = mode;
    g_display_dirty = true;
}

/* --- Window & Window Stack syscalls --- */

static pebble_window_t *shim_window_create(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_CREATE, true);
    pebble_window_t *win = (pebble_window_t *)pebble_app_calloc(1u, sizeof(pebble_window_t));
    if (win == NULL) {
        return NULL;
    }
    int16_t w = 180;
    int16_t h = 180;
    pebble_gfx_get_display_size(&w, &h);
    pebble_grect_t root_frame = {.origin = {0, 0}, .size = {w, h}};
    init_layer(&win->root_layer, root_frame);
    win->root_layer.window = win;
    win->bg_color.argb = PEBBLE_GCOLOR_WHITE;
    win->fullscreen = true;
    return win;
}

static void shim_window_destroy(pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_DESTROY, true);
    if (win == NULL) return;
    if (win->is_loaded) {
        if (win->handlers.unload != NULL) {
            win->handlers.unload(win);
        }
        win->is_loaded = false;
    }
    pebble_app_free(win);
}

static pebble_layer_t *shim_window_get_root_layer(const pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_GET_ROOT_LAYER, true);
    return (win != NULL) ? (pebble_layer_t *)&win->root_layer : NULL;
}

static void shim_window_set_window_handlers(pebble_window_t *win, pebble_window_handlers_t handlers) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_WINDOW_HANDLERS, true);
    if (win != NULL) {
        win->handlers = handlers;
    }
}

static void shim_window_set_background_color(pebble_window_t *win, pebble_gcolor8_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_BACKGROUND_COLOR, true);
    if (win != NULL) {
        win->bg_color = color;
        g_display_dirty = true;
    }
}

static void shim_window_set_background_color_2bit(pebble_window_t *win, int32_t color) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_BACKGROUND_COLOR_2BIT, true);
    if (win != NULL) {
        win->bg_color.argb = pebble_gcolor2_to_8(color);
        g_display_dirty = true;
    }
}

static bool shim_window_is_loaded(pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_IS_LOADED, true);
    return (win != NULL) ? win->is_loaded : false;
}

static void shim_window_set_user_data(pebble_window_t *win, void *data) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_USER_DATA, true);
    if (win != NULL) win->user_data = data;
}

static void *shim_window_get_user_data(const pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_GET_USER_DATA, true);
    return (win != NULL) ? win->user_data : NULL;
}

static void shim_window_set_fullscreen(pebble_window_t *win, bool enabled) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_FULLSCREEN, true);
    if (win != NULL) win->fullscreen = enabled;
}

static bool shim_window_get_fullscreen(const pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_GET_FULLSCREEN, true);
    return (win != NULL) ? win->fullscreen : true;
}

static void set_click_config_common(
    pebble_window_t *win,
    pebble_click_config_provider_t provider,
    void *context,
    bool has_context) {
    if (win == NULL) {
        return;
    }
    win->click_config_provider = provider;
    win->click_config_context = context;
    win->has_click_config_context = has_context;
    if (g_window_count > 0u && g_window_stack[g_window_count - 1u] == win) {
        reconfigure_clicks_for_top_window();
    }
}

static void shim_window_set_click_config_provider(
    pebble_window_t *win,
    pebble_click_config_provider_t provider) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_CLICK_CONFIG_PROVIDER, true);
    set_click_config_common(win, provider, NULL, false);
}

static void shim_window_set_click_config_provider_with_context(
    pebble_window_t *win,
    pebble_click_config_provider_t provider,
    void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_CLICK_CONFIG_PROVIDER_WITH_CONTEXT, true);
    set_click_config_common(win, provider, context, true);
}

static pebble_click_config_provider_t shim_window_get_click_config_provider(const pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_GET_CLICK_CONFIG_PROVIDER, true);
    return (win != NULL) ? win->click_config_provider : NULL;
}

static void *shim_window_get_click_config_context(pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_GET_CLICK_CONFIG_CONTEXT, true);
    if (win == NULL) {
        return NULL;
    }
    return win->has_click_config_context ? win->click_config_context : NULL;
}

static void shim_window_single_click_subscribe(uint8_t button_id, pebble_click_handler_t handler) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SINGLE_CLICK_SUBSCRIBE, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS) {
        return;
    }
    g_button_cfg[button_id].single_handler = handler;
}

static void shim_window_single_repeating_click_subscribe(
    uint8_t button_id,
    uint16_t repeat_interval_ms,
    pebble_click_handler_t handler) {
    (void)repeat_interval_ms;
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SINGLE_REPEATING_CLICK_SUBSCRIBE, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS || button_id == PEBBLE_BUTTON_ID_BACK) {
        return;
    }
    g_button_cfg[button_id].single_handler = handler;
}

static void shim_window_multi_click_subscribe(
    uint8_t button_id,
    uint8_t min_clicks,
    uint8_t max_clicks,
    uint16_t timeout,
    bool last_click_only,
    pebble_click_handler_t handler) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_MULTI_CLICK_SUBSCRIBE, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS) {
        return;
    }
    uint8_t min_c = (min_clicks == 0u) ? 2u : min_clicks;
    uint8_t max_c = (max_clicks == 0u) ? min_c : ((max_clicks < min_c) ? min_c : max_clicks);
    uint16_t t_ms = (timeout == 0u) ? 300u : timeout;
    g_button_cfg[button_id].multi_handler = handler;
    g_button_cfg[button_id].multi_min_clicks = min_c;
    g_button_cfg[button_id].multi_max_clicks = max_c;
    g_button_cfg[button_id].multi_timeout_ms = t_ms;
    g_button_cfg[button_id].multi_last_click_only = last_click_only;
}

static void shim_window_long_click_subscribe(
    uint8_t button_id,
    uint16_t delay_ms,
    pebble_click_handler_t down_handler,
    pebble_click_handler_t up_handler) {
    (void)delay_ms;
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_LONG_CLICK_SUBSCRIBE, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS || button_id == PEBBLE_BUTTON_ID_BACK) {
        return;
    }
    g_button_cfg[button_id].long_down_handler = down_handler;
    g_button_cfg[button_id].long_up_handler = up_handler;
}

static void shim_accel_tap_service_subscribe(pebble_accel_tap_handler_t handler) {
    pebble_rt_record_syscall(PEBBLE_SYS_ACCEL_TAP_SERVICE_SUBSCRIBE, true);
    g_accel_tap_handler = handler;
}

static void shim_accel_tap_service_unsubscribe(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_ACCEL_TAP_SERVICE_UNSUBSCRIBE, true);
    g_accel_tap_handler = NULL;
}

static void shim_window_raw_click_subscribe(
    uint8_t button_id,
    pebble_click_handler_t down_handler,
    pebble_click_handler_t up_handler,
    void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_RAW_CLICK_SUBSCRIBE, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS || button_id == PEBBLE_BUTTON_ID_BACK) {
        return;
    }
    g_button_cfg[button_id].raw_down_handler = down_handler;
    g_button_cfg[button_id].raw_up_handler = up_handler;
    if (context != NULL) {
        g_button_cfg[button_id].context = context;
    }
}

static void shim_window_set_click_context(uint8_t button_id, void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_SET_CLICK_CONTEXT, true);
    if (!g_in_click_config || button_id >= PEBBLE_NUM_BUTTONS) {
        return;
    }
    g_button_cfg[button_id].context = context;
}

static uint8_t shim_click_recognizer_get_button_id(pebble_click_recognizer_ref_t recognizer) {
    pebble_rt_record_syscall(PEBBLE_SYS_CLICK_RECOGNIZER_GET_BUTTON_ID, true);
    const pebble_click_recognizer_t *r = (const pebble_click_recognizer_t *)recognizer;
    return (r != NULL) ? r->button_id : 0u;
}

static uint8_t shim_click_number_of_clicks_counted(pebble_click_recognizer_ref_t recognizer) {
    pebble_rt_record_syscall(PEBBLE_SYS_CLICK_NUMBER_OF_CLICKS_COUNTED, true);
    const pebble_click_recognizer_t *r = (const pebble_click_recognizer_t *)recognizer;
    return (r != NULL) ? r->click_count : 0u;
}

static bool shim_click_recognizer_is_repeating(pebble_click_recognizer_ref_t recognizer) {
    pebble_rt_record_syscall(PEBBLE_SYS_CLICK_RECOGNIZER_IS_REPEATING, true);
    const pebble_click_recognizer_t *r = (const pebble_click_recognizer_t *)recognizer;
    return (r != NULL) ? r->is_repeating : false;
}

static void shim_window_stack_push(pebble_window_t *win, bool animated) {
    (void)animated;
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_PUSH, true);
    if (win == NULL || g_window_count >= PEBBLE_MAX_WINDOW_STACK) {
        return;
    }
    if (g_window_count > 0u) {
        pebble_window_t *prev = g_window_stack[g_window_count - 1u];
        if (prev->handlers.disappear != NULL) {
            prev->handlers.disappear(prev);
        }
    }
    g_window_stack[g_window_count++] = win;
    if (!win->is_loaded) {
        win->is_loaded = true;
        if (win->handlers.load != NULL) {
            win->handlers.load(win);
        }
    }
    reconfigure_clicks_for_top_window();
    if (win->handlers.appear != NULL) {
        win->handlers.appear(win);
    }
    g_display_dirty = true;
}

static pebble_window_t *shim_window_stack_pop(bool animated) {
    (void)animated;
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_POP, true);
    if (g_window_count == 0u) {
        return NULL;
    }
    pebble_window_t *win = g_window_stack[--g_window_count];
    if (win->handlers.disappear != NULL) {
        win->handlers.disappear(win);
    }
    if (win->is_loaded) {
        win->is_loaded = false;
        if (win->handlers.unload != NULL) {
            win->handlers.unload(win);
        }
    }
    reconfigure_clicks_for_top_window();
    if (g_window_count > 0u) {
        pebble_window_t *next = g_window_stack[g_window_count - 1u];
        if (next->handlers.appear != NULL) {
            next->handlers.appear(next);
        }
    }
    g_display_dirty = true;
    return win;
}

static void shim_window_stack_pop_all(bool animated) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_POP_ALL, true);
    while (g_window_count > 0u) {
        (void)shim_window_stack_pop(animated);
    }
}

static pebble_window_t *shim_window_stack_get_top_window(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_GET_TOP_WINDOW, true);
    return (g_window_count > 0u) ? g_window_stack[g_window_count - 1u] : NULL;
}

static bool shim_window_stack_contains_window(pebble_window_t *win) {
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_CONTAINS_WINDOW, true);
    for (uint32_t i = 0u; i < g_window_count; ++i) {
        if (g_window_stack[i] == win) {
            return true;
        }
    }
    return false;
}

static bool shim_window_stack_remove(pebble_window_t *win, bool animated) {
    (void)animated;
    pebble_rt_record_syscall(PEBBLE_SYS_WINDOW_STACK_REMOVE, true);
    if (win == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < g_window_count; ++i) {
        if (g_window_stack[i] != win) {
            continue;
        }
        bool was_top = (i == g_window_count - 1u);
        for (uint32_t j = i; j + 1u < g_window_count; ++j) {
            g_window_stack[j] = g_window_stack[j + 1u];
        }
        g_window_count--;
        if (was_top && win->handlers.disappear != NULL) {
            win->handlers.disappear(win);
        }
        if (win->is_loaded) {
            win->is_loaded = false;
            if (win->handlers.unload != NULL) {
                win->handlers.unload(win);
            }
        }
        if (was_top) {
            reconfigure_clicks_for_top_window();
            if (g_window_count > 0u) {
                pebble_window_t *next = g_window_stack[g_window_count - 1u];
                if (next->handlers.appear != NULL) {
                    next->handlers.appear(next);
                }
            }
        }
        g_display_dirty = true;
        return true;
    }
    return false;
}

/* --- Tick Timer Service & Event Loop --- */

static uint32_t diff_time_units(const pebble_tm_t *prev, const pebble_tm_t *now) {
    uint32_t mask = 0u;
    if (prev->tm_year != now->tm_year) {
        mask |= PEBBLE_YEAR_UNIT | PEBBLE_MONTH_UNIT | PEBBLE_DAY_UNIT |
                PEBBLE_HOUR_UNIT | PEBBLE_MINUTE_UNIT | PEBBLE_SECOND_UNIT;
    } else if (prev->tm_mon != now->tm_mon) {
        mask |= PEBBLE_MONTH_UNIT | PEBBLE_DAY_UNIT | PEBBLE_HOUR_UNIT |
                PEBBLE_MINUTE_UNIT | PEBBLE_SECOND_UNIT;
    } else if (prev->tm_mday != now->tm_mday) {
        mask |= PEBBLE_DAY_UNIT | PEBBLE_HOUR_UNIT | PEBBLE_MINUTE_UNIT | PEBBLE_SECOND_UNIT;
    } else if (prev->tm_hour != now->tm_hour) {
        mask |= PEBBLE_HOUR_UNIT | PEBBLE_MINUTE_UNIT | PEBBLE_SECOND_UNIT;
    } else if (prev->tm_min != now->tm_min) {
        mask |= PEBBLE_MINUTE_UNIT | PEBBLE_SECOND_UNIT;
    } else if (prev->tm_sec != now->tm_sec) {
        mask |= PEBBLE_SECOND_UNIT;
    }
    return mask;
}

static void shim_tick_timer_subscribe(uint32_t tick_units, pebble_tick_handler_t handler) {
    pebble_rt_record_syscall(PEBBLE_SYS_TICK_TIMER_SERVICE_SUBSCRIBE, true);
    g_tick_units = tick_units;
    g_tick_handler = handler;
    /* Deliver the initial tick when app_event_loop starts (after window_load completes). */
    g_has_last_tick = false;
}

static void shim_tick_timer_unsubscribe(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_TICK_TIMER_SERVICE_UNSUBSCRIBE, true);
    g_tick_units = 0u;
    g_tick_handler = NULL;
    g_has_last_tick = false;
}

static void invoke_click_handler(
    pebble_click_handler_t handler,
    uint8_t button_id,
    uint8_t click_count,
    bool is_repeating,
    void *ctx) {
    if (handler == NULL) {
        return;
    }
    pebble_click_recognizer_t rec = {
        .button_id = button_id,
        .click_count = click_count,
        .is_repeating = is_repeating,
    };
    handler(&rec, ctx);
}

static bool button_has_long_handler(uint8_t button_id) {
    if (button_id >= PEBBLE_NUM_BUTTONS) {
        return false;
    }
    return g_button_cfg[button_id].long_down_handler != NULL ||
           g_button_cfg[button_id].long_up_handler != NULL;
}

static void dispatch_button_long_click(uint8_t button_id) {
    if (button_id >= PEBBLE_NUM_BUTTONS || g_window_count == 0u) {
        return;
    }
    uint32_t epoch = g_click_config_epoch;
    pebble_button_config_t *cfg = &g_button_cfg[button_id];
    cfg->pending_clicks = 0u;
    cfg->pending_deadline_ms = 0u;
    void *ctx = cfg->context;
    pebble_click_handler_t down_h = cfg->long_down_handler;
    pebble_click_handler_t up_h = cfg->long_up_handler;
    if (down_h != NULL) {
        invoke_click_handler(down_h, button_id, 1u, false, ctx);
        if (g_click_config_epoch != epoch || g_window_count == 0u) {
            return;
        }
    }
    if (up_h != NULL) {
        invoke_click_handler(up_h, button_id, 1u, false, ctx);
    }
}

static void dispatch_button_click(uint8_t button_id, uint64_t now_ms) {
    if (button_id >= PEBBLE_NUM_BUTTONS || g_window_count == 0u) {
        return;
    }
    uint32_t epoch = g_click_config_epoch;
    pebble_button_config_t *cfg = &g_button_cfg[button_id];
    void *ctx = cfg->context;
    pebble_click_handler_t raw_down = cfg->raw_down_handler;
    pebble_click_handler_t raw_up = cfg->raw_up_handler;

    if (raw_down != NULL) {
        invoke_click_handler(raw_down, button_id, 1u, false, ctx);
        if (g_click_config_epoch != epoch || g_window_count == 0u) {
            return;
        }
    }
    if (raw_up != NULL) {
        invoke_click_handler(raw_up, button_id, 1u, false, ctx);
        if (g_click_config_epoch != epoch || g_window_count == 0u) {
            return;
        }
    }

    if (cfg->multi_handler != NULL) {
        if (cfg->pending_clicks < 255u) {
            cfg->pending_clicks++;
        }
        uint8_t count = cfg->pending_clicks;
        pebble_click_handler_t multi_h = cfg->multi_handler;
        bool reached_max = (count >= cfg->multi_max_clicks);
        bool fire_now = (count >= cfg->multi_min_clicks) &&
                        (!cfg->multi_last_click_only || reached_max);
        /* Commit pending-click state BEFORE invoking callback to stay re-entrancy safe. */
        if (reached_max) {
            cfg->pending_clicks = 0u;
            cfg->pending_deadline_ms = 0u;
        } else {
            cfg->pending_deadline_ms = now_ms + (uint64_t)cfg->multi_timeout_ms;
        }
        if (fire_now) {
            invoke_click_handler(multi_h, button_id, count, false, ctx);
        }
        return;
    }

    if (cfg->single_handler != NULL) {
        invoke_click_handler(cfg->single_handler, button_id, 1u, false, ctx);
        return;
    }

    if (button_id == PEBBLE_BUTTON_ID_BACK) {
        (void)shim_window_stack_pop(true);
    }
}

static void dispatch_button_event(uint8_t button_id, bool is_long, uint64_t now_ms) {
    if (is_long && button_has_long_handler(button_id)) {
        dispatch_button_long_click(button_id);
    } else {
        dispatch_button_click(button_id, now_ms);
    }
}

static void flush_expired_multi_clicks(uint64_t now_ms) {
    uint32_t epoch = g_click_config_epoch;
    for (uint8_t b = 0u; b < PEBBLE_NUM_BUTTONS && g_window_count > 0u; ++b) {
        pebble_button_config_t *cfg = &g_button_cfg[b];
        if (cfg->pending_clicks == 0u || now_ms < cfg->pending_deadline_ms) {
            continue;
        }
        uint8_t count = cfg->pending_clicks;
        void *ctx = cfg->context;
        pebble_click_handler_t multi_h = cfg->multi_handler;
        pebble_click_handler_t single_h = cfg->single_handler;
        uint8_t min_c = cfg->multi_min_clicks;
        bool last_only = cfg->multi_last_click_only;
        cfg->pending_clicks = 0u;
        cfg->pending_deadline_ms = 0u;

        if (multi_h != NULL && count >= min_c) {
            if (last_only) {
                invoke_click_handler(multi_h, b, count, false, ctx);
            }
        } else if (count == 1u) {
            if (single_h != NULL) {
                invoke_click_handler(single_h, b, 1u, false, ctx);
            } else if (b == PEBBLE_BUTTON_ID_BACK) {
                (void)shim_window_stack_pop(true);
            }
        }
        if (g_click_config_epoch != epoch) {
            break;
        }
    }
}

static bool dispatch_hal_event(const pebble_hal_event_t *ev, uint64_t now_ms) {
    if (ev == NULL) {
        return true;
    }
    bool is_long = (ev->value == PEBBLE_HAL_BTN_PRESS_LONG);
    switch (ev->type) {
        case PEBBLE_HAL_EVENT_QUIT:
            return false;
        case PEBBLE_HAL_EVENT_BUTTON_SELECT:
            dispatch_button_event(PEBBLE_BUTTON_ID_SELECT, is_long, now_ms);
            break;
        case PEBBLE_HAL_EVENT_BUTTON_BACK:
            /*
             * Halo has one physical button: Long-Press maps to BUTTON_ID_BACK by default,
             * and routes to BUTTON_ID_SELECT long-click when the active window explicitly
             * subscribed to Select long-click (Double-Click always remains BUTTON_ID_BACK).
             */
            if (is_long && button_has_long_handler(PEBBLE_BUTTON_ID_SELECT)) {
                dispatch_button_long_click(PEBBLE_BUTTON_ID_SELECT);
            } else {
                dispatch_button_click(PEBBLE_BUTTON_ID_BACK, now_ms);
            }
            break;
        case PEBBLE_HAL_EVENT_BUTTON_UP:
            dispatch_button_event(PEBBLE_BUTTON_ID_UP, is_long, now_ms);
            break;
        case PEBBLE_HAL_EVENT_BUTTON_DOWN:
            dispatch_button_event(PEBBLE_BUTTON_ID_DOWN, is_long, now_ms);
            break;
        case PEBBLE_HAL_EVENT_IMU_TAP_SINGLE:
            /* Halo control scheme: Single Temple Tap -> BUTTON_ID_DOWN + accel_tap_service */
            dispatch_button_click(PEBBLE_BUTTON_ID_DOWN, now_ms);
            if (g_accel_tap_handler != NULL && g_window_count > 0u) {
                g_accel_tap_handler(PEBBLE_ACCEL_AXIS_Z, 1);
            }
            break;
        case PEBBLE_HAL_EVENT_IMU_TAP_DOUBLE:
            /* Halo control scheme: Double Temple Tap -> BUTTON_ID_UP + accel_tap_service */
            dispatch_button_click(PEBBLE_BUTTON_ID_UP, now_ms);
            if (g_accel_tap_handler != NULL && g_window_count > 0u) {
                g_accel_tap_handler(PEBBLE_ACCEL_AXIS_Z, -1);
            }
            break;
        case PEBBLE_HAL_EVENT_IMU_TAP_TRIPLE:
            if (g_accel_tap_handler != NULL && g_window_count > 0u) {
                g_accel_tap_handler(PEBBLE_ACCEL_AXIS_Y, 1);
            }
            break;
        case PEBBLE_HAL_EVENT_NONE:
        default:
            break;
    }
    return true;
}

static void teardown_all_windows(void) {
    bool is_top = true;
    while (g_window_count > 0u) {
        pebble_window_t *win = g_window_stack[--g_window_count];
        if (is_top && win->handlers.disappear != NULL) {
            win->handlers.disappear(win);
        }
        is_top = false;
        if (win->is_loaded) {
            win->is_loaded = false;
            if (win->handlers.unload != NULL) {
                win->handlers.unload(win);
            }
        }
    }
}

static void shim_app_event_loop(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_EVENT_LOOP, true);
    const pebble_hal_t *hal = pebble_rt_hal();
    const pebble_run_opts_t *opts = pebble_rt_opts();
    if (hal == NULL || opts == NULL) {
        return;
    }

    if (g_tick_handler != NULL && !g_has_last_tick) {
        pebble_tm_t now_tm;
        pebble_rt_epoch_to_tm(pebble_rt_current_epoch(), &now_tm);
        g_last_tick_tm = now_tm;
        g_has_last_tick = true;
        g_tick_handler(&now_tm, g_tick_units);
    }

    uint32_t frames = 0u;
    while (g_window_count > 0u) {
        if (g_display_dirty || frames == 0u) {
            render_top_window(hal);
            g_display_dirty = false;
            frames++;
            if (opts->max_frames > 0u && frames >= opts->max_frames) {
                break;
            }
        }

        if (hal->poll_event != NULL) {
            pebble_hal_event_t ev;
            while (hal->poll_event(hal->ctx, &ev)) {
                if (!dispatch_hal_event(&ev, pebble_rt_mono_ms())) {
                    goto exit_loop;
                }
                if (g_window_count == 0u) {
                    goto exit_loop;
                }
            }
        }

        flush_expired_multi_clicks(pebble_rt_mono_ms());
        if (g_window_count == 0u) {
            goto exit_loop;
        }

        pebble_timer_process(pebble_rt_mono_ms());
        if (g_window_count == 0u) {
            goto exit_loop;
        }

        if (opts->sim_tick_step_sec > 0) {
            pebble_rt_advance_sim_epoch(opts->sim_tick_step_sec);
        }

        if (g_tick_handler != NULL) {
            pebble_tm_t now_tm;
            pebble_rt_epoch_to_tm(pebble_rt_current_epoch(), &now_tm);
            uint32_t changed = g_has_last_tick ? diff_time_units(&g_last_tick_tm, &now_tm) : g_tick_units;
            if ((changed & g_tick_units) != 0u) {
                g_last_tick_tm = now_tm;
                g_has_last_tick = true;
                g_tick_handler(&now_tm, changed);
            }
        } else if (opts->sim_tick_step_sec > 0) {
            /* Even apps without tick_timer_service advance deterministically when stepped. */
            g_display_dirty = true;
        }
    }

exit_loop:
    teardown_all_windows();
    pebble_timer_reset();
    g_tick_units = 0u;
    g_tick_handler = NULL;
    g_has_last_tick = false;
    g_accel_tap_handler = NULL;
    clear_button_configs(NULL);
}

void pebble_ui_register_syscalls(void **table) {
    table[PEBBLE_SYS_APP_EVENT_LOOP] = (void *)shim_app_event_loop;
    table[PEBBLE_SYS_TICK_TIMER_SERVICE_SUBSCRIBE] = (void *)shim_tick_timer_subscribe;
    table[PEBBLE_SYS_TICK_TIMER_SERVICE_UNSUBSCRIBE] = (void *)shim_tick_timer_unsubscribe;

    table[PEBBLE_SYS_WINDOW_CREATE] = (void *)shim_window_create;
    table[PEBBLE_SYS_WINDOW_DESTROY] = (void *)shim_window_destroy;
    table[PEBBLE_SYS_WINDOW_GET_ROOT_LAYER] = (void *)shim_window_get_root_layer;
    table[PEBBLE_SYS_WINDOW_SET_WINDOW_HANDLERS] = (void *)shim_window_set_window_handlers;
    table[PEBBLE_SYS_WINDOW_SET_BACKGROUND_COLOR] = (void *)shim_window_set_background_color;
    table[PEBBLE_SYS_WINDOW_SET_BACKGROUND_COLOR_2BIT] = (void *)shim_window_set_background_color_2bit;
    table[PEBBLE_SYS_WINDOW_IS_LOADED] = (void *)shim_window_is_loaded;
    table[PEBBLE_SYS_WINDOW_SET_USER_DATA] = (void *)shim_window_set_user_data;
    table[PEBBLE_SYS_WINDOW_GET_USER_DATA] = (void *)shim_window_get_user_data;
    table[PEBBLE_SYS_WINDOW_SET_FULLSCREEN] = (void *)shim_window_set_fullscreen;
    table[PEBBLE_SYS_WINDOW_GET_FULLSCREEN] = (void *)shim_window_get_fullscreen;
    table[PEBBLE_SYS_WINDOW_SET_CLICK_CONFIG_PROVIDER] = (void *)shim_window_set_click_config_provider;
    table[PEBBLE_SYS_WINDOW_SET_CLICK_CONFIG_PROVIDER_WITH_CONTEXT] =
        (void *)shim_window_set_click_config_provider_with_context;
    table[PEBBLE_SYS_WINDOW_GET_CLICK_CONFIG_PROVIDER] = (void *)shim_window_get_click_config_provider;
    table[PEBBLE_SYS_WINDOW_GET_CLICK_CONFIG_CONTEXT] = (void *)shim_window_get_click_config_context;
    table[PEBBLE_SYS_WINDOW_SINGLE_CLICK_SUBSCRIBE] = (void *)shim_window_single_click_subscribe;
    table[PEBBLE_SYS_WINDOW_SINGLE_REPEATING_CLICK_SUBSCRIBE] =
        (void *)shim_window_single_repeating_click_subscribe;
    table[PEBBLE_SYS_WINDOW_MULTI_CLICK_SUBSCRIBE] = (void *)shim_window_multi_click_subscribe;
    table[PEBBLE_SYS_WINDOW_LONG_CLICK_SUBSCRIBE] = (void *)shim_window_long_click_subscribe;
    table[PEBBLE_SYS_WINDOW_RAW_CLICK_SUBSCRIBE] = (void *)shim_window_raw_click_subscribe;
    table[PEBBLE_SYS_WINDOW_SET_CLICK_CONTEXT] = (void *)shim_window_set_click_context;
    table[PEBBLE_SYS_CLICK_RECOGNIZER_GET_BUTTON_ID] = (void *)shim_click_recognizer_get_button_id;
    table[PEBBLE_SYS_CLICK_NUMBER_OF_CLICKS_COUNTED] = (void *)shim_click_number_of_clicks_counted;
    table[PEBBLE_SYS_CLICK_RECOGNIZER_IS_REPEATING] = (void *)shim_click_recognizer_is_repeating;
    table[PEBBLE_SYS_ACCEL_TAP_SERVICE_SUBSCRIBE] = (void *)shim_accel_tap_service_subscribe;
    table[PEBBLE_SYS_ACCEL_TAP_SERVICE_UNSUBSCRIBE] = (void *)shim_accel_tap_service_unsubscribe;
    table[PEBBLE_SYS_WINDOW_STACK_PUSH] = (void *)shim_window_stack_push;
    table[PEBBLE_SYS_WINDOW_STACK_POP] = (void *)shim_window_stack_pop;
    table[PEBBLE_SYS_WINDOW_STACK_POP_ALL] = (void *)shim_window_stack_pop_all;
    table[PEBBLE_SYS_WINDOW_STACK_GET_TOP_WINDOW] = (void *)shim_window_stack_get_top_window;
    table[PEBBLE_SYS_WINDOW_STACK_CONTAINS_WINDOW] = (void *)shim_window_stack_contains_window;
    table[PEBBLE_SYS_WINDOW_STACK_REMOVE] = (void *)shim_window_stack_remove;

    table[PEBBLE_SYS_LAYER_CREATE] = (void *)shim_layer_create;
    table[PEBBLE_SYS_LAYER_CREATE_WITH_DATA] = (void *)shim_layer_create_with_data;
    table[PEBBLE_SYS_LAYER_DESTROY] = (void *)shim_layer_destroy;
    table[PEBBLE_SYS_LAYER_ADD_CHILD] = (void *)shim_layer_add_child;
    table[PEBBLE_SYS_LAYER_INSERT_BELOW_SIBLING] = (void *)shim_layer_insert_below_sibling;
    table[PEBBLE_SYS_LAYER_INSERT_ABOVE_SIBLING] = (void *)shim_layer_insert_above_sibling;
    table[PEBBLE_SYS_LAYER_REMOVE_FROM_PARENT] = (void *)shim_layer_remove_from_parent;
    table[PEBBLE_SYS_LAYER_REMOVE_CHILD_LAYERS] = (void *)shim_layer_remove_child_layers;
    table[PEBBLE_SYS_LAYER_GET_FRAME] = (void *)shim_layer_get_frame;
    table[PEBBLE_SYS_LAYER_GET_BOUNDS] = (void *)shim_layer_get_bounds;
    table[PEBBLE_SYS_LAYER_GET_UNOBSTRUCTED_BOUNDS] = (void *)shim_layer_get_unobstructed_bounds;
    table[PEBBLE_SYS_LAYER_SET_FRAME] = (void *)shim_layer_set_frame;
    table[PEBBLE_SYS_LAYER_SET_BOUNDS] = (void *)shim_layer_set_bounds;
    table[PEBBLE_SYS_LAYER_SET_UPDATE_PROC] = (void *)shim_layer_set_update_proc;
    table[PEBBLE_SYS_LAYER_MARK_DIRTY] = (void *)shim_layer_mark_dirty;
    table[PEBBLE_SYS_LAYER_SET_HIDDEN] = (void *)shim_layer_set_hidden;
    table[PEBBLE_SYS_LAYER_GET_HIDDEN] = (void *)shim_layer_get_hidden;
    table[PEBBLE_SYS_LAYER_SET_CLIPS] = (void *)shim_layer_set_clips;
    table[PEBBLE_SYS_LAYER_GET_CLIPS] = (void *)shim_layer_get_clips;
    table[PEBBLE_SYS_LAYER_GET_DATA] = (void *)shim_layer_get_data;
    table[PEBBLE_SYS_LAYER_GET_WINDOW] = (void *)shim_layer_get_window;

    table[PEBBLE_SYS_TEXT_LAYER_CREATE] = (void *)shim_text_layer_create;
    table[PEBBLE_SYS_TEXT_LAYER_DESTROY] = (void *)shim_text_layer_destroy;
    table[PEBBLE_SYS_TEXT_LAYER_GET_CONTENT_SIZE] = (void *)shim_text_layer_get_content_size;
    table[PEBBLE_SYS_TEXT_LAYER_GET_LAYER] = (void *)shim_text_layer_get_layer;
    table[PEBBLE_SYS_TEXT_LAYER_SET_TEXT] = (void *)shim_text_layer_set_text;
    table[PEBBLE_SYS_TEXT_LAYER_GET_TEXT] = (void *)shim_text_layer_get_text;
    table[PEBBLE_SYS_TEXT_LAYER_SET_BACKGROUND_COLOR] = (void *)shim_text_layer_set_bg_color;
    table[PEBBLE_SYS_TEXT_LAYER_SET_TEXT_COLOR] = (void *)shim_text_layer_set_text_color;
    table[PEBBLE_SYS_TEXT_LAYER_SET_FONT] = (void *)shim_text_layer_set_font;
    table[PEBBLE_SYS_TEXT_LAYER_SET_TEXT_ALIGNMENT] = (void *)shim_text_layer_set_alignment;
    table[PEBBLE_SYS_TEXT_LAYER_SET_OVERFLOW_MODE] = (void *)shim_text_layer_set_overflow;
    table[PEBBLE_SYS_TEXT_LAYER_SET_SIZE] = (void *)shim_text_layer_set_size;

    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_CREATE] = (void *)shim_text_layer_create;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_DESTROY] = (void *)shim_text_layer_destroy;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_GET_CONTENT_SIZE] = (void *)shim_text_layer_get_content_size;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_GET_LAYER] = (void *)shim_text_layer_get_layer;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_TEXT] = (void *)shim_text_layer_set_text;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_GET_TEXT] = (void *)shim_text_layer_get_text;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_BACKGROUND_COLOR_2BIT] =
        (void *)shim_text_layer_set_bg_color_2bit;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_TEXT_COLOR_2BIT] = (void *)shim_text_layer_set_text_color_2bit;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_FONT] = (void *)shim_text_layer_set_font;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_TEXT_ALIGNMENT] = (void *)shim_text_layer_set_alignment;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_OVERFLOW_MODE] = (void *)shim_text_layer_set_overflow;
    table[PEBBLE_SYS_TEXT_LAYER_LEGACY2_SET_SIZE] = (void *)shim_text_layer_set_size;

    table[PEBBLE_SYS_BITMAP_LAYER_CREATE] = (void *)shim_bitmap_layer_create;
    table[PEBBLE_SYS_BITMAP_LAYER_DESTROY] = (void *)shim_bitmap_layer_destroy;
    table[PEBBLE_SYS_BITMAP_LAYER_GET_LAYER] = (void *)shim_bitmap_layer_get_layer;
    table[PEBBLE_SYS_BITMAP_LAYER_GET_BITMAP] = (void *)shim_bitmap_layer_get_bitmap;
    table[PEBBLE_SYS_BITMAP_LAYER_SET_BITMAP] = (void *)shim_bitmap_layer_set_bitmap;
    table[PEBBLE_SYS_BITMAP_LAYER_SET_ALIGNMENT] = (void *)shim_bitmap_layer_set_alignment;
    table[PEBBLE_SYS_BITMAP_LAYER_SET_BACKGROUND_COLOR] = (void *)shim_bitmap_layer_set_background_color;
    table[PEBBLE_SYS_BITMAP_LAYER_SET_BACKGROUND_COLOR_2BIT] =
        (void *)shim_bitmap_layer_set_background_color_2bit;
    table[PEBBLE_SYS_BITMAP_LAYER_SET_COMPOSITING_MODE] = (void *)shim_bitmap_layer_set_compositing_mode;
}
