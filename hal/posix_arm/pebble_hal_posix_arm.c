#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_compat.h"

/* Linux ARM EABI syscall numbers */
#define LINUX_NR_READ 3u
#define LINUX_NR_WRITE 4u
#define LINUX_NR_CLOSE 6u
#define LINUX_NR_MUNMAP 91u
#define LINUX_NR_MMAP2 192u
#define LINUX_NR_CLOCK_GETTIME 263u
#define LINUX_NR_OPENAT 322u
#define LINUX_NR_CACHEFLUSH 0x0f0002u

#define LINUX_AT_FDCWD ((uint32_t)-100)
#define LINUX_O_RDONLY 0u
#define LINUX_PROT_READ 1u
#define LINUX_PROT_WRITE 2u
#define LINUX_PROT_EXEC 4u
#define LINUX_MAP_PRIVATE 0x02u
#define LINUX_MAP_ANONYMOUS 0x20u
#define LINUX_CLOCK_REALTIME 0u
#define LINUX_CLOCK_MONOTONIC 1u
#define LINUX_PAGE_SIZE 4096u

#define MAX_INPUT_FILE_BYTES (256u * 1024u)

extern int32_t linux_syscall6(
    uint32_t nr,
    uint32_t a0,
    uint32_t a1,
    uint32_t a2,
    uint32_t a3,
    uint32_t a4,
    uint32_t a5);

typedef struct {
    int32_t tv_sec;
    int32_t tv_nsec;
} linux_timespec32_t;

static size_t cstr_len(const char *s) {
    size_t n = 0u;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

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

static bool parse_u32(const char *s, uint32_t *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t val = 0u;
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(s[i] - '0');
        if (val > (0xffffffffu - digit) / 10u) {
            return false;
        }
        val = val * 10u + digit;
    }
    *out = val;
    return true;
}

static void write_all(uint32_t fd, const char *buf, size_t len) {
    size_t off = 0u;
    while (off < len) {
        int32_t rc = linux_syscall6(
            LINUX_NR_WRITE, fd, (uint32_t)(uintptr_t)(buf + off), (uint32_t)(len - off), 0u, 0u, 0u);
        if (rc <= 0) {
            break;
        }
        off += (size_t)rc;
    }
}

static void write_str(uint32_t fd, const char *s) {
    write_all(fd, s, cstr_len(s));
}

