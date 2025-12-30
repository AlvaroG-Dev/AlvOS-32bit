#ifndef EXEC_H
#define EXEC_H

#include "task.h"
#include "vfs.h"
#include <stdint.h>

// Configuración de memoria para ejecutables
#define EXEC_CODE_BASE 0x00400000       // 4MB - donde se carga el código
#define EXEC_STACK_SIZE (16 * 1024)     // 16KB de stack por defecto
#define EXEC_MAX_SIZE (2 * 1024 * 1024) // 2MB máximo por ejecutable

// Información de un ejecutable cargado
typedef struct {
  char path[VFS_PATH_MAX];
  void *code_base;
  uint32_t code_size;
  void *entry_point;
  uint32_t load_address; // Dirección donde se cargó realmente
} exec_info_t;

// Funciones públicas
task_t *exec_load_and_run(const char *path);
void exec_test_program(const char *path);

#endif // EXEC_H