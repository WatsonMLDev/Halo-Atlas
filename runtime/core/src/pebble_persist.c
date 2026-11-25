#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_PERSIST_MAX_VALUE_BYTES 256u
#define PEBBLE_PERSIST_MAX_TOTAL_BYTES 4096u
#define PEBBLE_PERSIST_MAX_KEYS 64u
#define PEBBLE_PERSIST_HEADER_BYTES 8u
#define PEBBLE_PERSIST_ENTRY_HDR_BYTES 6u
#define PEBBLE_PERSIST_BLOB_MAX \
    (PEBBLE_PERSIST_HEADER_BYTES + PEBBLE_PERSIST_MAX_KEYS * PEBBLE_PERSIST_ENTRY_HDR_BYTES + \
     PEBBLE_PERSIST_MAX_TOTAL_BYTES)

typedef struct {
    uint32_t key;
    uint16_t offset;
    uint16_t size;
} pebble_persist_meta_t;

static pebble_persist_meta_t g_persist_meta[PEBBLE_PERSIST_MAX_KEYS];
static uint8_t g_persist_arena[PEBBLE_PERSIST_MAX_TOTAL_BYTES];
static uint32_t g_persist_count;
static uint32_t g_persist_used_bytes;
static uint8_t g_persist_io_buf[PEBBLE_PERSIST_BLOB_MAX];

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

