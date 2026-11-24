#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "canvas.h"
#include "pebble_compat.h"
#include "pebble_hal_posix_arm.h"

/* Linux ARM EABI syscall numbers */
#define LINUX_NR_READ 3u
#define LINUX_NR_WRITE 4u
#define LINUX_NR_CLOSE 6u
#define LINUX_NR_MUNMAP 91u
#define LINUX_NR_NANOSLEEP 162u
#define LINUX_NR_POLL 168u
#define LINUX_NR_MMAP2 192u
#define LINUX_NR_CLOCK_GETTIME 263u
#define LINUX_NR_OPENAT 322u
#define LINUX_NR_CACHEFLUSH 0x0f0002u

#define LINUX_AT_FDCWD ((uint32_t)-100)
#define LINUX_O_RDONLY 0u
#define LINUX_O_WRONLY 1u
#define LINUX_O_CREAT 0x40u
#define LINUX_O_TRUNC 0x200u
#define LINUX_MODE_0600 0600u
#define LINUX_PROT_READ 1u
#define LINUX_PROT_WRITE 2u
#define LINUX_PROT_EXEC 4u
#define LINUX_MAP_PRIVATE 0x02u
#define LINUX_MAP_ANONYMOUS 0x20u
#define LINUX_CLOCK_REALTIME 0u
#define LINUX_CLOCK_MONOTONIC 1u
#define LINUX_PAGE_SIZE 4096u
#define LINUX_POLLIN 0x0001
#define LINUX_POLLHUP 0x0010

#define MAX_INPUT_FILE_BYTES (1024u * 1024u)

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

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} linux_pollfd_t;

/* Halo's verbatim physical rotated BGR888 framebuffer + logical RGB888 readback buffer */
static uint8_t g_phys_canvas_fb[PHYS_HEIGHT][PHYS_WIDTH][3];
static uint8_t g_stream_rgb888[PEBBLE_HAL_CANVAS_WIDTH * PEBBLE_HAL_CANVAS_HEIGHT * 3u];

static bool write_all(uint32_t fd, const uint8_t *buf, size_t len) {
    size_t off = 0u;
    while (off < len) {
        int32_t rc = linux_syscall6(
            LINUX_NR_WRITE, fd, (uint32_t)(uintptr_t)(buf + off), (uint32_t)(len - off), 0u, 0u, 0u);
        if (rc <= 0) {
            return false;
        }
        off += (size_t)rc;
    }
    return true;
}

void posix_arm_write_str(uint32_t fd, const char *s) {
    write_all(fd, (const uint8_t *)s, strlen(s));
}

void posix_arm_write_u32(uint32_t fd, uint32_t v) {
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
        write_all(fd, (const uint8_t *)&tmp[n], 1u);
    }
}

void posix_arm_write_i32(uint32_t fd, int32_t v) {
    uint32_t mag;
    if (v < 0) {
        write_all(fd, (const uint8_t *)"-", 1u);
        mag = (uint32_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint32_t)v;
    }
    posix_arm_write_u32(fd, mag);
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
        pebble_epoch_to_wall_time((int64_t)ts.tv_sec, tm_out);
    }
    return true;
}

