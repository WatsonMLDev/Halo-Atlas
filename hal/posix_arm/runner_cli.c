#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_compat.h"
#include "pebble_hal_posix_arm.h"

static bool cstr_eq(const char *a, const char *b) {
    size_t i = 0u;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        ++i;
    }
    return a[i] == b[i];
}

static bool parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint64_t val = 0u;
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(s[i] - '0');
        if (val > (0xffffffffffffffffull - digit) / 10u) {
            return false;
        }
        val = val * 10u + digit;
    }
    *out = val;
    return true;
}

static bool parse_platform(const char *s, pebble_platform_t *out) {
    if (cstr_eq(s, "chalk")) {
        *out = PEBBLE_PLATFORM_CHALK;
        return true;
    }
    if (cstr_eq(s, "gabbro")) {
        *out = PEBBLE_PLATFORM_GABBRO;
        return true;
    }
    if (cstr_eq(s, "basalt")) {
        *out = PEBBLE_PLATFORM_BASALT;
        return true;
    }
    if (cstr_eq(s, "aplite")) {
        *out = PEBBLE_PLATFORM_APLITE;
        return true;
    }
    return false;
}

int32_t runner_main(int32_t argc, char **argv) {
    pebble_run_opts_t opts = {
        .trace_syscalls = false,
        .max_virtual_size = 0u,
        .platform = PEBBLE_PLATFORM_CHALK,
        .max_frames = 1u,
        .sim_start_epoch = 0,
        .sim_tick_step_sec = 0,
    };
    bool max_frames_explicit = false;
    posix_arm_hal_ctx_t hal_ctx = {
        .stream_frames = false,
        .live_sleep_on_poll = false,
    };
    const char *app_path = NULL;

    for (int32_t i = 1; i < argc; ++i) {
        if (cstr_eq(argv[i], "--trace")) {
            opts.trace_syscalls = true;
        } else if (cstr_eq(argv[i], "--stream-frames")) {
            hal_ctx.stream_frames = true;
        } else if (cstr_eq(argv[i], "--platform") && i + 1 < argc) {
            if (!parse_platform(argv[++i], &opts.platform)) {
                posix_arm_write_str(2u, "error: invalid --platform (use gabbro|chalk|basalt|aplite)\n");
                return 2;
            }
        } else if (cstr_eq(argv[i], "--max-ram") && i + 1 < argc) {
            uint64_t limit = 0u;
            if (!parse_u64(argv[++i], &limit) || limit == 0u || limit > 0xffffffffull) {
                posix_arm_write_str(2u, "error: invalid --max-ram value\n");
                return 2;
            }
            opts.max_virtual_size = (uint32_t)limit;
        } else if (cstr_eq(argv[i], "--max-frames") && i + 1 < argc) {
            uint64_t n = 0u;
            if (!parse_u64(argv[++i], &n) || n > 0xffffffffull) {
                posix_arm_write_str(2u, "error: invalid --max-frames value\n");
                return 2;
            }
            opts.max_frames = (uint32_t)n;
            max_frames_explicit = true;
        } else if (cstr_eq(argv[i], "--time-epoch") && i + 1 < argc) {
            uint64_t ep = 0u;
            if (!parse_u64(argv[++i], &ep)) {
                posix_arm_write_str(2u, "error: invalid --time-epoch value\n");
                return 2;
            }
            opts.sim_start_epoch = (int64_t)ep;
        } else if (cstr_eq(argv[i], "--tick-step") && i + 1 < argc) {
            uint64_t step = 0u;
            if (!parse_u64(argv[++i], &step) || step > 0x7fffffffull) {
                posix_arm_write_str(2u, "error: invalid --tick-step value\n");
                return 2;
            }
            opts.sim_tick_step_sec = (int32_t)step;
        } else if (argv[i][0] == '-') {
            posix_arm_write_str(2u, "error: unknown flag: ");
            posix_arm_write_str(2u, argv[i]);
            posix_arm_write_str(2u, "\n");
            return 2;
        } else {
            app_path = argv[i];
        }
    }

    if (app_path == NULL) {
        posix_arm_write_str(
            2u,
            "usage: pebble-runner [--trace] [--stream-frames] [--platform gabbro|chalk|basalt|aplite] "
            "[--max-frames N] [--time-epoch SEC] [--tick-step SEC] [--max-ram BYTES] <pebble-app.bin>\n");
        return 2;
    }

    if (hal_ctx.stream_frames && !max_frames_explicit) {
        /* Continuous live streaming until quit event unless --max-frames was specified. */
        opts.max_frames = 0u;
    }
    hal_ctx.live_sleep_on_poll = (opts.sim_tick_step_sec == 0 && opts.max_frames == 0u);

    size_t bin_len = 0u;
    uint8_t *bin = posix_arm_read_file(app_path, &bin_len);
    if (bin == NULL) {
        posix_arm_write_str(2u, "error: failed to read binary: ");
        posix_arm_write_str(2u, app_path);
        posix_arm_write_str(2u, "\n");
        return 2;
    }

    pebble_hal_t hal;
    posix_arm_hal_init(&hal, &hal_ctx);

    pebble_run_result_t result;
    pebble_status_t st = pebble_runtime_run(&hal, bin, bin_len, &opts, &result);

    posix_arm_free_file(bin);

    if (st != PEBBLE_OK) {
        return (int32_t)st;
    }

    uint32_t summary_fd = hal_ctx.stream_frames ? 2u : 1u;
    posix_arm_write_str(summary_fd, "App exited cleanly (rc=");
    posix_arm_write_i32(summary_fd, result.app_return_code);
    posix_arm_write_str(summary_fd, ", syscalls=");
    posix_arm_write_u32(summary_fd, result.syscall_count);
    posix_arm_write_str(summary_fd, ")\n");
    return 0;
}
