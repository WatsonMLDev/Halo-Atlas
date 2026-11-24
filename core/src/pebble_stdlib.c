#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

/* --- Dedicated 64 KB app heap (freestanding block allocator) --- */

#define PEBBLE_APP_HEAP_BYTES (64u * 1024u)

typedef struct heap_block {
    uint32_t size; /* payload size in bytes (8-byte aligned) */
    uint32_t used;
    struct heap_block *next;
} heap_block_t;

static uint8_t g_app_heap[PEBBLE_APP_HEAP_BYTES] __attribute__((aligned(8)));
static heap_block_t *g_heap_head;

void pebble_stdlib_reset(void) {
    g_heap_head = (heap_block_t *)g_app_heap;
    g_heap_head->size = PEBBLE_APP_HEAP_BYTES - (uint32_t)sizeof(heap_block_t);
    g_heap_head->used = 0u;
    g_heap_head->next = NULL;
}

static size_t align8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

void *pebble_app_malloc(size_t size) {
    if (size == 0u) {
        size = 8u;
    }
    size_t want = align8(size);
    if (g_heap_head == NULL) {
        pebble_stdlib_reset();
    }

    for (heap_block_t *blk = g_heap_head; blk != NULL; blk = blk->next) {
        if (blk->used != 0u || blk->size < want) {
            continue;
        }
        size_t min_split = want + sizeof(heap_block_t) + 8u;
        if (blk->size >= min_split) {
            uint8_t *split_addr = (uint8_t *)(blk + 1) + want;
            heap_block_t *next_blk = (heap_block_t *)split_addr;
            next_blk->size = (uint32_t)(blk->size - want - sizeof(heap_block_t));
            next_blk->used = 0u;
            next_blk->next = blk->next;
            blk->size = (uint32_t)want;
            blk->next = next_blk;
        }
        blk->used = 1u;
        return (void *)(blk + 1);
    }
    return NULL;
}

void *pebble_app_calloc(size_t count, size_t size) {
    if (count != 0u && size > PEBBLE_APP_HEAP_BYTES / count) {
        return NULL;
    }
    size_t total = count * size;
    uint8_t *ptr = (uint8_t *)pebble_app_malloc(total);
    if (ptr != NULL) {
        for (size_t i = 0u; i < total; ++i) {
            ptr[i] = 0u;
        }
    }
    return ptr;
}

void pebble_app_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t lo = (uintptr_t)g_app_heap + sizeof(heap_block_t);
    uintptr_t hi = (uintptr_t)g_app_heap + PEBBLE_APP_HEAP_BYTES;
    if (p < lo || p >= hi) {
        return;
    }
    heap_block_t *blk = ((heap_block_t *)ptr) - 1;
    blk->used = 0u;
    /* Coalesce adjacent free blocks. */
    for (heap_block_t *cur = g_heap_head; cur != NULL && cur->next != NULL;) {
        if (cur->used == 0u && cur->next->used == 0u) {
            cur->size += (uint32_t)sizeof(heap_block_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* --- PebbleOS libc shims --- */

static void *shim_malloc(size_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_MALLOC, true);
    return pebble_app_malloc(size);
}

static void *shim_calloc(size_t count, size_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_CALLOC, true);
    return pebble_app_calloc(count, size);
}

static void shim_free(void *ptr) {
    pebble_rt_record_syscall(PEBBLE_SYS_FREE, true);
    pebble_app_free(ptr);
}

static void *shim_memset(void *dst, int c, size_t n) {
    pebble_rt_record_syscall(PEBBLE_SYS_MEMSET, true);
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0u; i < n; ++i) {
        d[i] = v;
    }
    return dst;
}