static void posix_flush_canvas(void *ctx, const uint8_t *rgb888, uint32_t width, uint32_t height) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    if (rgb888 == NULL || width != LOG_WIDTH || height != LOG_HEIGHT) {
        return;
    }

    /* Route every frame through Halo firmware's verbatim canvas.c (90-deg rotated physical BGR). */
    Canvas canvas;
    canvas_init(&canvas, g_phys_canvas_fb);
    canvas_draw_bitmap(&canvas, 0, 0, (int)width, (int)height, rgb888);

    if (hctx == NULL || !hctx->stream_frames) {
        return;
    }

    /* Read back from Halo's physical rotated BGR framebuffer into logical RGB888 for HaloEmulator. */
    for (uint32_t y = 0u; y < LOG_HEIGHT; ++y) {
        uint32_t phys_col = PHYS_WIDTH - 1u - y;
        uint8_t *dst_row = &g_stream_rgb888[y * LOG_WIDTH * 3u];
        for (uint32_t x = 0u; x < LOG_WIDTH; ++x) {
            const uint8_t *phys_px = g_phys_canvas_fb[x][phys_col];
            dst_row[x * 3u + 0u] = phys_px[2]; /* R */
            dst_row[x * 3u + 1u] = phys_px[1]; /* G */
            dst_row[x * 3u + 2u] = phys_px[0]; /* B */
        }
    }

    uint32_t payload_len = (uint32_t)sizeof(g_stream_rgb888);
    uint8_t header[8] = {
        'H', 'A', 'L', 'O',
        (uint8_t)(payload_len & 0xffu),
        (uint8_t)((payload_len >> 8) & 0xffu),
        (uint8_t)((payload_len >> 16) & 0xffu),
        (uint8_t)((payload_len >> 24) & 0xffu),
    };
    write_all(1u, header, sizeof(header));
    write_all(1u, g_stream_rgb888, sizeof(g_stream_rgb888));
}

static bool decode_stdin_byte(uint8_t ch, pebble_hal_event_t *out_event) {
    out_event->value = PEBBLE_HAL_BTN_PRESS_SHORT;
    switch (ch) {
        case 'S':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_SELECT;
            return true;
        case 's':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_SELECT;
            out_event->value = PEBBLE_HAL_BTN_PRESS_LONG;
            return true;
        case 'B':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_BACK;
            return true;
        case 'L':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_BACK;
            out_event->value = PEBBLE_HAL_BTN_PRESS_LONG;
            return true;
        case 'U':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_UP;
            return true;
        case 'u':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_UP;
            out_event->value = PEBBLE_HAL_BTN_PRESS_LONG;
            return true;
        case 'D':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_DOWN;
            return true;
        case 'd':
            out_event->type = PEBBLE_HAL_EVENT_BUTTON_DOWN;
            out_event->value = PEBBLE_HAL_BTN_PRESS_LONG;
            return true;
        case 'T':
            out_event->type = PEBBLE_HAL_EVENT_IMU_TAP_SINGLE;
            out_event->value = 1;
            return true;
        case '2':
            out_event->type = PEBBLE_HAL_EVENT_IMU_TAP_DOUBLE;
            out_event->value = 2;
            return true;
        case '3':
            out_event->type = PEBBLE_HAL_EVENT_IMU_TAP_TRIPLE;
            out_event->value = 3;
            return true;
        case 'Q':
        case 'q':
            out_event->type = PEBBLE_HAL_EVENT_QUIT;
            return true;
        default:
            return false;
    }
}

static bool posix_poll_event(void *ctx, pebble_hal_event_t *out_event) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    if (out_event == NULL || hctx == NULL || !hctx->stream_frames) {
        return false;
    }

    int32_t timeout_ms = 0;
    if (hctx->live_sleep_on_poll) {
        if (hctx->skip_next_idle_sleep) {
            hctx->skip_next_idle_sleep = false;
            timeout_ms = 0;
        } else {
            timeout_ms = 5;
        }
    }

    for (;;) {
        linux_pollfd_t pfd = {.fd = 0, .events = LINUX_POLLIN, .revents = 0};
        int32_t prc = linux_syscall6(
            LINUX_NR_POLL, (uint32_t)(uintptr_t)&pfd, 1u, (uint32_t)timeout_ms, 0u, 0u, 0u);
        timeout_ms = 0; /* Never sleep twice in a single poll call. */
        if (prc <= 0) {
            return false;
        }
        if ((pfd.revents & LINUX_POLLIN) != 0) {
            uint8_t ch = 0u;
            int32_t n = linux_syscall6(LINUX_NR_READ, 0u, (uint32_t)(uintptr_t)&ch, 1u, 0u, 0u, 0u);
            if (n <= 0) {
                out_event->type = PEBBLE_HAL_EVENT_QUIT;
                out_event->value = 0;
                return true;
            }
            if (ch == '\n' || ch == '\r' || ch == ' ') {
                continue;
            }
            if (decode_stdin_byte(ch, out_event)) {
                hctx->skip_next_idle_sleep = true;
                return true;
            }
            continue;
        }
        if ((pfd.revents & LINUX_POLLHUP) != 0) {
            out_event->type = PEBBLE_HAL_EVENT_QUIT;
            out_event->value = 0;
            return true;
        }
        return false;
    }
}

