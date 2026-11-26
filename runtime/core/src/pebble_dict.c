#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

static void dict_memcpy(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0u; i < len; ++i) {
        dst[i] = src[i];
    }
}

static bool dict_memeq(const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0u; i < len; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static size_t dict_strlen(const char *s) {
    size_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void dict_write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void dict_write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t tuple_get_length(const pebble_tuple_t *t) {
    const uint8_t *p = (const uint8_t *)t;
    return pebble_read_u16_le(p + 5u);
}

static uint32_t tuple_get_key(const pebble_tuple_t *t) {
    const uint8_t *p = (const uint8_t *)t;
    return pebble_read_u32_le(p);
}

static uint8_t tuple_get_type(const pebble_tuple_t *t) {
    const uint8_t *p = (const uint8_t *)t;
    return p[4];
}

static const uint8_t *tuple_get_data(const pebble_tuple_t *t) {
    return ((const uint8_t *)t) + PEBBLE_TUPLE_HEADER_BYTES;
}

bool pebble_dict_validate_buffer(const uint8_t *buf, size_t len, uint32_t *actual_size_out) {
    if (buf == NULL || len < 1u) {
        return false;
    }
    uint8_t count = buf[0];
    size_t off = 1u;
    for (uint8_t i = 0u; i < count; ++i) {
        if (off + PEBBLE_TUPLE_HEADER_BYTES > len) {
            return false;
        }
        uint16_t tlen = pebble_read_u16_le(buf + off + 5u);
        if ((size_t)tlen > len - (off + PEBBLE_TUPLE_HEADER_BYTES)) {
            return false;
        }
        off += PEBBLE_TUPLE_HEADER_BYTES + (size_t)tlen;
    }
    if (actual_size_out != NULL) {
        *actual_size_out = (uint32_t)off;
    }
    return true;
}

static uint32_t dict_write_tuple_raw(
    pebble_dict_iterator_t *iter,
    uint32_t key,
    uint8_t type,
    const uint8_t *data,
    uint16_t length) {
    if (iter == NULL || iter->dictionary == NULL || iter->cursor == NULL || iter->end == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    if (length > 0u && data == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint8_t *base = (uint8_t *)iter->dictionary;
    uint8_t *cur = (uint8_t *)iter->cursor;
    const uint8_t *end = (const uint8_t *)iter->end;
    if (cur < base + 1u || cur > end) {
        return PEBBLE_DICT_INTERNAL_INCONSISTENCY;
    }
    if (base[0] == 255u) {
        return PEBBLE_DICT_NOT_ENOUGH_STORAGE;
    }
    size_t avail = (size_t)(end - cur);
    if (avail < PEBBLE_TUPLE_HEADER_BYTES || (size_t)length > avail - PEBBLE_TUPLE_HEADER_BYTES) {
        return PEBBLE_DICT_NOT_ENOUGH_STORAGE;
    }

    dict_write_u32_le(cur, key);
    cur[4] = type;
    dict_write_u16_le(cur + 5u, length);
    if (length > 0u) {
        dict_memcpy(cur + PEBBLE_TUPLE_HEADER_BYTES, data, length);
    }

    base[0] = (uint8_t)(base[0] + 1u);
    iter->cursor = (pebble_tuple_t *)(cur + PEBBLE_TUPLE_HEADER_BYTES + length);
    return PEBBLE_DICT_OK;
}

uint32_t pebble_dict_write_begin_internal(
    pebble_dict_iterator_t *iter,
    uint8_t *buffer,
    uint16_t size) {
    if (iter == NULL || buffer == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    if (size < 1u) {
        return PEBBLE_DICT_NOT_ENOUGH_STORAGE;
    }
    buffer[0] = 0u;
    iter->dictionary = (pebble_dict_t *)buffer;
    iter->end = buffer + size;
    iter->cursor = (pebble_tuple_t *)(buffer + 1u);
    return PEBBLE_DICT_OK;
}

uint32_t pebble_dict_write_end_internal(pebble_dict_iterator_t *iter) {
    if (iter == NULL || iter->dictionary == NULL || iter->cursor == NULL) {
        return 0u;
    }
    const uint8_t *base = (const uint8_t *)iter->dictionary;
    const uint8_t *cur = (const uint8_t *)iter->cursor;
    if (cur < base + 1u) {
        return 0u;
    }
    uint32_t written = (uint32_t)(cur - base);
    iter->end = base + written;
    iter->cursor = NULL;
    return written;
}

uint32_t pebble_dict_write_tuplet_internal(
    pebble_dict_iterator_t *iter,
    const pebble_tuplet_t *tuplet) {
    if (iter == NULL || tuplet == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    switch (tuplet->type) {
        case PEBBLE_TUPLE_BYTE_ARRAY:
            return dict_write_tuple_raw(
                iter,
                tuplet->key,
                PEBBLE_TUPLE_BYTE_ARRAY,
                tuplet->bytes.data,
                tuplet->bytes.length);
        case PEBBLE_TUPLE_CSTRING: {
            if (tuplet->cstring.data == NULL) {
                return PEBBLE_DICT_INVALID_ARGS;
            }
            uint16_t len = tuplet->cstring.length;
            if (len == 0u) {
                size_t slen = dict_strlen(tuplet->cstring.data) + 1u;
                if (slen > 0xffffu) {
                    return PEBBLE_DICT_NOT_ENOUGH_STORAGE;
                }
                len = (uint16_t)slen;
            }
            return dict_write_tuple_raw(
                iter,
                tuplet->key,
                PEBBLE_TUPLE_CSTRING,
                (const uint8_t *)tuplet->cstring.data,
                len);
        }
        case PEBBLE_TUPLE_UINT:
        case PEBBLE_TUPLE_INT: {
            uint16_t w = tuplet->integer.width;
            if (w != 1u && w != 2u && w != 4u) {
                return PEBBLE_DICT_INVALID_ARGS;
            }
            uint8_t tmp[4];
            dict_write_u32_le(tmp, tuplet->integer.storage);
            return dict_write_tuple_raw(iter, tuplet->key, tuplet->type, tmp, w);
        }
        default:
            return PEBBLE_DICT_INVALID_ARGS;
    }
}

pebble_tuple_t *pebble_dict_read_first_internal(pebble_dict_iterator_t *iter) {
    if (iter == NULL || iter->dictionary == NULL) {
        return NULL;
    }
    uint8_t *base = (uint8_t *)iter->dictionary;
    const uint8_t *end = (const uint8_t *)iter->end;
    if (end != NULL && end <= base) {
        iter->cursor = NULL;
        return NULL;
    }
    if (base[0] == 0u) {
        iter->cursor = NULL;
        return NULL;
    }
    uint8_t *first = base + 1u;
    if (end != NULL) {
        if (first + PEBBLE_TUPLE_HEADER_BYTES > end) {
            iter->cursor = NULL;
            return NULL;
        }
        uint16_t tlen = pebble_read_u16_le(first + 5u);
        if ((size_t)tlen > (size_t)(end - (first + PEBBLE_TUPLE_HEADER_BYTES))) {
            iter->cursor = NULL;
            return NULL;
        }
    }
    iter->cursor = (pebble_tuple_t *)first;
    return iter->cursor;
}

pebble_tuple_t *pebble_dict_read_next_internal(pebble_dict_iterator_t *iter) {
    if (iter == NULL || iter->dictionary == NULL || iter->cursor == NULL) {
        return NULL;
    }
    uint8_t *base = (uint8_t *)iter->dictionary;
    const uint8_t *end = (const uint8_t *)iter->end;
    uint8_t count = base[0];
    uint8_t *p = base + 1u;
    uint8_t *cur = (uint8_t *)iter->cursor;

    for (uint8_t i = 0u; i < count; ++i) {
        if (end != NULL && p + PEBBLE_TUPLE_HEADER_BYTES > end) {
            iter->cursor = NULL;
            return NULL;
        }
        uint16_t tlen = pebble_read_u16_le(p + 5u);
        if (end != NULL && (size_t)tlen > (size_t)(end - (p + PEBBLE_TUPLE_HEADER_BYTES))) {
            iter->cursor = NULL;
            return NULL;
        }
        uint8_t *next = p + PEBBLE_TUPLE_HEADER_BYTES + tlen;
        if (p == cur) {
            if (i + 1u >= count) {
                iter->cursor = NULL;
                return NULL;
            }
            if (end != NULL) {
                if (next + PEBBLE_TUPLE_HEADER_BYTES > end) {
                    iter->cursor = NULL;
                    return NULL;
                }
                uint16_t nlen = pebble_read_u16_le(next + 5u);
                if ((size_t)nlen > (size_t)(end - (next + PEBBLE_TUPLE_HEADER_BYTES))) {
                    iter->cursor = NULL;
                    return NULL;
                }
            }
            iter->cursor = (pebble_tuple_t *)next;
            return iter->cursor;
        }
        p = next;
    }

    iter->cursor = NULL;
    return NULL;
}

pebble_tuple_t *pebble_dict_read_begin_internal(
    pebble_dict_iterator_t *iter,
    const uint8_t *buffer,
    uint16_t size) {
    if (iter == NULL || buffer == NULL || size < 1u) {
        if (iter != NULL) {
            iter->dictionary = NULL;
            iter->end = NULL;
            iter->cursor = NULL;
        }
        return NULL;
    }
    iter->dictionary = (pebble_dict_t *)(uintptr_t)buffer;
    iter->end = buffer + size;
    iter->cursor = NULL;
    return pebble_dict_read_first_internal(iter);
}

pebble_tuple_t *pebble_dict_find_internal(const pebble_dict_iterator_t *iter, uint32_t key) {
    if (iter == NULL || iter->dictionary == NULL) {
        return NULL;
    }
    pebble_dict_iterator_t scan = {
        .dictionary = iter->dictionary,
        .end = iter->end,
        .cursor = NULL,
    };
    pebble_tuple_t *t = pebble_dict_read_first_internal(&scan);
    while (t != NULL) {
        if (tuple_get_key(t) == key) {
            return t;
        }
        t = pebble_dict_read_next_internal(&scan);
    }
    return NULL;
}

static uint32_t dict_size_internal(const pebble_dict_iterator_t *iter) {
    if (iter == NULL || iter->dictionary == NULL) {
        return 0u;
    }
    const uint8_t *base = (const uint8_t *)iter->dictionary;
    const uint8_t *end = (const uint8_t *)iter->end;
    if (end != NULL && end <= base) {
        return 0u;
    }
    uint8_t count = base[0];
    const uint8_t *p = base + 1u;
    for (uint8_t i = 0u; i < count; ++i) {
        if (end != NULL && p + PEBBLE_TUPLE_HEADER_BYTES > end) {
            return 0u;
        }
        uint16_t tlen = pebble_read_u16_le(p + 5u);
        if (end != NULL && (size_t)tlen > (size_t)(end - (p + PEBBLE_TUPLE_HEADER_BYTES))) {
            return 0u;
        }
        p += PEBBLE_TUPLE_HEADER_BYTES + tlen;
    }
    return (uint32_t)(p - base);
}

static uint32_t dict_calc_buffer_size_from_tuplets_internal(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    if (tuplets == NULL) {
        return 0u;
    }
    uint32_t total = 1u;
    for (uint8_t i = 0u; i < tuplets_count; ++i) {
        const pebble_tuplet_t *t = &tuplets[i];
        uint32_t payload = 0u;
        switch (t->type) {
            case PEBBLE_TUPLE_BYTE_ARRAY:
                payload = t->bytes.length;
                break;
            case PEBBLE_TUPLE_CSTRING:
                payload = (t->cstring.length != 0u)
                              ? (uint32_t)t->cstring.length
                              : ((t->cstring.data != NULL) ? (uint32_t)(dict_strlen(t->cstring.data) + 1u) : 0u);
                break;
            case PEBBLE_TUPLE_UINT:
            case PEBBLE_TUPLE_INT:
                payload = t->integer.width;
                break;
            default:
                return 0u;
        }
        total += PEBBLE_TUPLE_HEADER_BYTES + payload;
    }
    return total;
}

static uint32_t dict_serialize_tuplets_to_buffer_with_iter_internal(
    pebble_dict_iterator_t *iter,
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count,
    uint8_t *buffer,
    uint32_t *size_in_out) {
    if (iter == NULL || tuplets == NULL || buffer == NULL || size_in_out == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint16_t cap = (*size_in_out > 0xffffu) ? 0xffffu : (uint16_t)(*size_in_out);
    uint32_t res = pebble_dict_write_begin_internal(iter, buffer, cap);
    if (res != PEBBLE_DICT_OK) {
        return res;
    }
    for (uint8_t i = 0u; i < tuplets_count; ++i) {
        res = pebble_dict_write_tuplet_internal(iter, &tuplets[i]);
        if (res != PEBBLE_DICT_OK) {
            return res;
        }
    }
    *size_in_out = pebble_dict_write_end_internal(iter);
    return PEBBLE_DICT_OK;
}

static bool tuples_equal(const pebble_tuple_t *a, const pebble_tuple_t *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    if (tuple_get_type(a) != tuple_get_type(b)) {
        return false;
    }
    uint16_t len = tuple_get_length(a);
    if (len != tuple_get_length(b)) {
        return false;
    }
    return dict_memeq(tuple_get_data(a), tuple_get_data(b), len);
}

uint32_t pebble_dict_merge_internal(
    pebble_dict_iterator_t *dest,
    uint32_t *dest_max_size_in_out,
    pebble_dict_iterator_t *source,
    bool update_existing_keys_only,
    pebble_dict_key_updated_cb_t key_callback,
    void *context) {
    if (dest == NULL || dest->dictionary == NULL || dest_max_size_in_out == NULL ||
        source == NULL || source->dictionary == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint32_t max_size = *dest_max_size_in_out;
    if (max_size < 1u || max_size > 0xffffu) {
        return PEBBLE_DICT_INVALID_ARGS;
    }

    uint32_t old_size = dict_size_internal(dest);
    if (old_size < 1u || old_size > max_size) {
        return PEBBLE_DICT_INTERNAL_INCONSISTENCY;
    }

    uint8_t *old_buf = (uint8_t *)pebble_rt_alloc_data(old_size);
    uint8_t *merged_buf = (uint8_t *)pebble_rt_alloc_data(max_size);
    if (old_buf == NULL || merged_buf == NULL) {
        pebble_rt_free_data(old_buf, old_size);
        pebble_rt_free_data(merged_buf, max_size);
        return PEBBLE_DICT_MALLOC_FAILED;
    }

    dict_memcpy(old_buf, (const uint8_t *)dest->dictionary, old_size);

    pebble_dict_iterator_t old_iter;
    pebble_dict_read_begin_internal(&old_iter, old_buf, (uint16_t)old_size);

    pebble_dict_iterator_t merged_iter;
    uint32_t res = pebble_dict_write_begin_internal(&merged_iter, merged_buf, (uint16_t)max_size);

    /* Keep existing keys (updated from source when present). */
    pebble_tuple_t *old_t = pebble_dict_read_first_internal(&old_iter);
    while (res == PEBBLE_DICT_OK && old_t != NULL) {
        uint32_t key = tuple_get_key(old_t);
        pebble_tuple_t *src_t = pebble_dict_find_internal(source, key);
        const pebble_tuple_t *chosen = (src_t != NULL) ? src_t : old_t;
        res = dict_write_tuple_raw(
            &merged_iter,
            key,
            tuple_get_type(chosen),
            tuple_get_data(chosen),
            tuple_get_length(chosen));
        old_t = pebble_dict_read_next_internal(&old_iter);
    }

    /* Append new keys from source when allowed. */
    if (res == PEBBLE_DICT_OK && !update_existing_keys_only) {
        pebble_dict_iterator_t src_scan = {
            .dictionary = source->dictionary,
            .end = source->end,
            .cursor = NULL,
        };
        pebble_tuple_t *src_t = pebble_dict_read_first_internal(&src_scan);
        while (res == PEBBLE_DICT_OK && src_t != NULL) {
            uint32_t key = tuple_get_key(src_t);
            if (pebble_dict_find_internal(&old_iter, key) == NULL) {
                res = dict_write_tuple_raw(
                    &merged_iter,
                    key,
                    tuple_get_type(src_t),
                    tuple_get_data(src_t),
                    tuple_get_length(src_t));
            }
            src_t = pebble_dict_read_next_internal(&src_scan);
        }
    }

    if (res != PEBBLE_DICT_OK) {
        pebble_rt_free_data(old_buf, old_size);
        pebble_rt_free_data(merged_buf, max_size);
        return res;
    }

    uint32_t merged_size = pebble_dict_write_end_internal(&merged_iter);
    dict_memcpy((uint8_t *)dest->dictionary, merged_buf, merged_size);
    dest->end = (const uint8_t *)dest->dictionary + merged_size;
    dest->cursor = NULL;
    *dest_max_size_in_out = merged_size;

    if (key_callback != NULL) {
        pebble_dict_iterator_t src_scan = {
            .dictionary = source->dictionary,
            .end = source->end,
            .cursor = NULL,
        };
        pebble_tuple_t *src_t = pebble_dict_read_first_internal(&src_scan);
        while (src_t != NULL) {
            uint32_t key = tuple_get_key(src_t);
            const pebble_tuple_t *old_found = pebble_dict_find_internal(&old_iter, key);
            if (old_found != NULL || !update_existing_keys_only) {
                const pebble_tuple_t *new_found = pebble_dict_find_internal(dest, key);
                if (new_found != NULL && (old_found == NULL || !tuples_equal(old_found, new_found))) {
                    key_callback(key, new_found, old_found, context);
                }
            }
            src_t = pebble_dict_read_next_internal(&src_scan);
        }
    }

    pebble_rt_free_data(old_buf, old_size);
    pebble_rt_free_data(merged_buf, max_size);
    return PEBBLE_DICT_OK;
}

/* --- PebbleOS Dictionary Syscall Shims (24 symbols) --- */

static uint32_t shim_dict_calc_buffer_size(uint32_t tuple_count_arg, ...) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_CALC_BUFFER_SIZE, true);
    uint8_t tuple_count = (uint8_t)tuple_count_arg;
    uint32_t total = 1u;
    va_list ap;
    va_start(ap, tuple_count_arg);
    for (uint8_t i = 0u; i < tuple_count; ++i) {
        size_t sz = va_arg(ap, size_t);
        total += PEBBLE_TUPLE_HEADER_BYTES + (uint32_t)sz;
    }
    va_end(ap);
    return total;
}

static uint32_t shim_dict_calc_buffer_size_from_tuplets(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS, true);
    return dict_calc_buffer_size_from_tuplets_internal(tuplets, tuplets_count);
}

static uint32_t shim_dict_size(pebble_dict_iterator_t *iter) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_SIZE, true);
    return dict_size_internal(iter);
}

static uint32_t shim_dict_write_begin(
    pebble_dict_iterator_t *iter,
    uint8_t *buffer,
    uint16_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_BEGIN, true);
    return pebble_dict_write_begin_internal(iter, buffer, size);
}

static uint32_t shim_dict_write_end(pebble_dict_iterator_t *iter) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_END, true);
    return pebble_dict_write_end_internal(iter);
}

