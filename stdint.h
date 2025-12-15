#ifndef _STDINT_H
#define _STDINT_H

#include <stddef.h>   // para size_t

// Tipos enteros fijos
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

// Para sistemas de 32 bits (ajusta si es 64 bits)
typedef int32_t  intptr_t;
typedef uint32_t uintptr_t;

// ssize_t = signed size_t
typedef intptr_t ssize_t;

#endif
