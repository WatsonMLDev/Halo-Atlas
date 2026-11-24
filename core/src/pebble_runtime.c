#include "pebble_compat.h"
#include "pebble_syscall_table_gen.h"

/* Internal loader execution helper defined in pebble_loader.c. */
pebble_status_t pebble_loader_exec(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    const pebble_image_info_t *info,
    void **syscall_table,
    int32_t *app_return_out);

typedef struct {
    const pebble_hal_t *hal;
    bool trace_syscalls;
    uint32_t syscall_count;
} pebble_active_ctx_t;

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
} str_buf_t;

static pebble_active_ctx_t g_active_ctx;
static void *g_syscall_table[PEBBLE_SYSCALL_TABLE_SLOTS];

static str_buf_t str_buf_init(char *buf, size_t cap) {
    str_buf_t sb = {.buf = buf, .cap = cap, .pos = 0u};
    if (cap > 0u) {
        buf[0] = '\0';
    }
    return sb;
}

static void str_buf_append(str_buf_t *sb, const char *s) {
    if (s == NULL || sb->cap == 0u) {
        return;
    }
    while (*s != '\0' && sb->pos + 1u < sb->cap) {
        sb->buf[sb->pos++] = *s++;
    }
    sb->buf[sb->pos] = '\0';
}

