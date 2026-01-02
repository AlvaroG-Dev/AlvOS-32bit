// pmm.c - NUEVO ARCHIVO
#include "pmm.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "memutils.h"
#include "mmu.h"
#include "string.h"
#include "terminal.h"

// ==================== VARIABLES PMM ====================

mem_region_t mem_regions[MAX_MEMORY_REGIONS];
uint32_t mem_region_count = 0;
pmm_bitmap_t pmm_bitmap = {0};

// ==================== FUNCIONES PMM ====================

void pmm_init(struct multiboot_tag_mmap *mmap_tag) {
  if (!mmap_tag) {
    return;
  }

  // Inicializar contador de regiones
  mem_region_count = 0;

  // Recoger regiones de memoria del GRUB
  uint8_t *entry_ptr = (uint8_t *)mmap_tag + sizeof(struct multiboot_tag_mmap);
  uint32_t entry_count = (mmap_tag->size - sizeof(struct multiboot_tag_mmap)) /
                         mmap_tag->entry_size;

  // Contar memoria total disponible
  uint64_t total_memory = 0;

  for (uint32_t i = 0; i < entry_count && mem_region_count < MAX_MEMORY_REGIONS;
       i++) {
    struct multiboot_mmap_entry *entry =
        (struct multiboot_mmap_entry *)entry_ptr;

    // Solo considerar memoria disponible (tipo 1)
    if (entry->type == 1) {
      mem_regions[mem_region_count].base = ALIGN_4KB_UP(entry->addr);
      mem_regions[mem_region_count].length = ALIGN_4KB_DOWN(entry->len);
      mem_regions[mem_region_count].used = 0;

      // Verificar que la región sea válida
      if (mem_regions[mem_region_count].length >= PAGE_SIZE) {
        total_memory += mem_regions[mem_region_count].length;
        mem_region_count++;
      }
    }
    entry_ptr += mmap_tag->entry_size;
  }

  // Ordenar regiones por dirección base
  for (uint32_t i = 0; i < mem_region_count; i++) {
    for (uint32_t j = i + 1; j < mem_region_count; j++) {
      if (mem_regions[i].base > mem_regions[j].base) {
        mem_region_t tmp = mem_regions[i];
        mem_regions[i] = mem_regions[j];
        mem_regions[j] = tmp;
      }
    }
  }

  // Fusionar regiones contiguas
  uint32_t merged_count = mem_region_count;
  for (uint32_t i = 0; i < merged_count - 1; i++) {
    uint64_t current_end = mem_regions[i].base + mem_regions[i].length;
    uint64_t next_base = mem_regions[i + 1].base;

    if (current_end >= next_base) {
      // Regiones solapadas o contiguas, fusionar
      uint64_t next_end = mem_regions[i + 1].base + mem_regions[i + 1].length;
      mem_regions[i].length = (next_end > current_end)
                                  ? (next_end - mem_regions[i].base)
                                  : (current_end - mem_regions[i].base);

      // Eliminar región fusionada
      for (uint32_t j = i + 1; j < merged_count - 1; j++) {
        mem_regions[j] = mem_regions[j + 1];
      }
      merged_count--;
      i--; // Revisar nuevamente esta posición
    }
  }
  mem_region_count = merged_count;

  // Calcular páginas totales para bitmap
  uint32_t total_pages = 0;
  for (uint32_t i = 0; i < mem_region_count; i++) {
    total_pages += mem_regions[i].length / PAGE_SIZE;
  }

  // Encontrar espacio para el bitmap (usar final de la primera región grande)
  uint32_t bitmap_pages_needed = (total_pages + 31) / 32; // 1 bit por página
  bitmap_pages_needed = ALIGN_4KB_UP(bitmap_pages_needed * 4) / PAGE_SIZE;

  for (uint32_t i = 0; i < mem_region_count; i++) {
    if (mem_regions[i].length >= bitmap_pages_needed * PAGE_SIZE * 2) {
      uint32_t bitmap_addr = mem_regions[i].base + mem_regions[i].length -
                             (bitmap_pages_needed * PAGE_SIZE);

      // Alinear a página
      bitmap_addr = ALIGN_4KB_UP(bitmap_addr);

      // Inicializar bitmap
      pmm_bitmap.bitmap = (uint32_t *)bitmap_addr;
      pmm_bitmap.total_pages = total_pages;
      pmm_bitmap.free_pages = total_pages;
      pmm_bitmap.bitmap_size = bitmap_pages_needed * PAGE_SIZE;

      // Limpiar bitmap (todas las páginas libres inicialmente)
      memset(pmm_bitmap.bitmap, 0xFF, pmm_bitmap.bitmap_size);

      // Marcar páginas usadas por el bitmap como ocupadas
      uint32_t bitmap_start_page =
          (bitmap_addr - mem_regions[i].base) / PAGE_SIZE;
      for (uint32_t j = 0; j < bitmap_pages_needed; j++) {
        uint32_t page_idx = bitmap_start_page + j;
        pmm_bitmap.bitmap[page_idx / 32] &= ~(1 << (page_idx % 32));
        pmm_bitmap.free_pages--;
      }

      // Reducir tamaño de la región
      mem_regions[i].length -= bitmap_pages_needed * PAGE_SIZE;
      return;
    }
  }
}

