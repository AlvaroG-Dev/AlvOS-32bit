#ifndef SSE_H
#define SSE_H

#include <stddef.h>
#include <stdint.h>

// Inicializar SSE y FPU
void sse_init(void);

// Versiones optimizadas de funciones de memoria
void *sse_memcpy(void *dest, const void *src, size_t n);
void *sse_memset(void *dest, int val, size_t n);
void *sse_memset32(void *dest, uint32_t val, size_t n);

#endif