static void *shim_memcpy(void *dst, const void *src, size_t n) {
    pebble_rt_record_syscall(PEBBLE_SYS_MEMCPY, true);
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0u; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

static void *shim_memmove(void *dst, const void *src, size_t n) {
    pebble_rt_record_syscall(PEBBLE_SYS_MEMMOVE, true);
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0u; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0u; --i) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

static size_t shim_strlen(const char *s) {
    pebble_rt_record_syscall(PEBBLE_SYS_STRLEN, true);
    size_t n = 0u;
    if (s == NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static char *shim_strcpy(char *dst, const char *src) {
    pebble_rt_record_syscall(PEBBLE_SYS_STRCPY, true);
    size_t i = 0u;
    while (src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    return dst;
}

static char *shim_strncpy(char *dst, const char *src, size_t n) {
    pebble_rt_record_syscall(PEBBLE_SYS_STRNCPY, true);
    size_t i = 0u;
    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
    return dst;
}

/* --- Freestanding snprintf --- */

typedef struct {
    char *buf;
    size_t cap;
    size_t count;
} fmt_out_t;

static void fmt_putc(fmt_out_t *out, char c) {
    if (out->buf != NULL && out->cap > 0u && out->count + 1u < out->cap) {
        out->buf[out->count] = c;
    }
    out->count++;
}

static void fmt_puts_padded(fmt_out_t *out, const char *s, size_t len, int width, bool left, char pad) {
    int pad_count = (width > (int)len) ? (width - (int)len) : 0;
    if (!left) {
        for (int i = 0; i < pad_count; ++i) {
            fmt_putc(out, pad);
        }
    }
    for (size_t i = 0u; i < len; ++i) {
        fmt_putc(out, s[i]);
    }
    if (left) {
        for (int i = 0; i < pad_count; ++i) {
            fmt_putc(out, ' ');
        }
    }
}

static int pebble_vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    fmt_out_t out = {.buf = str, .cap = size, .count = 0u};
    if (fmt == NULL) {
        if (str != NULL && size > 0u) {
            str[0] = '\0';
        }
        return 0;
    }

    for (const char *p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            fmt_putc(&out, *p);
            continue;
        }
        ++p;
        bool left = false;
        bool zero_pad = false;
        bool plus_sign = false;
        while (*p == '-' || *p == '0' || *p == '+') {
            if (*p == '-') left = true;
            if (*p == '0') zero_pad = true;
            if (*p == '+') plus_sign = true;
            ++p;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            ++p;
        }
        int precision = -1;
        if (*p == '.') {
            ++p;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                ++p;
            }
        }
        while (*p == 'l' || *p == 'h') {
            ++p;
        }

        char spec = *p;
        if (spec == '\0') {
            break;
        }
        if (spec == '%') {
            fmt_putc(&out, '%');
        } else if (spec == 'c') {
            char ch = (char)va_arg(ap, int);
            fmt_puts_padded(&out, &ch, 1u, width, left, ' ');
        } else if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t len = 0u;
            while (s[len] != '\0') {
                if (precision >= 0 && (int)len >= precision) {
                    break;
                }
                ++len;
            }
            fmt_puts_padded(&out, s, len, width, left, ' ');
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X') {
            bool is_signed = (spec == 'd' || spec == 'i');
            uint32_t base = (spec == 'x' || spec == 'X') ? 16u : 10u;
            const char *digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            bool neg = false;
            uint32_t uval = 0u;
            if (is_signed) {
                int32_t sval = va_arg(ap, int32_t);
                if (sval < 0) {
                    neg = true;
                    uval = (uint32_t)(-(sval + 1)) + 1u;
                } else {
                    uval = (uint32_t)sval;
                }
            } else {
                uval = va_arg(ap, uint32_t);
            }

            char tmp[16];
            size_t n = 0u;
            if (uval == 0u) {
                tmp[n++] = '0';
            } else {
                while (uval > 0u && n < sizeof(tmp)) {
                    tmp[n++] = digits[uval % base];
                    uval /= base;
                }
            }
            char formatted[20];
            size_t flen = 0u;
            char sign_ch = neg ? '-' : (plus_sign && is_signed ? '+' : '\0');
            if (sign_ch != '\0' && (!zero_pad || left)) {
                formatted[flen++] = sign_ch;
                sign_ch = '\0';
            }
            if (sign_ch != '\0') {
                fmt_putc(&out, sign_ch);
                if (width > 0) {
                    --width;
                }
            }
            while (n > 0u) {
                formatted[flen++] = tmp[--n];
            }
            fmt_puts_padded(&out, formatted, flen, width, left, (zero_pad && !left) ? '0' : ' ');
        } else {
            fmt_putc(&out, '%');
            fmt_putc(&out, spec);
        }
    }

    if (str != NULL && size > 0u) {
        size_t term = (out.count < size) ? out.count : (size - 1u);
        str[term] = '\0';
    }
    return (int)out.count;
}