void pmm_exclude_kernel_heap(void *heap_start, size_t heap_size) {
  uint64_t heap_begin = ALIGN_4KB_DOWN((uintptr_t)heap_start);
  uint64_t heap_end = ALIGN_4KB_UP(heap_begin + heap_size);

  for (uint32_t i = 0; i < mem_region_count; i++) {
    uint64_t region_start = mem_regions[i].base;
    uint64_t region_end = mem_regions[i].base + mem_regions[i].length;

    if (heap_end <= region_start || heap_begin >= region_end) {
      continue; // No overlap
    }

    if (heap_begin > region_start && heap_end < region_end) {
      // Heap en medio de la región
      uint64_t region1_length = heap_begin - region_start;
      uint64_t region2_base = heap_end;
      uint64_t region2_length = region_end - heap_end;

      // Actualizar primera región
      mem_regions[i].length = region1_length;

      // Insertar segunda región si hay espacio
      if (region2_length > 0 && mem_region_count < MAX_MEMORY_REGIONS - 1) {
        for (uint32_t j = mem_region_count; j > i + 1; j--) {
          mem_regions[j] = mem_regions[j - 1];
        }
        mem_regions[i + 1].base = region2_base;
        mem_regions[i + 1].length = region2_length;
        mem_regions[i + 1].used = 0;
        mem_region_count++;
      }
      i++; // Saltar la región que acabamos de insertar
    } else if (heap_begin <= region_start && heap_end < region_end) {
      // Heap comienza antes pero termina dentro
      mem_regions[i].base = heap_end;
      mem_regions[i].length = region_end - heap_end;
    } else if (heap_begin > region_start && heap_end >= region_end) {
      // Heap comienza dentro pero termina después
      mem_regions[i].length = heap_begin - region_start;
    } else {
      // Heap cubre completamente la región
      for (uint32_t j = i; j < mem_region_count - 1; j++) {
        mem_regions[j] = mem_regions[j + 1];
      }
      mem_region_count--;
      i--; // Revisar la misma posición
    }
  }
}

void *pmm_alloc_page(void) {
  // Buscar primera página libre en el bitmap
  for (uint32_t i = 0; i < (pmm_bitmap.total_pages + 31) / 32; i++) {
    if (pmm_bitmap.bitmap[i] != 0) {
      // Hay al menos una página libre en este word
      for (uint32_t j = 0; j < 32; j++) {
        if (pmm_bitmap.bitmap[i] & (1 << j)) {
          uint32_t page_idx = i * 32 + j;

          // Verificar que la página esté en una región válida
          uint64_t page_addr = 0;
          uint64_t current_base = 0;

          for (uint32_t r = 0; r < mem_region_count; r++) {
            uint32_t region_pages = mem_regions[r].length / PAGE_SIZE;
            if (page_idx >= current_base / PAGE_SIZE &&
                page_idx < (current_base / PAGE_SIZE) + region_pages) {
              page_addr = mem_regions[r].base +
                          (page_idx - current_base / PAGE_SIZE) * PAGE_SIZE;
              break;
            }
            current_base += mem_regions[r].length;
          }

          if (page_addr == 0) {
            continue; // Página no está en región válida
          }

          // Marcar como usada
          pmm_bitmap.bitmap[i] &= ~(1 << j);
          pmm_bitmap.free_pages--;

          return (void *)(uintptr_t)page_addr;
        }
      }
    }
  }

  return NULL; // No hay memoria disponible
}