static void str_buf_append_u32(str_buf_t *sb, uint32_t v) {
    char tmp[12];
    size_t n = 0u;
    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        while (v > 0u && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (n > 0u && sb->pos + 1u < sb->cap) {
        sb->buf[sb->pos++] = tmp[--n];
    }
    if (sb->cap > 0u) {
        sb->buf[sb->pos] = '\0';
    }
}

static void log_line(const pebble_hal_t *hal, pebble_hal_log_level_t level, const char *msg) {
    if (hal != NULL && hal->log != NULL && msg != NULL) {
        hal->log(hal->ctx, level, msg);
    }
}

static void record_syscall(uint32_t slot, bool implemented) {
    g_active_ctx.syscall_count++;
    /* Unimplemented stubs always log a warning; --trace also logs implemented calls. */
    if (implemented && !g_active_ctx.trace_syscalls) {
        return;
    }
    if (g_active_ctx.hal == NULL || g_active_ctx.hal->log == NULL) {
        return;
    }
    char line[160];
    str_buf_t sb = str_buf_init(line, sizeof(line));
    str_buf_append(&sb, "[syscall #");
    str_buf_append_u32(&sb, slot);
    str_buf_append(&sb, "] ");
    str_buf_append(&sb, pebble_syscall_name(slot));
    if (!implemented) {
        str_buf_append(&sb, " (stub)");
    }
    pebble_hal_log_level_t level = implemented ? PEBBLE_HAL_LOG_INFO : PEBBLE_HAL_LOG_WARN;
    g_active_ctx.hal->log(g_active_ctx.hal->ctx, level, line);
}

/*
 * Soft-float ABI bridge for PebbleOS difftime(time_t end, time_t beginning).
 * Enforces hard-float host <-> soft-float .pbw double return via pcs("aapcs").
 */
static double PEBBLE_SOFTFP_ABI pebble_shim_difftime(int32_t end, int32_t beginning) {
    record_syscall(PEBBLE_SYS_DIFFTIME, true);
    return (double)end - (double)beginning;
}

/*
 * -fshort-enums ABI shim for dict_calc_buffer_size_from_tuplets.
 * Reads Pebble Tuplet structs (1-byte TupleType enum) across the .pbw ABI boundary.
 */
static uint32_t pebble_shim_dict_calc_buffer_size_from_tuplets(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    record_syscall(PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS, true);
    if (tuplets == NULL) {
        return 0u;
    }
    /* Dictionary header is 1 byte; each serialized Tuple header is 7 bytes + payload length. */
    uint32_t total = 1u;
    for (uint8_t i = 0u; i < tuplets_count; ++i) {
        const pebble_tuplet_t *t = &tuplets[i];
        uint32_t payload = 0u;
        switch (t->type) {
            case PEBBLE_TUPLE_BYTE_ARRAY:
                payload = t->bytes.length;
                break;
            case PEBBLE_TUPLE_CSTRING:
                payload = t->cstring.length;
                break;
            case PEBBLE_TUPLE_UINT:
            case PEBBLE_TUPLE_INT:
                payload = t->integer.width;
                break;
            default:
                return 0u;
        }
        total += 7u + payload;
    }
    return total;
}

uint64_t pebble_syscall_default_stub(
    uint32_t slot,
    uintptr_t r0,
    uintptr_t r1,
    uintptr_t r2,
    uintptr_t r3) {
    (void)r0;
    (void)r1;
    (void)r2;
    (void)r3;
    record_syscall(slot, false);
    return 0u;
}

static void log_image_banner(const pebble_hal_t *hal, const pebble_image_info_t *info) {
    char line[192];
    str_buf_t sb = str_buf_init(line, sizeof(line));
    str_buf_append(&sb, "Loaded Pebble app '");
    str_buf_append(&sb, info->name);
    str_buf_append(&sb, "' (struct v");
    str_buf_append_u32(&sb, info->struct_version_major);
    str_buf_append(&sb, ".");
    str_buf_append_u32(&sb, info->struct_version_minor);
    str_buf_append(&sb, ", load=");
    str_buf_append_u32(&sb, info->load_size);
    str_buf_append(&sb, ", virt=");
    str_buf_append_u32(&sb, info->virtual_size);
    str_buf_append(&sb, ", relocs=");
    str_buf_append_u32(&sb, info->num_reloc_entries);
    str_buf_append(&sb, ")");
    log_line(hal, PEBBLE_HAL_LOG_INFO, line);
}

pebble_status_t pebble_runtime_run(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    size_t bin_len,
    const pebble_run_opts_t *opts,
    pebble_run_result_t *result_out) {
    if (hal == NULL || bin == NULL) {
        return PEBBLE_ERR_INVALID_ARG;
    }

    pebble_run_opts_t effective_opts = {
        .trace_syscalls = false,
        .max_virtual_size = 0u,
    };
    if (opts != NULL) {
        effective_opts = *opts;
    }

    pebble_image_info_t info;
    pebble_status_t st = pebble_loader_parse(bin, bin_len, effective_opts.max_virtual_size, &info);
    if (st != PEBBLE_OK) {
        char err_line[160];
        str_buf_t sb = str_buf_init(err_line, sizeof(err_line));
        str_buf_append(&sb, "Loader error: ");
        str_buf_append(&sb, pebble_status_str(st));
        log_line(hal, PEBBLE_HAL_LOG_ERROR, err_line);
        if (result_out != NULL) {
            result_out->status = st;
            result_out->app_return_code = -1;
            result_out->syscall_count = 0u;
            result_out->image = info;
        }
        return st;
    }

    pebble_syscall_table_init_defaults(g_syscall_table);
    g_syscall_table[PEBBLE_SYS_DIFFTIME] = (void *)pebble_shim_difftime;
    g_syscall_table[PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS] =
        (void *)pebble_shim_dict_calc_buffer_size_from_tuplets;

    g_active_ctx.hal = hal;
    g_active_ctx.trace_syscalls = effective_opts.trace_syscalls;
    g_active_ctx.syscall_count = 0u;

    log_image_banner(hal, &info);

    int32_t app_rc = 0;
    st = pebble_loader_exec(hal, bin, &info, g_syscall_table, &app_rc);

    if (result_out != NULL) {
        result_out->status = st;
        result_out->app_return_code = app_rc;
        result_out->syscall_count = g_active_ctx.syscall_count;
        result_out->image = info;
    }

    g_active_ctx.hal = NULL;
    return st;
}