static uint32_t shim_dict_write_data(
    pebble_dict_iterator_t *iter,
    uint32_t key,
    const uint8_t *data,
    uint16_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_DATA, true);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_BYTE_ARRAY, data, size);
}

static uint32_t shim_dict_write_cstring(
    pebble_dict_iterator_t *iter,
    uint32_t key,
    const char *cstring) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_CSTRING, true);
    if (cstring == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    size_t len = dict_strlen(cstring) + 1u;
    if (len > 0xffffu) {
        return PEBBLE_DICT_NOT_ENOUGH_STORAGE;
    }
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_CSTRING, (const uint8_t *)cstring, (uint16_t)len);
}

static uint32_t shim_dict_write_int(
    pebble_dict_iterator_t *iter,
    uint32_t key,
    const void *integer,
    uint8_t width_bytes,
    bool is_signed) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_INT, true);
    if (integer == NULL || (width_bytes != 1u && width_bytes != 2u && width_bytes != 4u)) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint8_t type = is_signed ? PEBBLE_TUPLE_INT : PEBBLE_TUPLE_UINT;
    return dict_write_tuple_raw(iter, key, type, (const uint8_t *)integer, width_bytes);
}

static uint32_t shim_dict_write_uint8(pebble_dict_iterator_t *iter, uint32_t key, uint8_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_UINT8, true);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_UINT, &value, 1u);
}