static int32_t find_key(uint32_t key) {
    for (uint32_t i = 0u; i < g_persist_count; ++i) {
        if (g_persist_meta[i].key == key) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void remove_entry_at(uint32_t idx) {
    uint16_t off = g_persist_meta[idx].offset;
    uint16_t sz = g_persist_meta[idx].size;
    uint32_t tail_start = (uint32_t)off + (uint32_t)sz;
    if (tail_start < g_persist_used_bytes) {
        uint32_t move_len = g_persist_used_bytes - tail_start;
        for (uint32_t i = 0u; i < move_len; ++i) {
            g_persist_arena[(uint32_t)off + i] = g_persist_arena[tail_start + i];
        }
    }
    g_persist_used_bytes -= (uint32_t)sz;
    for (uint32_t i = idx; i + 1u < g_persist_count; ++i) {
        g_persist_meta[i] = g_persist_meta[i + 1u];
        g_persist_meta[i].offset = (uint16_t)(g_persist_meta[i].offset - sz);
    }
    g_persist_count--;
}

static bool persist_flush_candidate(
    uint32_t target_key,
    bool has_new_value,
    const uint8_t *new_data,
    uint16_t new_size) {
    const pebble_hal_t *hal = pebble_rt_hal();
    if (hal == NULL || hal->write_persist == NULL) {
        return true;
    }

    uint32_t new_count = 0u;
    for (uint32_t i = 0u; i < g_persist_count; ++i) {
        if (g_persist_meta[i].key != target_key) {
            new_count++;
        }
    }
    if (has_new_value) {
        new_count++;
    }

    g_persist_io_buf[0] = 'P';
    g_persist_io_buf[1] = 'K';
    g_persist_io_buf[2] = 'V';
    g_persist_io_buf[3] = '1';
    write_u32_le(g_persist_io_buf + 4u, new_count);
    size_t pos = PEBBLE_PERSIST_HEADER_BYTES;
    for (uint32_t i = 0u; i < g_persist_count; ++i) {
        const pebble_persist_meta_t *m = &g_persist_meta[i];
        if (m->key == target_key) {
            continue;
        }
        if (pos + PEBBLE_PERSIST_ENTRY_HDR_BYTES + m->size > sizeof(g_persist_io_buf)) {
            return false;
        }
        write_u32_le(g_persist_io_buf + pos, m->key);
        write_u16_le(g_persist_io_buf + pos + 4u, m->size);
        pos += PEBBLE_PERSIST_ENTRY_HDR_BYTES;
        if (m->size > 0u) {
            copy_bytes(g_persist_io_buf + pos, g_persist_arena + m->offset, m->size);
            pos += m->size;
        }
    }
    if (has_new_value) {
        if (pos + PEBBLE_PERSIST_ENTRY_HDR_BYTES + new_size > sizeof(g_persist_io_buf)) {
            return false;
        }
        write_u32_le(g_persist_io_buf + pos, target_key);
        write_u16_le(g_persist_io_buf + pos + 4u, new_size);
        pos += PEBBLE_PERSIST_ENTRY_HDR_BYTES;
        if (new_size > 0u) {
            copy_bytes(g_persist_io_buf + pos, new_data, new_size);
            pos += new_size;
        }
    }
    return hal->write_persist(hal->ctx, g_persist_io_buf, pos);
}

void pebble_persist_reset(const pebble_hal_t *hal) {
    g_persist_count = 0u;
    g_persist_used_bytes = 0u;
    if (hal == NULL || hal->read_persist == NULL) {
        return;
    }
    size_t n = hal->read_persist(hal->ctx, g_persist_io_buf, sizeof(g_persist_io_buf));
    if (n < PEBBLE_PERSIST_HEADER_BYTES) {
        return;
    }
    if (g_persist_io_buf[0] != 'P' || g_persist_io_buf[1] != 'K' ||
        g_persist_io_buf[2] != 'V' || g_persist_io_buf[3] != '1') {
        return;
    }
    uint32_t count = pebble_read_u32_le(g_persist_io_buf + 4u);
    if (count > PEBBLE_PERSIST_MAX_KEYS) {
        return;
    }
    size_t pos = PEBBLE_PERSIST_HEADER_BYTES;
    for (uint32_t i = 0u; i < count; ++i) {
        if (pos + PEBBLE_PERSIST_ENTRY_HDR_BYTES > n) {
            break;
        }
        uint32_t key = pebble_read_u32_le(g_persist_io_buf + pos);
        uint16_t sz = pebble_read_u16_le(g_persist_io_buf + pos + 4u);
        pos += PEBBLE_PERSIST_ENTRY_HDR_BYTES;
        if (sz > PEBBLE_PERSIST_MAX_VALUE_BYTES || pos + sz > n) {
            break;
        }
        if (g_persist_used_bytes + sz > PEBBLE_PERSIST_MAX_TOTAL_BYTES) {
            break;
        }
        if (find_key(key) >= 0) {
            pos += sz;
            continue;
        }
        g_persist_meta[g_persist_count].key = key;
        g_persist_meta[g_persist_count].offset = (uint16_t)g_persist_used_bytes;
        g_persist_meta[g_persist_count].size = sz;
        if (sz > 0u) {
            copy_bytes(g_persist_arena + g_persist_used_bytes, g_persist_io_buf + pos, sz);
            g_persist_used_bytes += sz;
        }
        g_persist_count++;
        pos += sz;
    }
}

static int32_t persist_store_raw(uint32_t key, const uint8_t *data, size_t size) {
    if (data == NULL && size > 0u) {
        return PEBBLE_E_INVALID_ARGUMENT;
    }
    if (size > PEBBLE_PERSIST_MAX_VALUE_BYTES) {
        return PEBBLE_E_RANGE;
    }

    int32_t existing = find_key(key);
    uint32_t existing_size = (existing >= 0) ? (uint32_t)g_persist_meta[(uint32_t)existing].size : 0u;
    if (existing < 0 && g_persist_count >= PEBBLE_PERSIST_MAX_KEYS) {
        return PEBBLE_E_OUT_OF_STORAGE;
    }
    if ((g_persist_used_bytes - existing_size) + (uint32_t)size > PEBBLE_PERSIST_MAX_TOTAL_BYTES) {
        return PEBBLE_E_OUT_OF_STORAGE;
    }

    /* Persist to HAL atomically before committing in-memory state. */
    if (!persist_flush_candidate(key, true, data, (uint16_t)size)) {
        return PEBBLE_E_OUT_OF_STORAGE;
    }
    if (existing >= 0) {
        remove_entry_at((uint32_t)existing);
    }
    g_persist_meta[g_persist_count].key = key;
    g_persist_meta[g_persist_count].offset = (uint16_t)g_persist_used_bytes;
    g_persist_meta[g_persist_count].size = (uint16_t)size;
    if (size > 0u) {
        copy_bytes(g_persist_arena + g_persist_used_bytes, data, size);
        g_persist_used_bytes += (uint32_t)size;
    }
    g_persist_count++;
    return (int32_t)size;
}

/* --- PebbleOS Persist Syscalls --- */

static bool shim_persist_exists(uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_EXISTS, true);
    return find_key(key) >= 0;
}

static size_t shim_persist_get_max_size(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_GET_MAX_SIZE, true);
    return PEBBLE_PERSIST_MAX_TOTAL_BYTES;
}

static int32_t shim_persist_get_size(uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_GET_SIZE, true);
    int32_t idx = find_key(key);
    if (idx < 0) {
        return PEBBLE_E_DOES_NOT_EXIST;
    }
    return (int32_t)g_persist_meta[(uint32_t)idx].size;
}

static bool shim_persist_read_bool(uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_READ_BOOL, true);
    int32_t idx = find_key(key);
    if (idx < 0 || g_persist_meta[(uint32_t)idx].size == 0u) {
        return false;
    }
    return g_persist_arena[g_persist_meta[(uint32_t)idx].offset] != 0u;
}

static int32_t shim_persist_read_int(uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_READ_INT, true);
    int32_t idx = find_key(key);
    if (idx < 0 || g_persist_meta[(uint32_t)idx].size < 4u) {
        return 0;
    }
    return (int32_t)pebble_read_u32_le(g_persist_arena + g_persist_meta[(uint32_t)idx].offset);
}

