// pmm.h - NUEVO ARCHIVO
#ifndef PMM_H
#define PMM_H

#include "multiboot2.h"
#include "terminal.h"
#include <stddef.h>
#include <stdint.h>

// Constantes
#define MAX_MEMORY_REGIONS 32

// Estructuras
typedef struct {
  uint64_t base;
  uint64_t length;
  uint8_t used;
} mem_region_t;

typedef struct {
  uint32_t *bitmap;
  uint32_t total_pages;
  uint32_t free_pages;
  uint32_t bitmap_size;
} pmm_bitmap_t;

// Variables globales
extern mem_region_t mem_regions[MAX_MEMORY_REGIONS];
extern uint32_t mem_region_count;
extern pmm_bitmap_t pmm_bitmap;

// Prototipos de funciones
void pmm_init(struct multiboot_tag_mmap *mmap_tag);
void pmm_exclude_kernel_heap(void *heap_start, size_t heap_size);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t count);
void pmm_free_page(void *page);
void pmm_free_pages(void *base, uint32_t count);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_total_pages(void);
void pmm_debug_info(Terminal *term);

#endif