static uint32_t shim_dict_write_uint16(pebble_dict_iterator_t *iter, uint32_t key, uint16_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_UINT16, true);
    uint8_t tmp[2];
    dict_write_u16_le(tmp, value);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_UINT, tmp, 2u);
}

static uint32_t shim_dict_write_uint32(pebble_dict_iterator_t *iter, uint32_t key, uint32_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_UINT32, true);
    uint8_t tmp[4];
    dict_write_u32_le(tmp, value);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_UINT, tmp, 4u);
}

static uint32_t shim_dict_write_int8(pebble_dict_iterator_t *iter, uint32_t key, int8_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_INT8, true);
    uint8_t b = (uint8_t)value;
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_INT, &b, 1u);
}

static uint32_t shim_dict_write_int16(pebble_dict_iterator_t *iter, uint32_t key, int16_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_INT16, true);
    uint8_t tmp[2];
    dict_write_u16_le(tmp, (uint16_t)value);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_INT, tmp, 2u);
}

static uint32_t shim_dict_write_int32(pebble_dict_iterator_t *iter, uint32_t key, int32_t value) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_INT32, true);
    uint8_t tmp[4];
    dict_write_u32_le(tmp, (uint32_t)value);
    return dict_write_tuple_raw(iter, key, PEBBLE_TUPLE_INT, tmp, 4u);
}

