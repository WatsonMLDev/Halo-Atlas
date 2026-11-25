#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_MAX_TIMERS 64u
#define PEBBLE_TIMER_SLOT_MASK 0xFFu
#define PEBBLE_TIMER_GEN_SHIFT 8u

typedef struct {
    bool active;
    uint32_t generation;
    uint64_t deadline_ms;
    pebble_app_timer_callback_t callback;
    void *data;
} pebble_timer_slot_t;

static pebble_timer_slot_t g_timers[PEBBLE_MAX_TIMERS];
static uint32_t g_next_gen;

static pebble_app_timer_t *encode_handle(uint32_t idx, uint32_t gen) {
    uintptr_t raw = ((uintptr_t)gen << PEBBLE_TIMER_GEN_SHIFT) | (uintptr_t)(idx + 1u);
    return (pebble_app_timer_t *)raw;
}

static pebble_timer_slot_t *decode_active_handle(pebble_app_timer_t *handle) {
    if (handle == NULL) {
        return NULL;
    }
    uintptr_t raw = (uintptr_t)handle;
    uint32_t slot_one_based = (uint32_t)(raw & PEBBLE_TIMER_SLOT_MASK);
    uint32_t gen = (uint32_t)(raw >> PEBBLE_TIMER_GEN_SHIFT);
    if (slot_one_based == 0u || slot_one_based > PEBBLE_MAX_TIMERS || gen == 0u) {
        return NULL;
    }
    pebble_timer_slot_t *slot = &g_timers[slot_one_based - 1u];
    if (!slot->active || slot->generation != gen) {
        return NULL;
    }
    return slot;
}

void pebble_timer_reset(void) {
    for (uint32_t i = 0u; i < PEBBLE_MAX_TIMERS; ++i) {
        g_timers[i].active = false;
        g_timers[i].generation = 0u;
        g_timers[i].deadline_ms = 0u;
        g_timers[i].callback = NULL;
        g_timers[i].data = NULL;
    }
    g_next_gen = 1u;
}

void pebble_timer_process(uint64_t now_ms) {
    /*
     * Snapshot timers already due at entry (with their generation) so a callback
     * registering a 0ms timer cannot starve input polling or display rendering.
     */
    uint32_t due_gen[PEBBLE_MAX_TIMERS];
    uint32_t due_count = 0u;
    for (uint32_t i = 0u; i < PEBBLE_MAX_TIMERS; ++i) {
        if (g_timers[i].active && g_timers[i].deadline_ms <= now_ms) {
            due_gen[i] = g_timers[i].generation;
            due_count++;
        } else {
            due_gen[i] = 0u;
        }
    }

    for (uint32_t fired = 0u; fired < due_count && pebble_ui_has_windows(); ++fired) {
        int32_t earliest = -1;
        uint64_t earliest_deadline = 0u;
        for (uint32_t i = 0u; i < PEBBLE_MAX_TIMERS; ++i) {
            if (due_gen[i] == 0u || !g_timers[i].active || g_timers[i].generation != due_gen[i] ||
                g_timers[i].deadline_ms > now_ms) {
                continue;
            }
            if (earliest < 0 || g_timers[i].deadline_ms < earliest_deadline) {
                earliest = (int32_t)i;
                earliest_deadline = g_timers[i].deadline_ms;
            }
        }
        if (earliest < 0) {
            break;
        }
        uint32_t idx = (uint32_t)earliest;
        due_gen[idx] = 0u;
        pebble_timer_slot_t *slot = &g_timers[idx];
        pebble_app_timer_callback_t cb = slot->callback;
        void *data = slot->data;
        /* Disarm before invoking callback so elapsed handles cannot be rescheduled inside cb. */
        slot->active = false;
        slot->callback = NULL;
        slot->data = NULL;
        if (cb != NULL) {
            cb(data);
        }
    }
}

static pebble_app_timer_t *shim_app_timer_register(
    uint32_t timeout_ms,
    pebble_app_timer_callback_t callback,
    void *callback_data) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_TIMER_REGISTER, true);
    if (callback == NULL) {
        return NULL;
    }
    for (uint32_t i = 0u; i < PEBBLE_MAX_TIMERS; ++i) {
        if (!g_timers[i].active) {
            uint32_t gen = g_next_gen++;
            if (gen == 0u || gen >= 0x00FFFFFFu) {
                g_next_gen = 1u;
                gen = g_next_gen++;
            }
            g_timers[i].active = true;
            g_timers[i].generation = gen;
            g_timers[i].deadline_ms = pebble_rt_mono_ms() + (uint64_t)timeout_ms;
            g_timers[i].callback = callback;
            g_timers[i].data = callback_data;
            return encode_handle(i, gen);
        }
    }
    return NULL;
}

static bool shim_app_timer_reschedule(pebble_app_timer_t *timer_handle, uint32_t new_timeout_ms) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_TIMER_RESCHEDULE, true);
    pebble_timer_slot_t *slot = decode_active_handle(timer_handle);
    if (slot == NULL) {
        return false;
    }
    slot->deadline_ms = pebble_rt_mono_ms() + (uint64_t)new_timeout_ms;
    return true;
}

static void shim_app_timer_cancel(pebble_app_timer_t *timer_handle) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_TIMER_CANCEL, true);
    pebble_timer_slot_t *slot = decode_active_handle(timer_handle);
    if (slot == NULL) {
        return;
    }
    slot->active = false;
    slot->callback = NULL;
    slot->data = NULL;
}

void pebble_timer_register_syscalls(void **table) {
    table[PEBBLE_SYS_APP_TIMER_REGISTER] = (void *)shim_app_timer_register;
    table[PEBBLE_SYS_APP_TIMER_RESCHEDULE] = (void *)shim_app_timer_reschedule;
    table[PEBBLE_SYS_APP_TIMER_CANCEL] = (void *)shim_app_timer_cancel;
}
