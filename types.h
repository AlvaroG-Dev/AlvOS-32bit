// types.h
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Tipos básicos del sistema
typedef int32_t pid_t;      // ID de proceso
typedef int32_t fd_t;       // Descriptor de archivo
typedef uint32_t mode_t;    // Permisos de archivo
typedef int32_t ssize_t;    // Signed size_t
typedef uint32_t uid_t;     // ID de usuario
typedef uint32_t gid_t;     // ID de grupo
typedef int32_t off_t;      // Offset de archivo
typedef uint32_t dev_t;     // Número de dispositivo
typedef uint32_t ino_t;     // Número de inodo
typedef uint32_t nlink_t;   // Número de enlaces
typedef uint32_t blksize_t; // Tamaño de bloque
typedef uint32_t blkcnt_t;  // Contador de bloques
typedef uint32_t time_t;    // Tiempo

// Para compatibilidad
#ifndef __SIZE_TYPE__
#define __SIZE_TYPE__ unsigned int
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif // TYPES_H