static size_t posix_read_resource(void *ctx, uint32_t offset, void *dst, size_t len) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    if (hctx == NULL || hctx->res_data == NULL || dst == NULL || len == 0u) {
        return 0u;
    }
    if ((size_t)offset >= hctx->res_len) {
        return 0u;
    }
    size_t avail = hctx->res_len - (size_t)offset;
    size_t to_copy = (len < avail) ? len : avail;
    memcpy(dst, hctx->res_data + offset, to_copy);
    return to_copy;
}

static size_t posix_read_persist(void *ctx, void *dst, size_t max_len) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    if (hctx == NULL || hctx->persist_path == NULL || dst == NULL || max_len == 0u) {
        return 0u;
    }
    int32_t fd = linux_syscall6(
        LINUX_NR_OPENAT,
        LINUX_AT_FDCWD,
        (uint32_t)(uintptr_t)hctx->persist_path,
        LINUX_O_RDONLY,
        0u,
        0u,
        0u);
    if (fd < 0) {
        return 0u;
    }
    uint8_t *buf = (uint8_t *)dst;
    size_t total = 0u;
    while (total < max_len) {
        int32_t n = linux_syscall6(
            LINUX_NR_READ,
            (uint32_t)fd,
            (uint32_t)(uintptr_t)(buf + total),
            (uint32_t)(max_len - total),
            0u,
            0u,
            0u);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    (void)linux_syscall6(LINUX_NR_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
    return total;
}

static bool posix_write_persist(void *ctx, const void *src, size_t len) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    if (hctx == NULL || hctx->persist_path == NULL || (src == NULL && len > 0u)) {
        return false;
    }
    int32_t fd = linux_syscall6(
        LINUX_NR_OPENAT,
        LINUX_AT_FDCWD,
        (uint32_t)(uintptr_t)hctx->persist_path,
        LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC,
        LINUX_MODE_0600,
        0u,
        0u);
    if (fd < 0) {
        return false;
    }
    bool ok = write_all((uint32_t)fd, (const uint8_t *)src, len);
    (void)linux_syscall6(LINUX_NR_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
    return ok;
}

static void posix_log(void *ctx, pebble_hal_log_level_t level, const char *msg) {
    posix_arm_hal_ctx_t *hctx = (posix_arm_hal_ctx_t *)ctx;
    /* In --stream-frames mode stdout carries binary RGB888 packets, so route all logs to stderr. */
    uint32_t fd = ((hctx != NULL && hctx->stream_frames) || level >= PEBBLE_HAL_LOG_ERROR) ? 2u : 1u;
    posix_arm_write_str(fd, msg);
    write_all(fd, (const uint8_t *)"\n", 1u);
}

void posix_arm_hal_init(pebble_hal_t *hal_out, posix_arm_hal_ctx_t *ctx) {
    hal_out->ctx = ctx;
    hal_out->alloc_exec = posix_alloc_exec;
    hal_out->free_exec = posix_free_exec;
    hal_out->sync_icache = posix_sync_icache;
    hal_out->get_time_ms = posix_get_time_ms;
    hal_out->get_wall_time = posix_get_wall_time;
    hal_out->flush_canvas = posix_flush_canvas;
    hal_out->poll_event = posix_poll_event;
    hal_out->read_resource = posix_read_resource;
    hal_out->read_persist = posix_read_persist;
    hal_out->write_persist = posix_write_persist;
    hal_out->log = posix_log;
}

uint8_t *posix_arm_read_file(const char *path, size_t *len_out) {
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
            posix_arm_free_file(buf);
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

void posix_arm_free_file(uint8_t *buf) {
    if (buf != NULL) {
        (void)linux_syscall6(
            LINUX_NR_MUNMAP, (uint32_t)(uintptr_t)buf, MAX_INPUT_FILE_BYTES, 0u, 0u, 0u, 0u);
    }
}
