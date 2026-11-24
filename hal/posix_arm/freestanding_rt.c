#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0u; --i) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

void __aeabi_memcpy(void *dst, const void *src, size_t n) {
    (void)memcpy(dst, src, n);
}

void __aeabi_memcpy4(void *dst, const void *src, size_t n) {
    (void)memcpy(dst, src, n);
}

void __aeabi_memcpy8(void *dst, const void *src, size_t n) {
    (void)memcpy(dst, src, n);
}

void __aeabi_memmove(void *dst, const void *src, size_t n) {
    (void)memmove(dst, src, n);
}

void __aeabi_memset(void *dst, size_t n, int c) {
    (void)memset(dst, c, n);
}

void __aeabi_memset4(void *dst, size_t n, int c) {
    (void)memset(dst, c, n);
}

void __aeabi_memclr(void *dst, size_t n) {
    (void)memset(dst, 0, n);
}

void __aeabi_memclr4(void *dst, size_t n) {
    (void)memset(dst, 0, n);
}

void __aeabi_memclr8(void *dst, size_t n) {
    (void)memset(dst, 0, n);
}