static uint32_t shim_dict_write_tuplet(pebble_dict_iterator_t *iter, const pebble_tuplet_t *tuplet) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_WRITE_TUPLET, true);
    return pebble_dict_write_tuplet_internal(iter, tuplet);
}

static pebble_tuple_t *shim_dict_read_begin_from_buffer(
    pebble_dict_iterator_t *iter,
    const uint8_t *buffer,
    uint16_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_READ_BEGIN_FROM_BUFFER, true);
    return pebble_dict_read_begin_internal(iter, buffer, size);
}

static pebble_tuple_t *shim_dict_read_first(pebble_dict_iterator_t *iter) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_READ_FIRST, true);
    return pebble_dict_read_first_internal(iter);
}

static pebble_tuple_t *shim_dict_read_next(pebble_dict_iterator_t *iter) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_READ_NEXT, true);
    return pebble_dict_read_next_internal(iter);
}

static pebble_tuple_t *shim_dict_find(const pebble_dict_iterator_t *iter, uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_FIND, true);
    return pebble_dict_find_internal(iter, key);
}

static uint32_t shim_dict_merge(
    pebble_dict_iterator_t *dest,
    uint32_t *dest_max_size_in_out,
    pebble_dict_iterator_t *source,
    bool update_existing_keys_only,
    pebble_dict_key_updated_cb_t key_callback,
    void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_MERGE, true);
    return pebble_dict_merge_internal(
        dest, dest_max_size_in_out, source, update_existing_keys_only, key_callback, context);
}

