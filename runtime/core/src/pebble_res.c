#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_PBPACK_MAX_ENTRIES 256u
#define PEBBLE_PBPACK_HEADER_BYTES 12u
#define PEBBLE_PBPACK_ENTRY_BYTES 16u
#define PEBBLE_PBPACK_DATA_OFFSET_256 (PEBBLE_PBPACK_HEADER_BYTES + 256u * PEBBLE_PBPACK_ENTRY_BYTES)
#define PEBBLE_PBPACK_DATA_OFFSET_255 (PEBBLE_PBPACK_HEADER_BYTES + 255u * PEBBLE_PBPACK_ENTRY_BYTES)

typedef struct {
    uint32_t resource_id;
    uint32_t abs_offset;
    uint32_t length;
    uint32_t crc;
} pebble_res_entry_t;

static pebble_res_entry_t g_res_entries[PEBBLE_PBPACK_MAX_ENTRIES];
static uint32_t g_res_count;

static const pebble_res_entry_t *validate_handle(void *handle) {
    if (handle == NULL || g_res_count == 0u) {
        return NULL;
    }
    uintptr_t p = (uintptr_t)handle;
    uintptr_t lo = (uintptr_t)&g_res_entries[0];
    uintptr_t hi = (uintptr_t)&g_res_entries[g_res_count];
    if (p < lo || p >= hi) {
        return NULL;
    }
    if (((p - lo) % sizeof(pebble_res_entry_t)) != 0u) {
        return NULL;
    }
    return (const pebble_res_entry_t *)handle;
}