static void write_u32(uint32_t fd, uint32_t v) {
    char tmp[16];
    size_t n = 0u;
    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        while (v > 0u && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (n > 0u) {
        --n;
        write_all(fd, &tmp[n], 1u);
    }
}

static void write_i32(uint32_t fd, int32_t v) {
    uint32_t mag;
    if (v < 0) {
        write_all(fd, "-", 1u);
        mag = (uint32_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint32_t)v;
    }
    write_u32(fd, mag);
}

static size_t align_up_page(size_t size) {
    if (size == 0u) {
        return LINUX_PAGE_SIZE;
    }
    return (size + LINUX_PAGE_SIZE - 1u) & ~(size_t)(LINUX_PAGE_SIZE - 1u);
}

static void *posix_alloc_exec(void *ctx, size_t size) {
    (void)ctx;
    size_t map_len = align_up_page(size);
    int32_t res = linux_syscall6(
        LINUX_NR_MMAP2,
        0u,
        (uint32_t)map_len,
        LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
        (uint32_t)-1,
        0u);
    if (res < 0 && res > -4096) {
        return NULL;
    }
    uint8_t *mem = (uint8_t *)(uintptr_t)(uint32_t)res;
    /* Pre-fill entire mapping with non-zero poison so .bss zeroing is verified (matches ITCM). */
    for (size_t i = 0u; i < map_len; ++i) {
        mem[i] = 0xa5u;
    }
    return mem;
}

static void posix_sync_icache(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    if (ptr == NULL || size == 0u) {
        return;
    }
    (void)linux_syscall6(
        LINUX_NR_CACHEFLUSH,
        (uint32_t)(uintptr_t)ptr,
        (uint32_t)((uintptr_t)ptr + size),
        0u,
        0u,
        0u,
        0u);
}

static void posix_free_exec(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    if (ptr == NULL) {
        return;
    }
    size_t map_len = align_up_page(size);
    (void)linux_syscall6(LINUX_NR_MUNMAP, (uint32_t)(uintptr_t)ptr, (uint32_t)map_len, 0u, 0u, 0u, 0u);
}

static uint64_t posix_get_time_ms(void *ctx) {
    (void)ctx;
    linux_timespec32_t ts = {0, 0};
    int32_t rc = linux_syscall6(
        LINUX_NR_CLOCK_GETTIME, LINUX_CLOCK_MONOTONIC, (uint32_t)(uintptr_t)&ts, 0u, 0u, 0u, 0u);
    if (rc != 0) {
        return 0u;
    }
    return (uint64_t)(uint32_t)ts.tv_sec * 1000u + (uint64_t)(uint32_t)ts.tv_nsec / 1000000u;
}

/* Convert Unix days-since-1970-01-01 to Gregorian calendar fields (UTC). */
static void civil_from_days(int32_t z, int32_t *year_out, int32_t *mon_out, int32_t *mday_out, int32_t *yday_out) {
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
    int32_t yday = (int32_t)kDaysBeforeMonth[m - 1u] + (int32_t)d - 1 + ((leap && m > 2u) ? 1 : 0);
    *year_out = y - 1900;
    *mon_out = (int32_t)(m - 1u);
    *mday_out = (int32_t)d;
    *yday_out = yday;
}

static bool posix_get_wall_time(void *ctx, int64_t *unix_sec_out, pebble_hal_wall_time_t *tm_out) {
    (void)ctx;
    linux_timespec32_t ts = {0, 0};
    int32_t rc = linux_syscall6(
        LINUX_NR_CLOCK_GETTIME, LINUX_CLOCK_REALTIME, (uint32_t)(uintptr_t)&ts, 0u, 0u, 0u, 0u);
    if (rc != 0) {
        return false;
    }
    if (unix_sec_out != NULL) {
        *unix_sec_out = (int64_t)ts.tv_sec;
    }
    if (tm_out != NULL) {
        uint32_t sec = (uint32_t)ts.tv_sec;
        int32_t days = (int32_t)(sec / 86400u);
        uint32_t sod = sec % 86400u;
        tm_out->sec = (int32_t)(sod % 60u);
        tm_out->min = (int32_t)((sod / 60u) % 60u);
        tm_out->hour = (int32_t)(sod / 3600u);
        tm_out->wday = (int32_t)(((uint32_t)days + 4u) % 7u);
        civil_from_days(days, &tm_out->year, &tm_out->mon, &tm_out->mday, &tm_out->yday);
        tm_out->isdst = 0;
    }
    return true;
}

static void posix_flush_canvas(void *ctx, const uint8_t *rgb888, uint32_t width, uint32_t height) {
    (void)ctx;
    (void)rgb888;
    (void)width;
    (void)height;
}

static bool posix_poll_event(void *ctx, pebble_hal_event_t *out_event) {
    (void)ctx;
    (void)out_event;
    return false;
}

static size_t posix_read_resource(void *ctx, uint32_t offset, void *dst, size_t len) {
    (void)ctx;
    (void)offset;
    (void)dst;
    (void)len;
    return 0u;
}

static void posix_log(void *ctx, pebble_hal_log_level_t level, const char *msg) {
    (void)ctx;
    uint32_t fd = (level >= PEBBLE_HAL_LOG_ERROR) ? 2u : 1u;
    write_str(fd, msg);
    write_all(fd, "\n", 1u);
}

static uint8_t *read_file_mmap(const char *path, size_t *len_out) {
    int32_t fd = linux_syscall6(
        LINUX_NR_OPENAT, LINUX_AT_FDCWD, (uint32_t)(uintptr_t)path, LINUX_O_RDONLY, 0u, 0u, 0u);
    if (fd < 0) {
        return NULL;
    }

    int32_t map_res = linux_syscall6(
        LINUX_NR_MMAP2,
        0u,
        MAX_INPUT_FILE_BYTES,
        LINUX_PROT_READ | LINUX_PROT_WRITE,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
        (uint32_t)-1,
        0u);
    if (map_res < 0 && map_res > -4096) {
        (void)linux_syscall6(LINUX_NR_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)map_res;
    size_t total = 0u;
    while (total < MAX_INPUT_FILE_BYTES) {
        int32_t n = linux_syscall6(
            LINUX_NR_READ,
            (uint32_t)fd,
            (uint32_t)(uintptr_t)(buf + total),
            (uint32_t)(MAX_INPUT_FILE_BYTES - total),
            0u,
            0u,
            0u);
        if (n < 0) {
            (void)linux_syscall6(LINUX_NR_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
            (void)linux_syscall6(
                LINUX_NR_MUNMAP, (uint32_t)(uintptr_t)buf, MAX_INPUT_FILE_BYTES, 0u, 0u, 0u, 0u);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }

    (void)linux_syscall6(LINUX_NR_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
    *len_out = total;
    return buf;
}

int32_t runner_main(int32_t argc, char **argv) {
    pebble_run_opts_t opts = {
        .trace_syscalls = false,
        .max_virtual_size = 0u,
    };
    const char *app_path = NULL;

    for (int32_t i = 1; i < argc; ++i) {
        if (cstr_eq(argv[i], "--trace")) {
            opts.trace_syscalls = true;
        } else if (cstr_eq(argv[i], "--max-ram") && i + 1 < argc) {
            uint32_t limit = 0u;
            if (!parse_u32(argv[++i], &limit) || limit == 0u) {
                write_str(2u, "error: invalid --max-ram value\n");
                return 2;
            }
            opts.max_virtual_size = limit;
        } else if (argv[i][0] == '-') {
            write_str(2u, "error: unknown flag: ");
            write_str(2u, argv[i]);
            write_str(2u, "\n");
            return 2;
        } else {
            app_path = argv[i];
        }
    }

    if (app_path == NULL) {
        write_str(2u, "usage: pebble-runner [--trace] [--max-ram <bytes>] <pebble-app.bin>\n");
        return 2;
    }

    size_t bin_len = 0u;
    uint8_t *bin = read_file_mmap(app_path, &bin_len);
    if (bin == NULL) {
        write_str(2u, "error: failed to read binary: ");
        write_str(2u, app_path);
        write_str(2u, "\n");
        return 2;
    }

    pebble_hal_t hal = {
        .ctx = NULL,
        .alloc_exec = posix_alloc_exec,
        .free_exec = posix_free_exec,
        .sync_icache = posix_sync_icache,
        .get_time_ms = posix_get_time_ms,
        .get_wall_time = posix_get_wall_time,
        .flush_canvas = posix_flush_canvas,
        .poll_event = posix_poll_event,
        .read_resource = posix_read_resource,
        .log = posix_log,
    };

    pebble_run_result_t result;
    pebble_status_t st = pebble_runtime_run(&hal, bin, bin_len, &opts, &result);

    (void)linux_syscall6(
        LINUX_NR_MUNMAP, (uint32_t)(uintptr_t)bin, MAX_INPUT_FILE_BYTES, 0u, 0u, 0u, 0u);

    if (st != PEBBLE_OK) {
        return (int32_t)st;
    }

    write_str(1u, "App exited cleanly (rc=");
    write_i32(1u, result.app_return_code);
    write_str(1u, ", syscalls=");
    write_u32(1u, result.syscall_count);
    write_str(1u, ")\n");
    return 0;
}