static uint32_t shim_dict_serialize_tuplets(
    pebble_dict_serialize_cb_t callback,
    void *context,
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_SERIALIZE_TUPLETS, true);
    if (callback == NULL || tuplets == NULL) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint32_t needed = dict_calc_buffer_size_from_tuplets_internal(tuplets, tuplets_count);
    if (needed == 0u || needed > 0xffffu) {
        return PEBBLE_DICT_INVALID_ARGS;
    }
    uint8_t *buf = (uint8_t *)pebble_rt_alloc_data(needed);
    if (buf == NULL) {
        return PEBBLE_DICT_MALLOC_FAILED;
    }
    pebble_dict_iterator_t iter;
    uint32_t sz = needed;
    uint32_t res = dict_serialize_tuplets_to_buffer_with_iter_internal(
        &iter, tuplets, tuplets_count, buf, &sz);
    if (res == PEBBLE_DICT_OK) {
        callback(buf, (uint16_t)sz, context);
    }
    pebble_rt_free_data(buf, needed);
    return res;
}

static uint32_t shim_dict_serialize_tuplets_to_buffer(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count,
    uint8_t *buffer,
    uint32_t *size_in_out) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER, true);
    pebble_dict_iterator_t iter;
    return dict_serialize_tuplets_to_buffer_with_iter_internal(
        &iter, tuplets, tuplets_count, buffer, size_in_out);
}

