#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

// Información de un módulo cargado
typedef struct {
    uint32_t start;     // Dirección física de inicio
    uint32_t end;       // Dirección física de fin
    uint32_t size;      // Tamaño en bytes
    char* cmdline;      // Línea de comandos del módulo (allocated)
    void* data;         // Puntero a los datos del módulo
} module_info_t;

// Lista de módulos encontrados
extern module_info_t* loaded_modules;
extern uint32_t module_count;

// Funciones principales
bool module_loader_init(struct multiboot_tag* mb_info);
module_info_t* module_find_by_name(const char* name);
module_info_t* module_get_by_index(uint32_t index);
void module_list_all(void);
void module_loader_cleanup(void);

// Funciones de debugging y diagnóstico
void module_debug_multiboot_info(struct multiboot_tag* mb_info);
void module_check_grub_config(void);
bool module_verify_multiboot_magic(uint32_t magic);

// Macros útiles
#define MODULE_EXISTS(name) (module_find_by_name(name) != NULL)
#define MODULE_COUNT() (module_count)
#define MODULE_GET(index) module_get_by_index(index)

#endif