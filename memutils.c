#include "memutils.h"
#include "cpuid.h"
#include "sse.h"

void *memcpy(void *dest, const void *src, size_t count) {
  if (cpu_info.caps.has_sse && count >= 64) {
    return sse_memcpy(dest, src, count);
  }

  // Fallback optimizado con rep movsl si están alineados
  if (((uintptr_t)dest % 4 == 0) && ((uintptr_t)src % 4 == 0)) {
    uint32_t dwords = count / 4;
    uint32_t rem = count % 4;
    __asm__ volatile("rep movsl"
                     : "+D"(dest), "+S"(src), "+c"(dwords)
                     :
                     : "memory");
    if (rem) {
      __asm__ volatile("rep movsb"
                       : "+D"(dest), "+S"(src), "+c"(rem)
                       :
                       : "memory");
    }
  } else {
    __asm__ volatile("rep movsb"
                     : "+D"(dest), "+S"(src), "+c"(count)
                     :
                     : "memory");
  }
  return dest;
}

void *memset(void *dest, int value, size_t count) {
  if (cpu_info.caps.has_sse && count >= 64) {
    return sse_memset(dest, value, count);
  }

  uint8_t v8 = (uint8_t)value;
  if (((uintptr_t)dest % 4 == 0) && count >= 4) {
    uint32_t v32 = (v8 << 24) | (v8 << 16) | (v8 << 8) | v8;
    uint32_t dwords = count / 4;
    uint32_t rem = count % 4;
    __asm__ volatile("rep stosl"
                     : "+D"(dest), "+c"(dwords)
                     : "a"(v32)
                     : "memory");
    if (rem) {
      __asm__ volatile("rep stosb"
                       : "+D"(dest), "+c"(rem)
                       : "a"(v8)
                       : "memory");
    }
  } else {
    __asm__ volatile("rep stosb"
                     : "+D"(dest), "+c"(count)
                     : "a"(v8)
                     : "memory");
  }
  return dest;
}

void *memset32(void *dest, uint32_t value, size_t count) {
  if (cpu_info.caps.has_sse && count >= 16) {
    return sse_memset32(dest, value, count);
  }

  __asm__ volatile("rep stosl"
                   : "+D"(dest), "+c"(count)
                   : "a"(value)
                   : "memory");
  return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (dest == src)
    return dest;

  if (s < d && d < s + count) {
    // Copia hacia atrás (solapamiento)
    for (size_t i = count; i > 0; i--) {
      d[i - 1] = s[i - 1];
    }
  } else {
    // Copia hacia adelante (o sin solapamiento)
    return memcpy(dest, src, count);
  }
  return dest;
}
