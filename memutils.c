#include "memutils.h"

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *dest, int value, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    for (size_t i = 0; i < count; i++) {
        d[i] = (uint8_t)value;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (s < d) {
        // Copia hacia atrás
        for (size_t i = count; i > 0; i--) {
            d[i-1] = s[i-1];
        }
    } else {
        // Copia hacia adelante
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    }
    return dest;
}
