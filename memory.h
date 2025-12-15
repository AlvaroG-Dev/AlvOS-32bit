#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "multiboot2.h"
#include "mmu.h"
#include "terminal.h"


// Constantes
#define MAX_MEMORY_REGIONS 32
#define HEAP_MAGIC         0x48454150  // 'HEAP' en ASCII
#define ALIGN8(x)          (((x) + 7) & ~7)
#define ALIGN4K(x)         (((x) + 0xFFF) & ~0xFFF)
#define MIN_BLOCK_SIZE     (sizeof(heap_block_t) + 8)

// Umbrales para desfragmentación
#define FRAGMENTATION_THRESHOLD     40.0f   // % de fragmentación
#define MIN_DEFRAG_INTERVAL_MS      10000   // 10 segundos mínimo entre defrags
#define FORCE_DEFRAG_INTERVAL_MS    60000   // Forzar cada 60 segundos

// Definición de región de memoria
typedef struct {
    uint64_t base;
    uint64_t length;
} mem_region_t;

// En memory.h
typedef struct {
    uint32_t total_tests;
    uint32_t passed_tests;
    uint32_t failed_tests;
    char last_error[256];
} heap_test_results_t;

heap_test_results_t heap_run_exhaustive_tests(void);

// Estructura del bloque de heap
typedef struct heap_block {
    uint32_t magic;
    size_t size;
    uint8_t free;
    struct heap_block* next;
} heap_block_t;

typedef struct {
    size_t used;
    size_t free;
    float fragmentation;       // Porcentaje de fragmentación
    size_t largest_free_block; // Tamaño del bloque libre más grande
    uint32_t free_blocks_count;// Número de bloques libres
} heap_info_t;

// Estadísticas
typedef struct {
    uint32_t total_defrags;
    uint32_t successful_merges;
    uint32_t last_defrag_time;
    uint32_t largest_block_before;
    uint32_t largest_block_after;
} defrag_stats_t;

// Variables globales
extern mem_region_t mem_regions[MAX_MEMORY_REGIONS];
extern uint32_t mem_region_count;
extern void* heap_start;
extern void* heap_end;
extern void* heap_base;
extern heap_block_t* free_list;
extern void* kernel_heap_start;
extern void* kernel_heap_end;
extern defrag_stats_t defrag_stats;

heap_info_t heap_stats(void);
heap_info_t heap_stats_fast(void);

// Prototipos de funciones
void heap_init(void* heap_memory, size_t heap_size);
void* kernel_malloc(size_t size);
int kernel_free(void* ptr);
void* kernel_realloc(void* ptr, size_t new_size);
size_t heap_available(void);
void pmem_init(struct multiboot_tag_mmap *mmap_tag);
void exclude_kernel_heap_from_regions(void* heap_start, size_t heap_size);
uint32_t mmu_map_heap_region(uint32_t virtual_start, uint32_t size);

// Funciones de depuración
void debug_print_memory_map(uint32_t *screen, uint32_t pitch);
void debug_print_heap_info(uint32_t *screen, uint32_t pitch);
void heap_print_test_results(const heap_test_results_t* results, Terminal* term);
void debug_heap();
void heap_defragment(void);

// Función principal de la tarea (no llamar directamente)
void memory_defrag_task(void* arg);
// Forzar defragmentación manual (ya definida en memory.h)
void heap_defragment(void);
// Imprimir estadísticas de defragmentación
void defrag_print_stats(void);

void cmd_defrag_stats(void);
void cmd_force_defrag(void);

#endif