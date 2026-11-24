#include "pebble_compat.h"
#include "pebble_syscall_table_gen.h"

/* Internal loader execution helper defined in pebble_loader.c. */
pebble_status_t pebble_loader_exec(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    size_t bin_len,
    const pebble_image_info_t *info,
    void **syscall_table,
    int32_t *app_return_out);

typedef struct {
    const pebble_hal_t *hal;
    bool trace_syscalls;
    uint32_t syscall_count;
} pebble_active_ctx_t;

static pebble_active_ctx_t g_active_ctx;
static void *g_syscall_table[PEBBLE_SYSCALL_TABLE_SLOTS];

static size_t buf_append_str(char *buf, size_t cap, size_t pos, const char *s) {
    if (s == NULL) {
        return pos;
    }
    while (*s != '\0' && pos + 1u < cap) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

static size_t buf_append_u32(char *buf, size_t cap, size_t pos, uint32_t v) {
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
    while (n > 0u && pos + 1u < cap) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = '\0';
    return pos;
}

static void log_line(const pebble_hal_t *hal, pebble_hal_log_level_t level, const char *msg) {
    if (hal != NULL && hal->log != NULL && msg != NULL) {
        hal->log(hal->ctx, level, msg);
    }
}

static void trace_syscall(uint32_t slot, bool implemented) {
    g_active_ctx.syscall_count++;
    if (!g_active_ctx.trace_syscalls || g_active_ctx.hal == NULL || g_active_ctx.hal->log == NULL) {
        return;
    }
    char line[160];
    size_t pos = 0u;
    line[0] = '\0';
    pos = buf_append_str(line, sizeof(line), pos, "[syscall #");
    pos = buf_append_u32(line, sizeof(line), pos, slot);
    pos = buf_append_str(line, sizeof(line), pos, "] ");
    pos = buf_append_str(line, sizeof(line), pos, pebble_syscall_name(slot));
    if (!implemented) {
        pos = buf_append_str(line, sizeof(line), pos, " (stub)");
    }
    (void)pos;
    g_active_ctx.hal->log(g_active_ctx.hal->ctx, PEBBLE_HAL_LOG_INFO, line);
}

/*
 * Soft-float ABI bridge for PebbleOS difftime(time_t end, time_t beginning).
 * Demonstrates and enforces hard-float host <-> soft-float .pbw double return via pcs("aapcs").
 */
static double PEBBLE_SOFTFP_ABI pebble_shim_difftime(int32_t end, int32_t beginning) {
    trace_syscall(PEBBLE_SYS_DIFFTIME, true);
    return (double)end - (double)beginning;
}

/*
 * Packed / -fshort-enums ABI shim for dict_calc_buffer_size_from_tuplets.
 * Reads Pebble Tuplet structs (1-byte TupleType enum) across the .pbw ABI boundary.
 */
static uint32_t pebble_shim_dict_calc_buffer_size_from_tuplets(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    trace_syscall(PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS, true);
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
    trace_syscall(slot, false);
    return 0u;
}

static void log_image_banner(const pebble_hal_t *hal, const pebble_image_info_t *info) {
    char line[192];
    size_t pos = 0u;
    line[0] = '\0';
    pos = buf_append_str(line, sizeof(line), pos, "Loaded Pebble app '");
    pos = buf_append_str(line, sizeof(line), pos, info->name);
    pos = buf_append_str(line, sizeof(line), pos, "' (struct v");
    pos = buf_append_u32(line, sizeof(line), pos, info->struct_version_major);
    pos = buf_append_str(line, sizeof(line), pos, ".");
    pos = buf_append_u32(line, sizeof(line), pos, info->struct_version_minor);
    pos = buf_append_str(line, sizeof(line), pos, ", load=");
    pos = buf_append_u32(line, sizeof(line), pos, info->load_size);
    pos = buf_append_str(line, sizeof(line), pos, ", virt=");
    pos = buf_append_u32(line, sizeof(line), pos, info->virtual_size);
    pos = buf_append_str(line, sizeof(line), pos, ", relocs=");
    pos = buf_append_u32(line, sizeof(line), pos, info->num_reloc_entries);
    pos = buf_append_str(line, sizeof(line), pos, ")");
    (void)pos;
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
        size_t pos = 0u;
        err_line[0] = '\0';
        pos = buf_append_str(err_line, sizeof(err_line), pos, "Loader error: ");
        pos = buf_append_str(err_line, sizeof(err_line), pos, pebble_status_str(st));
        (void)pos;
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
    st = pebble_loader_exec(hal, bin, bin_len, &info, g_syscall_table, &app_rc);

    if (result_out != NULL) {
        result_out->status = st;
        result_out->app_return_code = app_rc;
        result_out->syscall_count = g_active_ctx.syscall_count;
        result_out->image = info;
    }

    g_active_ctx.hal = NULL;
    return st;
}