void pebble_res_reset(const pebble_hal_t *hal) {
    g_res_count = 0u;
    if (hal == NULL || hal->read_resource == NULL) {
        return;
    }

    uint8_t hdr[PEBBLE_PBPACK_HEADER_BYTES];
    if (hal->read_resource(hal->ctx, 0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return;
    }

    uint32_t num_files = pebble_read_u32_le(hdr);
    if (num_files == 0u) {
        return;
    }
    if (num_files > PEBBLE_PBPACK_MAX_ENTRIES) {
        num_files = PEBBLE_PBPACK_MAX_ENTRIES;
    }

    uint32_t loaded = 0u;
    uint32_t max_rel_end = 0u;
    for (uint32_t i = 0u; i < num_files; ++i) {
        uint8_t raw[PEBBLE_PBPACK_ENTRY_BYTES];
        uint32_t entry_off = PEBBLE_PBPACK_HEADER_BYTES + i * PEBBLE_PBPACK_ENTRY_BYTES;
        if (hal->read_resource(hal->ctx, entry_off, raw, sizeof(raw)) != sizeof(raw)) {
            break;
        }
        uint32_t res_id = pebble_read_u32_le(raw + 0u);
        uint32_t rel_off = pebble_read_u32_le(raw + 4u);
        uint32_t length = pebble_read_u32_le(raw + 8u);
        uint32_t crc = pebble_read_u32_le(raw + 12u);
        if (res_id == 0u ||
            rel_off > 0xFFFFFFFFu - PEBBLE_PBPACK_DATA_OFFSET_256 ||
            length > 0xFFFFFFFFu - (PEBBLE_PBPACK_DATA_OFFSET_256 + rel_off)) {
            continue;
        }
        if (rel_off + length > max_rel_end) {
            max_rel_end = rel_off + length;
        }
        g_res_entries[loaded].resource_id = res_id;
        g_res_entries[loaded].abs_offset = rel_off; /* shifted by data_base below */
        g_res_entries[loaded].length = length;
        g_res_entries[loaded].crc = crc;
        loaded++;
    }

    /* Standard SDK 2/3 app pbpacks use 256 table slots (4108); SDK 4 can use 255 slots (4092). */
    uint32_t data_base = PEBBLE_PBPACK_DATA_OFFSET_256;
    if (max_rel_end > 0u) {
        uint8_t probe = 0u;
        if (hal->read_resource(hal->ctx, PEBBLE_PBPACK_DATA_OFFSET_256 + max_rel_end - 1u, &probe, 1u) != 1u &&
            hal->read_resource(hal->ctx, PEBBLE_PBPACK_DATA_OFFSET_255 + max_rel_end - 1u, &probe, 1u) == 1u) {
            data_base = PEBBLE_PBPACK_DATA_OFFSET_255;
        }
    }
    for (uint32_t i = 0u; i < loaded; ++i) {
        g_res_entries[i].abs_offset += data_base;
    }
    g_res_count = loaded;
}

bool pebble_res_lookup_id(uint32_t resource_id, uint32_t *abs_offset_out, uint32_t *length_out) {
    if (resource_id == 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < g_res_count; ++i) {
        if (g_res_entries[i].resource_id == resource_id) {
            if (abs_offset_out != NULL) *abs_offset_out = g_res_entries[i].abs_offset;
            if (length_out != NULL) *length_out = g_res_entries[i].length;
            return true;
        }
    }
    return false;
}

bool pebble_res_lookup_handle(void *handle, uint32_t *abs_offset_out, uint32_t *length_out) {
    const pebble_res_entry_t *e = validate_handle(handle);
    if (e == NULL) {
        return false;
    }
    if (abs_offset_out != NULL) *abs_offset_out = e->abs_offset;
    if (length_out != NULL) *length_out = e->length;
    return true;
}

size_t pebble_res_read_abs(uint32_t abs_offset, void *dst, size_t len) {
    const pebble_hal_t *hal = pebble_rt_hal();
    if (hal == NULL || hal->read_resource == NULL || dst == NULL || len == 0u) {
        return 0u;
    }
    return hal->read_resource(hal->ctx, abs_offset, dst, len);
}

static size_t load_entry_range(
    const pebble_res_entry_t *entry,
    uint32_t start_offset,
    uint8_t *buffer,
    size_t num_bytes) {
    if (entry == NULL || buffer == NULL || num_bytes == 0u || start_offset >= entry->length) {
        return 0u;
    }
    uint32_t avail = entry->length - start_offset;
    size_t to_read = (num_bytes < (size_t)avail) ? num_bytes : (size_t)avail;
    return pebble_res_read_abs(entry->abs_offset + start_offset, buffer, to_read);
}

/* --- PebbleOS Resource Syscalls --- */

static void *shim_resource_get_handle(uint32_t resource_id) {
    pebble_rt_record_syscall(PEBBLE_SYS_RESOURCE_GET_HANDLE, true);
    if (resource_id == 0u) {
        return NULL;
    }
    for (uint32_t i = 0u; i < g_res_count; ++i) {
        if (g_res_entries[i].resource_id == resource_id) {
            return &g_res_entries[i];
        }
    }
    return NULL;
}

static size_t shim_resource_size(void *h) {
    pebble_rt_record_syscall(PEBBLE_SYS_RESOURCE_SIZE, true);
    const pebble_res_entry_t *e = validate_handle(h);
    return (e != NULL) ? (size_t)e->length : 0u;
}

static size_t shim_resource_load(void *h, uint8_t *buffer, size_t max_length) {
    pebble_rt_record_syscall(PEBBLE_SYS_RESOURCE_LOAD, true);
    const pebble_res_entry_t *e = validate_handle(h);
    return load_entry_range(e, 0u, buffer, max_length);
}

static size_t shim_resource_load_byte_range(
    void *h,
    uint32_t start_offset,
    uint8_t *buffer,
    size_t num_bytes) {
    pebble_rt_record_syscall(PEBBLE_SYS_RESOURCE_LOAD_BYTE_RANGE, true);
    const pebble_res_entry_t *e = validate_handle(h);
    return load_entry_range(e, start_offset, buffer, num_bytes);
}

void pebble_res_register_syscalls(void **table) {
    table[PEBBLE_SYS_RESOURCE_GET_HANDLE] = (void *)shim_resource_get_handle;
    table[PEBBLE_SYS_RESOURCE_SIZE] = (void *)shim_resource_size;
    table[PEBBLE_SYS_RESOURCE_LOAD] = (void *)shim_resource_load;
    table[PEBBLE_SYS_RESOURCE_LOAD_BYTE_RANGE] = (void *)shim_resource_load_byte_range;
}
