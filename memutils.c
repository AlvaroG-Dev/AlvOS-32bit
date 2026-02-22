#include "memutils.h"

void *memcpy(void *dest, const void *src, size_t count) {
    if (((uintptr_t)dest % 4 == 0) && ((uintptr_t)src % 4 == 0)) {
        uint32_t *d = (uint32_t *)dest;
        const uint32_t *s = (const uint32_t *)src;
        size_t dwords = count / 4;
        for (size_t i = 0; i < dwords; i++) {
            d[i] = s[i];
        }
        uint8_t *d8 = (uint8_t *)(d + dwords);
        const uint8_t *s8 = (const uint8_t *)(s + dwords);
        for (size_t i = 0; i < (count % 4); i++) {
            d8[i] = s8[i];
        }
    } else {
        uint8_t *d = (uint8_t *)dest;
        const uint8_t *s = (const uint8_t *)src;
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    }
    return dest;
}

void *memset(void *dest, int value, size_t count) {
    uint8_t v8 = (uint8_t)value;
    if (((uintptr_t)dest % 4 == 0) && count >= 4) {
        uint32_t v32 = (v8 << 24) | (v8 << 16) | (v8 << 8) | v8;
        uint32_t *d = (uint32_t *)dest;
        size_t dwords = count / 4;
        for (size_t i = 0; i < dwords; i++) {
            d[i] = v32;
        }
        uint8_t *d8 = (uint8_t *)(d + dwords);
        for (size_t i = 0; i < (count % 4); i++) {
            d8[i] = v8;
        }
    } else {
        uint8_t *d = (uint8_t *)dest;
        for (size_t i = 0; i < count; i++) {
            d[i] = v8;
        }
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (dest == src) return dest;

    if (s < d && d < s + count) {
        // Copia hacia atrás (solapamiento)
        for (size_t i = count; i > 0; i--) {
            d[i-1] = s[i-1];
        }
    } else {
        // Copia hacia adelante (o sin solapamiento)
        return memcpy(dest, src, count);
    }
    return dest;
}

