#ifndef MEMUTILS_H
#define MEMUTILS_H

#include <stddef.h>
#include <stdint.h>


// Copia 'count' bytes de 'src' a 'dest'. Devuelve 'dest'.
void *memcpy(void *dest, const void *src, size_t count);

// Rellena 'count' bytes de 'dest' con el valor 'value'. Devuelve 'dest'.
void *memset(void *dest, int value, size_t count);

// Mueve 'count' bytes de 'src' a 'dest', permitiendo solapamiento. Devuelve 'dest'.
void *memmove(void *dest, const void *src, size_t count);

#endif
