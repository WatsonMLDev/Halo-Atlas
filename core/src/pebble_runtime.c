#include "pebble_compat.h"
#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

typedef struct {
    const pebble_hal_t *hal;
    pebble_run_opts_t opts;
    uint32_t syscall_count;
    uint32_t frames_flushed;
    int64_t sim_epoch;
    bool use_sim_clock;
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

const pebble_hal_t *pebble_rt_hal(void) {
    return g_active_ctx.hal;
}

const pebble_run_opts_t *pebble_rt_opts(void) {
    return &g_active_ctx.opts;
}

void pebble_rt_note_frame_flushed(void) {
    g_active_ctx.frames_flushed++;
}

int64_t pebble_rt_current_epoch(void) {
    if (g_active_ctx.use_sim_clock) {
        return g_active_ctx.sim_epoch;
    }
    if (g_active_ctx.hal != NULL && g_active_ctx.hal->get_wall_time != NULL) {
        int64_t sec = 0;
        if (g_active_ctx.hal->get_wall_time(g_active_ctx.hal->ctx, &sec, NULL)) {
            return sec;
        }
    }
    return 0;
}

void pebble_rt_advance_sim_epoch(int32_t delta_sec) {
    if (!g_active_ctx.use_sim_clock) {
        g_active_ctx.sim_epoch = pebble_rt_current_epoch();
        g_active_ctx.use_sim_clock = true;
    }
    g_active_ctx.sim_epoch += (int64_t)delta_sec;
}

void pebble_epoch_to_wall_time(int64_t epoch_sec, pebble_hal_wall_time_t *out) {
    if (out == NULL) {
        return;
    }
    if (epoch_sec < 0) {
        epoch_sec = 0;
    }
    uint32_t sec = (uint32_t)epoch_sec;
    int32_t z = (int32_t)(sec / 86400u);
    uint32_t sod = sec % 86400u;

    out->sec = (int32_t)(sod % 60u);
    out->min = (int32_t)((sod / 60u) % 60u);
    out->hour = (int32_t)(sod / 3600u);
    out->wday = (int32_t)(((uint32_t)z + 4u) % 7u);

    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
    if (m <= 2u) {
        y += 1;
    }

    static const uint16_t kDaysBeforeMonth[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    out->year = y - 1900;
    out->mon = (int32_t)(m - 1u);
    out->mday = (int32_t)d;
    out->yday = (int32_t)kDaysBeforeMonth[m - 1u] + (int32_t)d - 1 + ((leap && m > 2u) ? 1 : 0);
    out->isdst = 0;
}

void pebble_rt_epoch_to_tm(int64_t epoch_sec, pebble_tm_t *out) {
    if (out == NULL) {
        return;
    }
    pebble_hal_wall_time_t wt;
    pebble_epoch_to_wall_time(epoch_sec, &wt);
    out->tm_sec = wt.sec;
    out->tm_min = wt.min;
    out->tm_hour = wt.hour;
    out->tm_mday = wt.mday;
    out->tm_mon = wt.mon;
    out->tm_year = wt.year;
    out->tm_wday = wt.wday;
    out->tm_yday = wt.yday;
    out->tm_isdst = wt.isdst;
    out->tm_gmtoff = 0;
    out->tm_zone[0] = 'U';
    out->tm_zone[1] = 'T';
    out->tm_zone[2] = 'C';
    out->tm_zone[3] = '\0';
}

void pebble_rt_record_syscall(uint32_t slot, bool implemented) {
    g_active_ctx.syscall_count++;
    /* Unimplemented stubs always log a warning; --trace also logs implemented calls. */
    if (implemented && !g_active_ctx.opts.trace_syscalls) {
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
    pebble_rt_record_syscall(PEBBLE_SYS_DIFFTIME, true);
    return (double)end - (double)beginning;
}

/*
 * -fshort-enums ABI shim for dict_calc_buffer_size_from_tuplets.
 * Reads Pebble Tuplet structs (1-byte TupleType enum) across the .pbw ABI boundary.
 */
static uint32_t pebble_shim_dict_calc_buffer_size_from_tuplets(
    const pebble_tuplet_t *tuplets,
    uint8_t tuplets_count) {
    pebble_rt_record_syscall(PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS, true);
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
    pebble_rt_record_syscall(slot, false);
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
        .platform = PEBBLE_PLATFORM_CHALK,
        .max_frames = 1u,
        .sim_start_epoch = 0,
        .sim_tick_step_sec = 0,
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
            result_out->frames_flushed = 0u;
            result_out->image = info;
        }
        return st;
    }

    pebble_stdlib_reset();
    pebble_res_reset(hal);
    pebble_font_reset();
    pebble_gfx_reset(effective_opts.platform);
    pebble_ui_reset();

    pebble_syscall_table_init_defaults(g_syscall_table);
    g_syscall_table[PEBBLE_SYS_DIFFTIME] = (void *)pebble_shim_difftime;
    g_syscall_table[PEBBLE_SYS_DICT_CALC_BUFFER_SIZE_FROM_TUPLETS] =
        (void *)pebble_shim_dict_calc_buffer_size_from_tuplets;
    pebble_stdlib_register_syscalls(g_syscall_table);
    pebble_res_register_syscalls(g_syscall_table);
    pebble_font_register_syscalls(g_syscall_table);
    pebble_bitmap_register_syscalls(g_syscall_table);
    pebble_gfx_register_syscalls(g_syscall_table);
    pebble_ui_register_syscalls(g_syscall_table);

    g_active_ctx.hal = hal;
    g_active_ctx.opts = effective_opts;
    g_active_ctx.syscall_count = 0u;
    g_active_ctx.frames_flushed = 0u;
    g_active_ctx.use_sim_clock = (effective_opts.sim_start_epoch > 0);
    g_active_ctx.sim_epoch = effective_opts.sim_start_epoch;

    log_image_banner(hal, &info);

    int32_t app_rc = 0;
    st = pebble_loader_exec(hal, bin, &info, g_syscall_table, &app_rc);

    if (result_out != NULL) {
        result_out->status = st;
        result_out->app_return_code = app_rc;
        result_out->syscall_count = g_active_ctx.syscall_count;
        result_out->frames_flushed = g_active_ctx.frames_flushed;
        result_out->image = info;
    }

    g_active_ctx.hal = NULL;
    return st;
}