static int shim_snprintf(char *str, size_t size, const char *fmt, ...) {
    pebble_rt_record_syscall(PEBBLE_SYS_SNPRINTF, true);
    va_list ap;
    va_start(ap, fmt);
    int n = pebble_vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- Pebble 16-bit trig lookup (TRIG_MAX_ANGLE = 0x10000, TRIG_MAX_RATIO = 0x10000) --- */

static const int32_t kSinQuarterTable[257] = {
        0,   402,   804,  1206,  1608,  2010,  2412,  2814,  3216,  3617,  4019,  4420,
     4821,  5222,  5623,  6023,  6424,  6824,  7224,  7623,  8022,  8421,  8820,  9218,
     9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391, 12785, 13180, 13573, 13966,
    14359, 14751, 15143, 15534, 15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699, 22078, 22457, 22834, 23210,
    23586, 23961, 24335, 24708, 25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538, 30893, 31248, 31600, 31952,
    32303, 32652, 33000, 33347, 33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716, 39040, 39362, 39683, 40002,
    40320, 40636, 40951, 41264, 41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056, 46341, 46624, 46906, 47186,
    47464, 47741, 48015, 48288, 48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398, 52639, 52878, 53114, 53349,
    53581, 53812, 54040, 54267, 54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607, 57798, 57986, 58172, 58356,
    58538, 58718, 58896, 59071, 59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568, 61705, 61839, 61971, 62101,
    62228, 62353, 62476, 62596, 62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197, 64277, 64354, 64429, 64501,
    64571, 64639, 64704, 64766, 64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436, 65457, 65476, 65492, 65505,
    65516, 65525, 65531, 65535, 65536,
};

int32_t pebble_sin_lookup_raw(int32_t angle) {
    uint32_t a = (uint32_t)angle & 0xFFFFu;
    bool neg = false;
    if (a >= 0x8000u) {
        a -= 0x8000u;
        neg = true;
    }
    if (a > 0x4000u) {
        a = 0x8000u - a;
    }
    uint32_t idx = a >> 6;
    uint32_t frac = a & 63u;
    int32_t v0 = kSinQuarterTable[idx];
    int32_t v1 = (idx < 256u) ? kSinQuarterTable[idx + 1u] : 65536;
    int32_t val = v0 + (((v1 - v0) * (int32_t)frac) >> 6);
    return neg ? -val : val;
}

int32_t pebble_cos_lookup_raw(int32_t angle) {
    return pebble_sin_lookup_raw(angle + 0x4000);
}

static int32_t shim_sin_lookup(int32_t angle) {
    pebble_rt_record_syscall(PEBBLE_SYS_SIN_LOOKUP, true);
    return pebble_sin_lookup_raw(angle);
}

static int32_t shim_cos_lookup(int32_t angle) {
    pebble_rt_record_syscall(PEBBLE_SYS_COS_LOOKUP, true);
    return pebble_cos_lookup_raw(angle);
}

static int32_t shim_atan2_lookup(int16_t y, int16_t x) {
    pebble_rt_record_syscall(PEBBLE_SYS_ATAN2_LOOKUP, true);
    if (x == 0 && y == 0) {
        return 0;
    }
    int32_t ay = (y < 0) ? -(int32_t)y : (int32_t)y;
    int32_t ax = (x < 0) ? -(int32_t)x : (int32_t)x;
    int32_t mn = (ax < ay) ? ax : ay;
    int32_t mx = (ax < ay) ? ay : ax;
    /* Ratio r in [0..65536]; atan(r) ~= 0x2000 * r + 0x0528 * r * (1 - r) in Pebble units. */
    int32_t r = (mn * 65536) / mx;
    int32_t ang = (r * 0x2000) / 65536 + (int32_t)(((int64_t)r * (65536 - r) * 0x0528) >> 32);
    if (ay > ax) {
        ang = 0x4000 - ang;
    }
    if (x < 0) {
        ang = 0x8000 - ang;
    }
    if (y < 0) {
        ang = (0x10000 - ang) & 0xFFFF;
    }
    return ang & 0xFFFF;
}

/* --- Time & strftime shims --- */

static pebble_tm_t g_localtime_buf;

static int32_t shim_time(int32_t *tloc) {
    pebble_rt_record_syscall(PEBBLE_SYS_TIME, true);
    int32_t now = (int32_t)pebble_rt_current_epoch();
    if (tloc != NULL) {
        *tloc = now;
    }
    return now;
}

static pebble_tm_t *shim_localtime(const int32_t *timep) {
    pebble_rt_record_syscall(PEBBLE_SYS_LOCALTIME, true);
    int64_t epoch = (timep != NULL) ? (int64_t)(*timep) : pebble_rt_current_epoch();
    pebble_rt_epoch_to_tm(epoch, &g_localtime_buf);
    return &g_localtime_buf;
}

static size_t str_append_str(char *dst, size_t maxsize, size_t pos, const char *s) {
    while (*s != '\0' && pos + 1u < maxsize) {
        dst[pos++] = *s++;
    }
    return pos;
}

static size_t str_append_2d(char *dst, size_t maxsize, size_t pos, int32_t v, char pad) {
    if (v < 0) v = 0;
    int32_t tens = (v / 10) % 10;
    int32_t ones = v % 10;
    if (pos + 1u < maxsize) {
        dst[pos++] = (tens == 0 && pad != '0') ? pad : (char)('0' + tens);
    }
    if (pos + 1u < maxsize) {
        dst[pos++] = (char)('0' + ones);
    }
    return pos;
}

static size_t shim_strftime(char *s, size_t maxsize, const char *format, const pebble_tm_t *tm_p) {
    pebble_rt_record_syscall(PEBBLE_SYS_STRFTIME, true);
    if (s == NULL || maxsize == 0u || format == NULL || tm_p == NULL) {
        return 0u;
    }
    static const char *const kWdayShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const kWdayLong[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char *const kMonShort[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char *const kMonLong[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    size_t pos = 0u;
    for (const char *p = format; *p != '\0' && pos + 1u < maxsize; ++p) {
        if (*p != '%') {
            s[pos++] = *p;
            continue;
        }
        ++p;
        if (*p == '\0') {
            break;
        }
        int32_t h12 = tm_p->tm_hour % 12;
        if (h12 == 0) h12 = 12;
        int32_t wday = (tm_p->tm_wday >= 0 && tm_p->tm_wday < 7) ? tm_p->tm_wday : 0;
        int32_t mon = (tm_p->tm_mon >= 0 && tm_p->tm_mon < 12) ? tm_p->tm_mon : 0;

        switch (*p) {
            case '%': s[pos++] = '%'; break;
            case 'H': pos = str_append_2d(s, maxsize, pos, tm_p->tm_hour, '0'); break;
            case 'I': pos = str_append_2d(s, maxsize, pos, h12, '0'); break;
            case 'M': pos = str_append_2d(s, maxsize, pos, tm_p->tm_min, '0'); break;
            case 'S': pos = str_append_2d(s, maxsize, pos, tm_p->tm_sec, '0'); break;
            case 'm': pos = str_append_2d(s, maxsize, pos, tm_p->tm_mon + 1, '0'); break;
            case 'd': pos = str_append_2d(s, maxsize, pos, tm_p->tm_mday, '0'); break;
            case 'e': pos = str_append_2d(s, maxsize, pos, tm_p->tm_mday, ' '); break;
            case 'y': pos = str_append_2d(s, maxsize, pos, (1900 + tm_p->tm_year) % 100, '0'); break;
            case 'Y': {
                int32_t yr = 1900 + tm_p->tm_year;
                pos = str_append_2d(s, maxsize, pos, yr / 100, '0');
                pos = str_append_2d(s, maxsize, pos, yr % 100, '0');
                break;
            }
            case 'a': pos = str_append_str(s, maxsize, pos, kWdayShort[wday]); break;
            case 'A': pos = str_append_str(s, maxsize, pos, kWdayLong[wday]); break;
            case 'b': case 'h': pos = str_append_str(s, maxsize, pos, kMonShort[mon]); break;
            case 'B': pos = str_append_str(s, maxsize, pos, kMonLong[mon]); break;
            case 'p': pos = str_append_str(s, maxsize, pos, tm_p->tm_hour >= 12 ? "PM" : "AM"); break;
            case 'P': pos = str_append_str(s, maxsize, pos, tm_p->tm_hour >= 12 ? "pm" : "am"); break;
            case 'R':
                pos = str_append_2d(s, maxsize, pos, tm_p->tm_hour, '0');
                if (pos + 1u < maxsize) s[pos++] = ':';
                pos = str_append_2d(s, maxsize, pos, tm_p->tm_min, '0');
                break;
            case 'T':
                pos = str_append_2d(s, maxsize, pos, tm_p->tm_hour, '0');
                if (pos + 1u < maxsize) s[pos++] = ':';
                pos = str_append_2d(s, maxsize, pos, tm_p->tm_min, '0');
                if (pos + 1u < maxsize) s[pos++] = ':';
                pos = str_append_2d(s, maxsize, pos, tm_p->tm_sec, '0');
                break;
            default:
                break;
        }
    }
    s[pos] = '\0';
    return pos;
}

static void shim_clock_copy_time_string(char *buffer, uint8_t size) {
    pebble_rt_record_syscall(PEBBLE_SYS_CLOCK_COPY_TIME_STRING, true);
    if (buffer == NULL || size == 0u) {
        return;
    }
    pebble_tm_t tm_now;
    pebble_rt_epoch_to_tm(pebble_rt_current_epoch(), &tm_now);
    (void)shim_strftime(buffer, (size_t)size, "%H:%M", &tm_now);
}

static bool shim_clock_is_24h_style(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_CLOCK_IS_24H_STYLE, true);
    return true;
}

void pebble_stdlib_register_syscalls(void **table) {
    table[PEBBLE_SYS_MALLOC] = (void *)shim_malloc;
    table[PEBBLE_SYS_CALLOC] = (void *)shim_calloc;
    table[PEBBLE_SYS_FREE] = (void *)shim_free;
    table[PEBBLE_SYS_MEMSET] = (void *)shim_memset;
    table[PEBBLE_SYS_MEMCPY] = (void *)shim_memcpy;
    table[PEBBLE_SYS_MEMMOVE] = (void *)shim_memmove;
    table[PEBBLE_SYS_STRLEN] = (void *)shim_strlen;
    table[PEBBLE_SYS_STRCPY] = (void *)shim_strcpy;
    table[PEBBLE_SYS_STRNCPY] = (void *)shim_strncpy;
    table[PEBBLE_SYS_SNPRINTF] = (void *)shim_snprintf;
    table[PEBBLE_SYS_SIN_LOOKUP] = (void *)shim_sin_lookup;
    table[PEBBLE_SYS_COS_LOOKUP] = (void *)shim_cos_lookup;
    table[PEBBLE_SYS_ATAN2_LOOKUP] = (void *)shim_atan2_lookup;
    table[PEBBLE_SYS_TIME] = (void *)shim_time;
    table[PEBBLE_SYS_TIME__DEPRECATED] = (void *)shim_time;
    table[PEBBLE_SYS_LOCALTIME] = (void *)shim_localtime;
    table[PEBBLE_SYS_LOCALTIME__DEPRECATED] = (void *)shim_localtime;
    table[PEBBLE_SYS_STRFTIME] = (void *)shim_strftime;
    table[PEBBLE_SYS_CLOCK_COPY_TIME_STRING] = (void *)shim_clock_copy_time_string;
    table[PEBBLE_SYS_CLOCK_IS_24H_STYLE] = (void *)shim_clock_is_24h_style;
}