void *pmm_alloc_pages(uint32_t count) {
  if (count == 0)
    return NULL;

  // Búsqueda simple de bloques contiguos
  uint32_t consecutive = 0;
  uint32_t start_idx = 0;

  for (uint32_t i = 0; i < pmm_bitmap.total_pages; i++) {
    uint32_t word_idx = i / 32;
    uint32_t bit_idx = i % 32;

    if (pmm_bitmap.bitmap[word_idx] & (1 << bit_idx)) {
      consecutive++;
      if (consecutive == 1)
        start_idx = i;
      if (consecutive == count) {
        // Encontramos bloque contiguo
        uint64_t page_addr = 0;
        uint64_t current_base = 0;

        for (uint32_t r = 0; r < mem_region_count; r++) {
          uint32_t region_pages = mem_regions[r].length / PAGE_SIZE;
          if (start_idx >= current_base / PAGE_SIZE &&
              start_idx < (current_base / PAGE_SIZE) + region_pages) {
            page_addr = mem_regions[r].base +
                        (start_idx - current_base / PAGE_SIZE) * PAGE_SIZE;
            break;
          }
          current_base += mem_regions[r].length;
        }

        if (page_addr == 0) {
          consecutive = 0;
          continue;
        }

        // Marcar páginas como usadas
        for (uint32_t j = 0; j < count; j++) {
          uint32_t page_num = start_idx + j;
          uint32_t w_idx = page_num / 32;
          uint32_t b_idx = page_num % 32;
          pmm_bitmap.bitmap[w_idx] &= ~(1 << b_idx);
        }

        pmm_bitmap.free_pages -= count;
        return (void *)(uintptr_t)page_addr;
      }
    } else {
      consecutive = 0;
    }
  }

  return NULL; // No hay bloque contiguo del tamaño solicitado
}

void pmm_free_page(void *page) {
  uint64_t page_addr = (uint64_t)(uintptr_t)page;

  if (page_addr % PAGE_SIZE != 0) {
    return; // Dirección no alineada
  }

  // Encontrar índice de la página
  uint64_t current_base = 0;
  uint32_t page_idx = 0;

  for (uint32_t r = 0; r < mem_region_count; r++) {
    if (page_addr >= mem_regions[r].base &&
        page_addr < mem_regions[r].base + mem_regions[r].length) {
      page_idx = (page_addr - mem_regions[r].base) / PAGE_SIZE +
                 current_base / PAGE_SIZE;
      break;
    }
    current_base += mem_regions[r].length;
  }

  if (page_idx >= pmm_bitmap.total_pages) {
    return; // Página fuera de rango
  }

  // Marcar como libre
  uint32_t word_idx = page_idx / 32;
  uint32_t bit_idx = page_idx % 32;

  // Verificar que no esté ya libre
  if (!(pmm_bitmap.bitmap[word_idx] & (1 << bit_idx))) {
    pmm_bitmap.bitmap[word_idx] |= (1 << bit_idx);
    pmm_bitmap.free_pages++;
  }
}

void pmm_free_pages(void *base, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    pmm_free_page((void *)((uintptr_t)base + i * PAGE_SIZE));
  }
}

uint32_t pmm_get_free_pages(void) { return pmm_bitmap.free_pages; }

uint32_t pmm_get_total_pages(void) { return pmm_bitmap.total_pages; }

void pmm_debug_info(Terminal *term) {
  char msg[256];

  terminal_puts(term, "\r\n=== Physical Memory Manager ===\r\n");

  snprintf(msg, sizeof(msg), "Total pages: %u (%u MB)\r\n",
           pmm_get_total_pages(),
           (pmm_get_total_pages() * PAGE_SIZE) / (1024 * 1024));
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "Free pages: %u (%u MB)\r\n", pmm_get_free_pages(),
           (pmm_get_free_pages() * PAGE_SIZE) / (1024 * 1024));
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "Used pages: %u (%u MB)\r\n",
           pmm_get_total_pages() - pmm_get_free_pages(),
           ((pmm_get_total_pages() - pmm_get_free_pages()) * PAGE_SIZE) /
               (1024 * 1024));
  terminal_puts(term, msg);

  terminal_puts(term, "\r\nMemory regions:\r\n");
  for (uint32_t i = 0; i < mem_region_count; i++) {
    snprintf(msg, sizeof(msg), "  Region %u: 0x%08x-0x%08x (%u KB) %s\r\n", i,
             (uint32_t)mem_regions[i].base,
             (uint32_t)(mem_regions[i].base + mem_regions[i].length),
             (uint32_t)(mem_regions[i].length / 1024),
             mem_regions[i].used ? "[USED]" : "[FREE]");
    terminal_puts(term, msg);
  }
}