static int32_t shim_persist_read_data(uint32_t key, void *buffer, size_t buffer_size) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_READ_DATA, true);
    int32_t idx = find_key(key);
    if (idx < 0) {
        return PEBBLE_E_DOES_NOT_EXIST;
    }
    if (buffer == NULL && buffer_size > 0u) {
        return PEBBLE_E_INVALID_ARGUMENT;
    }
    uint16_t stored = g_persist_meta[(uint32_t)idx].size;
    size_t to_copy = ((size_t)stored < buffer_size) ? (size_t)stored : buffer_size;
    if (to_copy > 0u) {
        copy_bytes((uint8_t *)buffer, g_persist_arena + g_persist_meta[(uint32_t)idx].offset, to_copy);
    }
    return (int32_t)to_copy;
}

static int32_t shim_persist_read_string(uint32_t key, char *buffer, size_t buffer_size) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_READ_STRING, true);
    int32_t idx = find_key(key);
    if (idx < 0) {
        return PEBBLE_E_DOES_NOT_EXIST;
    }
    if (buffer == NULL || buffer_size == 0u) {
        return PEBBLE_E_INVALID_ARGUMENT;
    }
    uint16_t stored = g_persist_meta[(uint32_t)idx].size;
    const uint8_t *src = g_persist_arena + g_persist_meta[(uint32_t)idx].offset;
    size_t str_len = 0u;
    while (str_len < (size_t)stored && src[str_len] != 0u) {
        str_len++;
    }
    size_t max_chars = buffer_size - 1u;
    size_t copy_chars = (str_len < max_chars) ? str_len : max_chars;
    for (size_t i = 0u; i < copy_chars; ++i) {
        buffer[i] = (char)src[i];
    }
    buffer[copy_chars] = '\0';
    return (int32_t)(copy_chars + 1u);
}

static int32_t shim_persist_write_bool(uint32_t key, bool value) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_WRITE_BOOL, true);
    uint8_t b = value ? 1u : 0u;
    return persist_store_raw(key, &b, 1u);
}

static int32_t shim_persist_write_int(uint32_t key, int32_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_WRITE_INT, true);
    uint8_t raw[4];
    write_u32_le(raw, (uint32_t)value);
    return persist_store_raw(key, raw, sizeof(raw));
}

static int32_t shim_persist_write_data(uint32_t key, const void *data, size_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_WRITE_DATA, true);
    return persist_store_raw(key, (const uint8_t *)data, size);
}

static int32_t shim_persist_write_string(uint32_t key, const char *cstring) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_WRITE_STRING, true);
    if (cstring == NULL) {
        return PEBBLE_E_INVALID_ARGUMENT;
    }
    size_t len = 0u;
    while (len < PEBBLE_PERSIST_MAX_VALUE_BYTES - 1u && cstring[len] != '\0') {
        len++;
    }
    uint8_t tmp[PEBBLE_PERSIST_MAX_VALUE_BYTES];
    for (size_t i = 0u; i < len; ++i) {
        tmp[i] = (uint8_t)cstring[i];
    }
    tmp[len] = 0u;
    return persist_store_raw(key, tmp, len + 1u);
}

static int32_t shim_persist_delete(uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_PERSIST_DELETE, true);
    int32_t idx = find_key(key);
    if (idx < 0) {
        return PEBBLE_E_DOES_NOT_EXIST;
    }
    if (!persist_flush_candidate(key, false, NULL, 0u)) {
        return PEBBLE_E_OUT_OF_STORAGE;
    }
    remove_entry_at((uint32_t)idx);
    return PEBBLE_S_TRUE;
}

void pebble_persist_register_syscalls(void **table) {
    table[PEBBLE_SYS_PERSIST_EXISTS] = (void *)shim_persist_exists;
    table[PEBBLE_SYS_PERSIST_GET_MAX_SIZE] = (void *)shim_persist_get_max_size;
    table[PEBBLE_SYS_PERSIST_GET_SIZE] = (void *)shim_persist_get_size;
    table[PEBBLE_SYS_PERSIST_READ_BOOL] = (void *)shim_persist_read_bool;
    table[PEBBLE_SYS_PERSIST_READ_INT] = (void *)shim_persist_read_int;
    table[PEBBLE_SYS_PERSIST_READ_DATA] = (void *)shim_persist_read_data;
    table[PEBBLE_SYS_PERSIST_READ_DATA__DEPRECATED] = (void *)shim_persist_read_data;
    table[PEBBLE_SYS_PERSIST_READ_STRING] = (void *)shim_persist_read_string;
    table[PEBBLE_SYS_PERSIST_READ_STRING__DEPRECATED] = (void *)shim_persist_read_string;
    table[PEBBLE_SYS_PERSIST_WRITE_BOOL] = (void *)shim_persist_write_bool;
    table[PEBBLE_SYS_PERSIST_WRITE_INT] = (void *)shim_persist_write_int;
    table[PEBBLE_SYS_PERSIST_WRITE_DATA] = (void *)shim_persist_write_data;
    table[PEBBLE_SYS_PERSIST_WRITE_DATA__DEPRECATED] = (void *)shim_persist_write_data;
    table[PEBBLE_SYS_PERSIST_WRITE_STRING] = (void *)shim_persist_write_string;
    table[PEBBLE_SYS_PERSIST_DELETE] = (void *)shim_persist_delete;
}
