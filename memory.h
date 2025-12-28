// memory.h - REVISADO CON VMM

#ifndef MEMORY_H
#define MEMORY_H

#include "mmu.h"
#include "multiboot2.h"
#include "terminal.h"
#include <stddef.h>
#include <stdint.h>

// ==================== CONSTANTES ====================
#define MAX_MEMORY_REGIONS 32
#define ALIGN8(x) (((x) + 7) & ~7)
#define ALIGN4K(x) (((x) + 0xFFF) & ~0xFFF)
#define MIN_BLOCK_SIZE (sizeof(heap_block_t) + 8)

// Magics para protección del heap
#define HEAP_MAGIC_OCCUPIED 0x48454150 // 'HEAP'
#define HEAP_MAGIC_FREE 0x46454150     // 'FEAP'

// Umbrales para desfragmentación
#define FRAGMENTATION_THRESHOLD 40.0f
#define MIN_DEFRAG_INTERVAL_MS 10000
#define FORCE_DEFRAG_INTERVAL_MS 60000

// ==================== VMM (Virtual Memory Manager) ====================

// Región de memoria virtual
typedef struct vmm_region {
  uint32_t virtual_start;
  uint32_t virtual_end;
  uint32_t physical_start; // 0 si no está respaldada por memoria física
  uint32_t flags;
  struct vmm_region *next;
  struct vmm_region *prev;
} vmm_region_t;

// Espacio de direcciones (para procesos)
typedef struct address_space {
  uint32_t page_directory; // Dirección física del PD
  vmm_region_t *regions;   // Lista de regiones
  uint32_t heap_start;     // Inicio del heap
  uint32_t heap_current;   // Current break (como brk())
  uint32_t stack_start;    // Inicio del stack
  uint32_t stack_size;     // Tamaño del stack
} address_space_t;

// ==================== HEAP ====================

typedef struct heap_block {
  uint32_t magic;
  size_t size;
  uint8_t free;
  struct heap_block *next;
} heap_block_t;

typedef struct {
  size_t used;
  size_t free;
  float fragmentation;
  size_t largest_free_block;
  uint32_t free_blocks_count;
} heap_info_t;

// ==================== TESTING ====================

typedef struct {
  uint32_t total_tests;
  uint32_t passed_tests;
  uint32_t failed_tests;
  char last_error[256];
} heap_test_results_t;

// ==================== DEFRAGMENTATION ====================

typedef struct {
  uint32_t total_defrags;
  uint32_t successful_merges;
  uint32_t last_defrag_time;
  uint32_t largest_block_before;
  uint32_t largest_block_after;
} defrag_stats_t;

// ==================== VARIABLES GLOBALES ====================

// VMM - DECLARACIÓN (la definición está en vmm.c)
extern address_space_t kernel_address_space;

// Heap
extern void *kernel_heap_start;
extern void *kernel_heap_end;
extern heap_block_t *free_list;
extern defrag_stats_t defrag_stats;

// ==================== PROTOTIPOS VMM ====================

void vmm_init(void);
address_space_t *vmm_create_address_space(void);
void vmm_destroy_address_space(address_space_t *as);
bool vmm_map_region(address_space_t *as, uint32_t virt_start, uint32_t size,
                    uint32_t flags);
bool vmm_unmap_region(address_space_t *as, uint32_t virt_start, uint32_t size);
bool vmm_allocate_stack(address_space_t *as, uint32_t size);
bool vmm_allocate_heap(address_space_t *as, uint32_t initial_size);
void *vmm_brk(address_space_t *as, void *addr);
void vmm_switch_address_space(address_space_t *as);
void vmm_debug_info(address_space_t *as, Terminal *term);

// ==================== PROTOTIPOS HEAP ====================

void heap_init(void *heap_memory, size_t heap_size);
void *kernel_malloc(size_t size);
int kernel_free(void *ptr);
void *kernel_realloc(void *ptr, size_t new_size);
void heap_defragment(void);
heap_info_t heap_stats(void);
heap_info_t heap_stats_fast(void);
size_t heap_available(void);
void heap_debug(Terminal *term);

// ==================== TESTING ====================

heap_test_results_t heap_run_exhaustive_tests(void);
void heap_print_test_results(const heap_test_results_t *results,
                             Terminal *term);

// ==================== DEFRAGMENTATION ====================

void memory_defrag_task(void *arg);
void defrag_print_stats(void);
void cmd_defrag_stats(void);
void cmd_force_defrag(void);

#endif