static uint32_t shim_dict_serialize_tuplets_to_buffer_deprecated(
    uint8_t tuplets_count,
    const pebble_tuplet_t *tuplets,
    uint8_t *buffer,
    uint32_t *size_in_out) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER__DEPRECATED, true);
    pebble_dict_iterator_t iter;
    return dict_serialize_tuplets_to_buffer_with_iter_internal(
        &iter, tuplets, tuplets_count, buffer, size_in_out);
}

static uint32_t shim_dict_serialize_tuplets_to_buffer_with_iter(
    pebble_dict_iterator_t *iter,
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count,
    uint8_t *buffer,
    uint32_t *size_in_out) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER_WITH_ITER, true);
    return dict_serialize_tuplets_to_buffer_with_iter_internal(
        iter, tuplets, tuplets_count, buffer, size_in_out);
}

void pebble_dict_register_syscalls(void **table) {
    table[PEBBLE_SYS_DICT_CALC_BUFFER_SIZE] = (void *)shim_dict_calc_buffer_size;
    table[PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS] =
        (void *)shim_dict_calc_buffer_size_from_tuplets;
    table[PEBBLE_SYS_DICT_SIZE] = (void *)shim_dict_size;
    table[PEBBLE_SYS_DICT_WRITE_BEGIN] = (void *)shim_dict_write_begin;
    table[PEBBLE_SYS_DICT_WRITE_END] = (void *)shim_dict_write_end;
    table[PEBBLE_SYS_DICT_WRITE_DATA] = (void *)shim_dict_write_data;
    table[PEBBLE_SYS_DICT_WRITE_CSTRING] = (void *)shim_dict_write_cstring;
    table[PEBBLE_SYS_DICT_WRITE_INT] = (void *)shim_dict_write_int;
    table[PEBBLE_SYS_DICT_WRITE_UINT8] = (void *)shim_dict_write_uint8;
    table[PEBBLE_SYS_DICT_WRITE_UINT16] = (void *)shim_dict_write_uint16;
    table[PEBBLE_SYS_DICT_WRITE_UINT32] = (void *)shim_dict_write_uint32;
    table[PEBBLE_SYS_DICT_WRITE_INT8] = (void *)shim_dict_write_int8;
    table[PEBBLE_SYS_DICT_WRITE_INT16] = (void *)shim_dict_write_int16;
    table[PEBBLE_SYS_DICT_WRITE_INT32] = (void *)shim_dict_write_int32;
    table[PEBBLE_SYS_DICT_WRITE_TUPLET] = (void *)shim_dict_write_tuplet;
    table[PEBBLE_SYS_DICT_READ_BEGIN_FROM_BUFFER] = (void *)shim_dict_read_begin_from_buffer;
    table[PEBBLE_SYS_DICT_READ_FIRST] = (void *)shim_dict_read_first;
    table[PEBBLE_SYS_DICT_READ_NEXT] = (void *)shim_dict_read_next;
    table[PEBBLE_SYS_DICT_FIND] = (void *)shim_dict_find;
    table[PEBBLE_SYS_DICT_MERGE] = (void *)shim_dict_merge;
    table[PEBBLE_SYS_DICT_SERIALIZE_TUPLETS] = (void *)shim_dict_serialize_tuplets;
    table[PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER] =
        (void *)shim_dict_serialize_tuplets_to_buffer;
    table[PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER__DEPRECATED] =
        (void *)shim_dict_serialize_tuplets_to_buffer_deprecated;
    table[PEBBLE_SYS_DICT_SERIALIZE_TUPLETS_TO_BUFFER_WITH_ITER] =
        (void *)shim_dict_serialize_tuplets_to_buffer_with_iter;
}
