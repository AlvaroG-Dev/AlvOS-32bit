#include "memory.h"
#include "drawing.h"
#include "irq.h"
#include "kernel.h"
#include "log.h"
#include "memutils.h"
#include "mmu.h"
#include "string.h"
#include "task.h"
#include "task_utils.h"

// ==================== VARIABLES HEAP ====================

void *kernel_heap_start = NULL;
void *kernel_heap_end = NULL;
heap_block_t *free_list = NULL;
defrag_stats_t defrag_stats = {0};

// ==================== FUNCIONES HEAP ====================

void heap_init(void *heap_memory, size_t heap_size) {
  // Verificar tamaño mínimo del heap
  if (heap_size < MIN_BLOCK_SIZE + sizeof(heap_block_t)) {
    __asm__ volatile("hlt");
  }

  // Alinear el heap a 4KB
  uint32_t aligned_start = ALIGN_4KB_UP((uint32_t)heap_memory);
  heap_size = ALIGN_4KB_DOWN(heap_size);

  // Inicializar estructuras del heap
  kernel_heap_start = (void *)aligned_start;
  kernel_heap_end = (void *)(aligned_start + heap_size);

  // Configurar el primer bloque libre
  free_list = (heap_block_t *)kernel_heap_start;
  free_list->magic = HEAP_MAGIC_FREE;
  free_list->size = heap_size - sizeof(heap_block_t);
  free_list->free = 1;
  free_list->next = NULL;
}

void *kernel_malloc(size_t size) {
  // Deshabilitar interrupciones
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  if (size == 0 || !kernel_heap_start) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return NULL;
  }

  // Alinear a 16 bytes
  size = (size + 15) & ~15;
  size_t total_size = size + sizeof(heap_block_t);

  // Buscar bloque libre (best-fit para allocaciones grandes)
  heap_block_t *prev = NULL;
  heap_block_t *current = free_list;
  heap_block_t *best_fit = NULL;
  heap_block_t *best_prev = NULL;
  size_t best_fit_size = (size_t)-1;

  bool use_best_fit = (size > 4096);

  while (current) {
    if (current->magic != HEAP_MAGIC_FREE) {
      // Corrupción detectada
      __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
      return NULL;
    }

    if (current->free && current->size >= size) {
      if (use_best_fit) {
        // Best-fit
        if (current->size < best_fit_size) {
          best_fit = current;
          best_prev = prev;
          best_fit_size = current->size;
        }
      } else {
        // First-fit
        best_fit = current;
        best_prev = prev;
        break;
      }
    }
    prev = current;
    current = current->next;
  }

  if (!best_fit) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return NULL;
  }

  current = best_fit;
  prev = best_prev;

  // Dividir bloque si hay suficiente espacio
  if (current->size >= total_size + MIN_BLOCK_SIZE) {
    heap_block_t *new_block = (heap_block_t *)((uint8_t *)current + total_size);
    new_block->magic = HEAP_MAGIC_FREE;
    new_block->size = current->size - total_size;
    new_block->free = 1;
    new_block->next = current->next;

    current->size = size;
    current->next = new_block;
  }

  // Marcar como ocupado
  current->free = 0;
  current->magic = HEAP_MAGIC_OCCUPIED;

  // Remover de free_list
  if (prev) {
    prev->next = current->next;
  } else {
    free_list = current->next;
  }

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  // Limpiar memoria para allocaciones grandes
  void *ptr = (void *)((uint8_t *)current + sizeof(heap_block_t));
  if (size >= 1024) {
    memset(ptr, 0, size);
  }

  return ptr;
}

int kernel_free(void *ptr) {
  if (!ptr || ptr < kernel_heap_start || ptr >= kernel_heap_end) {
    return 0;
  }

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));

  // Validaciones estrictas
  if (((uintptr_t)ptr % 16 != 0) ||
      ((uint8_t *)block < (uint8_t *)kernel_heap_start) ||
      ((uint8_t *)block + sizeof(heap_block_t) + block->size >
       (uint8_t *)kernel_heap_end)) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  if (block->magic != HEAP_MAGIC_OCCUPIED) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  block->free = 1;
  block->magic = HEAP_MAGIC_FREE;

  // Reinsertar en free_list ordenadamente
  if (!free_list || block < free_list) {
    block->next = free_list;
    free_list = block;
  } else {
    heap_block_t *current = free_list;
    while (current->next && current->next < block) {
      current = current->next;
    }
    block->next = current->next;
    current->next = block;
  }

  // Coalescencia
  heap_block_t *tmp = free_list;
  while (tmp && tmp->next) {
    uint8_t *tmp_end = (uint8_t *)tmp + sizeof(heap_block_t) + tmp->size;
    uint8_t *next_start = (uint8_t *)tmp->next;

    if (tmp->free && tmp->next->free && tmp_end == next_start) {
      // Fusionar bloques
      tmp->size += sizeof(heap_block_t) + tmp->next->size;
      tmp->next = tmp->next->next;
    } else {
      tmp = tmp->next;
    }
  }

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
  return 1;
}

void *kernel_realloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kernel_malloc(new_size);
  if (new_size == 0) {
    kernel_free(ptr);
    return NULL;
  }

  heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
  if (block->magic != HEAP_MAGIC_OCCUPIED) {
    return NULL;
  }

  new_size = ALIGN8(new_size);

  if (new_size == block->size) {
    return ptr;
  } else if (new_size < block->size) {
    // Shrink: Intentar reducir si el ahorro es significativo
    size_t shrink_amount = block->size - new_size;
    if (shrink_amount >= MIN_BLOCK_SIZE + sizeof(heap_block_t)) {
      // Crear un nuevo bloque libre con el espacio sobrante
      heap_block_t *new_free_block =
          (heap_block_t *)((uint8_t *)ptr + new_size);
      new_free_block->magic = HEAP_MAGIC_OCCUPIED; // Temporalmente para free()
      new_free_block->size = shrink_amount - sizeof(heap_block_t);
      new_free_block->free = 0;

      block->size = new_size;
      kernel_free((void *)((uint8_t *)new_free_block + sizeof(heap_block_t)));
    }
    return ptr;
  } else {
    // Grow: Siempre asignar nuevo y copiar para evitar corrupción
    // El código anterior usaba block->next de forma incorrecta (era free_list
    // next)
    void *new_ptr = kernel_malloc(new_size);
    if (!new_ptr)
      return NULL;

    memcpy(new_ptr, ptr, block->size);
    kernel_free(ptr);
    return new_ptr;
  }
}

// ==================== FUNCIONES AUXILIARES HEAP ====================

size_t heap_available(void) {
  size_t available = 0;
  heap_block_t *current = free_list;
  while (current) {
    if (current->free && current->magic == HEAP_MAGIC_FREE) {
      available += current->size;
    }
    current = current->next;
  }
  return available;
}

heap_info_t heap_stats(void) {
  heap_info_t info = {0};
  uint8_t *heap_ptr = (uint8_t *)kernel_heap_start;
  size_t largest_free = 0;
  uint32_t free_blocks = 0;

  while (heap_ptr < (uint8_t *)kernel_heap_end) {
    heap_block_t *block = (heap_block_t *)heap_ptr;

    if (block->magic != HEAP_MAGIC_OCCUPIED &&
        block->magic != HEAP_MAGIC_FREE) {
      break; // Corrupción
    }

    if (block->free) {
      info.free += block->size;
      free_blocks++;
      if (block->size > largest_free) {
        largest_free = block->size;
      }
    } else {
      info.used += block->size + sizeof(heap_block_t);
    }

    heap_ptr += sizeof(heap_block_t) + block->size;
  }

  info.free_blocks_count = free_blocks;
  info.largest_free_block = largest_free;
  info.fragmentation =
      (free_blocks > 1) ? (100.0f - (100.0f * largest_free) / info.free) : 0.0f;

  return info;
}

heap_info_t heap_stats_fast(void) {
  heap_info_t info = {0};

  heap_block_t *current = free_list;
  while (current) {
    if (current->free && current->magic == HEAP_MAGIC_FREE) {
      info.free += current->size;
      info.free_blocks_count++;
      if (current->size > info.largest_free_block) {
        info.largest_free_block = current->size;
      }
    }
    current = current->next;
  }

  size_t total_heap = (uint8_t *)kernel_heap_end - (uint8_t *)kernel_heap_start;
  info.used = total_heap - info.free;

  info.fragmentation =
      (info.free_blocks_count > 1 && info.free > 0)
          ? (100.0f - (100.0f * info.largest_free_block) / info.free)
          : 0.0f;

  return info;
}

// ==================== DEFRAGMENTACIÓN ====================

void heap_defragment(void) {
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  uint32_t start_time = ticks_since_boot;
  uint32_t merged_count = 0;
  uint32_t passes = 0;
  bool merged_this_pass = true;

  heap_info_t before = heap_stats_fast();

  while (merged_this_pass && passes < 10) {
    merged_this_pass = false;
    passes++;

    heap_block_t *current = free_list;
    while (current && current->next) {
      if (current->free && current->magic == HEAP_MAGIC_FREE &&
          current->next->free && current->next->magic == HEAP_MAGIC_FREE) {

        uint8_t *current_end =
            (uint8_t *)current + sizeof(heap_block_t) + current->size;
        uint8_t *next_start = (uint8_t *)current->next;

        if (current_end == next_start) {
          heap_block_t *next_block = current->next;
          current->size += sizeof(heap_block_t) + next_block->size;
          current->next = next_block->next;
          merged_count++;
          merged_this_pass = true;
          continue;
        }
      }
      current = current->next;
    }
  }

  heap_info_t after = heap_stats_fast();

  defrag_stats.total_defrags++;
  defrag_stats.successful_merges += merged_count;
  defrag_stats.last_defrag_time = ticks_since_boot;
  defrag_stats.largest_block_before = before.largest_free_block;
  defrag_stats.largest_block_after = after.largest_free_block;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  if (merged_count > 0) {
    log_message(LOG_INFO, "[DEFRAG] %u blocks merged in %u passes",
                merged_count, passes);
  }
}

// ==================== DEPURACIÓN ====================

void heap_debug(Terminal *term) {
  char msg[256];

  terminal_puts(term, "\r\n=== Heap Debug ===\r\n");

  snprintf(msg, sizeof(msg), "Heap range: 0x%08x - 0x%08x\r\n",
           (uint32_t)kernel_heap_start, (uint32_t)kernel_heap_end);
  terminal_puts(term, msg);

  heap_info_t stats = heap_stats();
  snprintf(msg, sizeof(msg), "Used: %u bytes\r\n", stats.used);
  terminal_puts(term, msg);
  snprintf(msg, sizeof(msg), "Free: %u bytes\r\n", stats.free);
  terminal_puts(term, msg);
  snprintf(msg, sizeof(msg), "Largest free block: %u bytes\r\n",
           stats.largest_free_block);
  terminal_puts(term, msg);
  snprintf(msg, sizeof(msg), "Free blocks: %u\r\n", stats.free_blocks_count);
  terminal_puts(term, msg);
  snprintf(msg, sizeof(msg), "Fragmentation: %.2f%%\r\n", stats.fragmentation);
  terminal_puts(term, msg);

  terminal_puts(term, "\r\nFree list:\r\n");
  heap_block_t *current = free_list;
  uint32_t block_num = 0;
  while (current) {
    snprintf(msg, sizeof(msg), "  Block %u: 0x%08x, size: %u, free: %u\r\n",
             block_num++, (uint32_t)current, current->size, current->free);
    terminal_puts(term, msg);
    current = current->next;
  }
}

void debug_heap() {
  terminal_puts(&main_terminal, "Heap debug:\r\n");

  char msg[128];
  snprintf(msg, sizeof(msg), "Heap start: 0x%x, end: 0x%x, size: %u bytes\r\n",
           (uint32_t)kernel_heap_start, (uint32_t)kernel_heap_end,
           (uint32_t)kernel_heap_end - (uint32_t)kernel_heap_start);
  terminal_puts(&main_terminal, msg);

  heap_block_t *current = free_list;
  while (current) {
    snprintf(msg, sizeof(msg), "Block: 0x%x, size: %u, free: %u\r\n",
             (uint32_t)current, current->size, current->free);
    terminal_puts(&main_terminal, msg);
    current = current->next;
  }
}

heap_test_results_t heap_run_exhaustive_tests(void) {
  heap_test_results_t results = {0};
  const size_t test_sizes[] = {16,  32,   64,   128,  256,
                               512, 1024, 2048, 4096, 8192};
  const size_t num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
  void *pointers[10] = {0};
  char test_msg[256];

  // 1. Test: Verificar integridad básica del primer bloque
  results.total_tests++;
  heap_block_t *current_block = (heap_block_t *)kernel_heap_start;

  if (current_block->magic != HEAP_MAGIC_FREE &&
      current_block->magic != HEAP_MAGIC_OCCUPIED) {
    snprintf(results.last_error, sizeof(results.last_error),
             "Heap init failed: Invalid magic number 0x%x",
             current_block->magic);
    results.failed_tests++;
  } else {
    results.passed_tests++;
  }

  // 2. Test: Asignaciones básicas
  for (size_t i = 0; i < num_tests; i++) {
    results.total_tests++;
    pointers[i] = kernel_malloc(test_sizes[i]);
    if (!pointers[i]) {
      snprintf(results.last_error, sizeof(results.last_error),
               "Malloc failed for size %u (test %u/%u)", test_sizes[i], i + 1,
               num_tests);
      results.failed_tests++;
      continue;
    }

    // Verificar que el bloque está marcado como ocupado
    heap_block_t *block =
        (heap_block_t *)((uint8_t *)pointers[i] - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC_OCCUPIED || block->free) {
      snprintf(results.last_error, sizeof(results.last_error),
               "Block corruption after malloc: magic=0x%x, free=%u (size %u)",
               block->magic, block->free, test_sizes[i]);
      results.failed_tests++;
    } else {
      results.passed_tests++;
    }

    // Escribir en la memoria asignada
    memset(pointers[i], 0xAA, test_sizes[i]);
  }

  // 3. Test: Verificar fragmentación y lista libre
  results.total_tests++;
  size_t free_before = heap_available();
  if (pointers[0]) {
    kernel_free(pointers[0]);
    size_t free_after = heap_available();

    if (free_after <= free_before) {
      snprintf(results.last_error, sizeof(results.last_error),
               "Free failed: free before=%u, after=%u (expected increase)",
               free_before, free_after);
      results.failed_tests++;
    } else {
      results.passed_tests++;
      pointers[0] = NULL;
    }
  } else {
    snprintf(results.last_error, sizeof(results.last_error),
             "Skipped free test due to previous malloc failure");
    results.failed_tests++;
  }

  // 4. Test: Reasignación en espacio liberado
  results.total_tests++;
  void *new_ptr = kernel_malloc(test_sizes[0]);
  if (!new_ptr) {
    snprintf(results.last_error, sizeof(results.last_error),
             "Realloc failed in freed space (size %u)", test_sizes[0]);
    results.failed_tests++;
  } else {
    // Verificar que reutilizó el espacio liberado
    heap_block_t *new_block =
        (heap_block_t *)((uint8_t *)new_ptr - sizeof(heap_block_t));
    if (new_block->size < test_sizes[0]) {
      snprintf(results.last_error, sizeof(results.last_error),
               "Realloc size mismatch: got %u, expected >=%u", new_block->size,
               test_sizes[0]);
      results.failed_tests++;
    } else {
      results.passed_tests++;
    }
    kernel_free(new_ptr);
  }

  // 5. Test: Realloc
  results.total_tests++;
  if (pointers[1]) {
    void *realloc_ptr = kernel_realloc(pointers[1], test_sizes[1] * 2);
    if (!realloc_ptr) {
      snprintf(results.last_error, sizeof(results.last_error),
               "Realloc failed to grow from %u to %u", test_sizes[1],
               test_sizes[1] * 2);
      results.failed_tests++;
    } else {
      pointers[1] = realloc_ptr;
      results.passed_tests++;
    }
  } else {
    snprintf(results.last_error, sizeof(results.last_error),
             "Skipped realloc test due to previous malloc failure");
    results.failed_tests++;
  }

  // 6. Test: Asignación máxima posible
  results.total_tests++;
  size_t max_size = heap_available();
  size_t attempt_size = max_size;

  // Intentar con tamaños decrecientes si falla
  while (attempt_size > sizeof(heap_block_t)) {
    void *max_ptr = kernel_malloc(attempt_size - sizeof(heap_block_t));
    if (max_ptr) {
      // Verificar que realmente obtuvimos el tamaño solicitado
      heap_block_t *block =
          (heap_block_t *)((uint8_t *)max_ptr - sizeof(heap_block_t));
      if (block->size >= (attempt_size - sizeof(heap_block_t))) {
        results.passed_tests++;
        kernel_free(max_ptr);
        break;
      } else {
        kernel_free(max_ptr);
        attempt_size = block->size; // Intentar con el tamaño que sí asignó
      }
    } else {
      // Reducir el tamaño en 4KB y reintentar
      attempt_size = (attempt_size > PAGE_SIZE) ? attempt_size - PAGE_SIZE
                                                : attempt_size / 2;
    }
  }

  if (attempt_size <= sizeof(heap_block_t)) {
    snprintf(
        results.last_error, sizeof(results.last_error),
        "Failed to allocate any significant size (max tried %u, available %u)",
        max_size - sizeof(heap_block_t), max_size);
    results.failed_tests++;
  }

  // 7. Test: Verificar coherencia después de todas las operaciones
  results.total_tests++;
  int corrupt = 0;
  current_block = (heap_block_t *)
      kernel_heap_start; // Reutilizamos la variable ya declarada

  while ((uint8_t *)current_block < (uint8_t *)kernel_heap_end) {
    if (current_block->magic != HEAP_MAGIC_FREE &&
        current_block->magic != HEAP_MAGIC_OCCUPIED) {
      corrupt = 1;
      break;
    }
    current_block =
        (heap_block_t *)((uint8_t *)current_block + sizeof(heap_block_t) +
                         current_block->size);
  }

  if (corrupt) {
    snprintf(results.last_error, sizeof(results.last_error),
             "Heap corruption detected after all tests");
    results.failed_tests++;
  } else {
    results.passed_tests++;
  }

  // Limpiar todos los punteros
  for (size_t i = 0; i < num_tests; i++) {
    if (pointers[i]) {
      kernel_free(pointers[i]);
    }
  }

  return results;
}

void heap_print_test_results(const heap_test_results_t *results,
                             Terminal *term) {
  char msg[256];
  heap_info_t info = heap_stats(); // Solo una declaración

  snprintf(msg, sizeof(msg), "Heap Test Results:\r\n");
  terminal_puts(term, msg);

  // snprintf(msg, sizeof(msg), "  Total tests: %u\r\n", results->total_tests);
  // terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "  Passed: %u\r\n", results->passed_tests);
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "  Failed: %u\r\n", results->failed_tests);
  terminal_puts(term, msg);

  // if (results->failed_tests > 0) {
  //     snprintf(msg, sizeof(msg), "  Last error: %s\r\n",
  //     results->last_error); terminal_puts(term, msg);
  // }

  // Mostrar estadísticas extendidas del heap
  snprintf(msg, sizeof(msg), "Detailed Heap Stats:\r\n");
  terminal_puts(term, msg);

  // snprintf(msg, sizeof(msg), "  Total Used: %u bytes (including
  // metadata)\r\n", info.used); terminal_puts(term, msg);
  //
  // snprintf(msg, sizeof(msg), "  Total Free: %u bytes\r\n", info.free);
  // terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "  Largest Free Block: %u bytes\r\n",
           info.largest_free_block);
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "  Free Blocks Count: %u\r\n",
           info.free_blocks_count);
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "  Fragmentation: %.2f%%\r\n", info.fragmentation);
  terminal_puts(term, msg);

  // snprintf(msg, sizeof(msg), "  Theoretical Max Allocation: %u bytes\r\n",
  //         info.largest_free_block > sizeof(heap_block_t) ?
  //         info.largest_free_block - sizeof(heap_block_t) : 0);
  // terminal_puts(term, msg);
}

// ========================================================================
// FUNCIÓN AUXILIAR: Verificar si necesita defragmentación
// ========================================================================

static bool needs_defragmentation(heap_info_t *info) {
  // Criterio 1: Fragmentación alta
  if (info->fragmentation > FRAGMENTATION_THRESHOLD) {
    return true;
  }

  // Criterio 2: Muchos bloques pequeños
  if (info->free_blocks_count > 20) {
    return true;
  }

  // Criterio 3: Bloque más grande es menos de la mitad del espacio libre
  if (info->free > 0 && info->largest_free_block < (info->free / 2)) {
    return true;
  }

  return false;
}

// ========================================================================
// TAREA DE DEFRAGMENTACIÓN PERIÓDICA
// ========================================================================

void memory_defrag_task(void *arg) {
  (void)arg;

  uint32_t last_defrag = 0;
  uint32_t check_count = 0;

  log_message(LOG_INFO, "[DEFRAG] Task started\r\n");
  log_message(LOG_INFO,
              "[DEFRAG] Thresholds: %.1f%% fragmentation, %u sec interval\r\n",
              FRAGMENTATION_THRESHOLD, MIN_DEFRAG_INTERVAL_MS / 1000);

  while (1) {
    // Dormir 5 segundos entre chequeos
    task_sleep(5000);
    check_count++;

    // Obtener estadísticas rápidas del heap
    heap_info_t info = heap_stats_fast();

    uint32_t time_since_last = ticks_since_boot - last_defrag;
    bool should_defrag = false;
    const char *reason = "";

    // Verificar si necesita defragmentación
    if (needs_defragmentation(&info) &&
        time_since_last > (MIN_DEFRAG_INTERVAL_MS / 10)) {
      should_defrag = true;
      reason = "high fragmentation";
    }

    // Forzar defragmentación periódica cada minuto
    if (time_since_last > (FORCE_DEFRAG_INTERVAL_MS / 10)) {
      should_defrag = true;
      reason = "periodic maintenance";
    }

    if (should_defrag) {
      log_message(LOG_INFO, "[DEFRAG] Starting defragmentation: %s\r\n",
                  reason);
      log_message(LOG_INFO,
                  "[DEFRAG] Current: %.2f%% fragmentation, %u free blocks, "
                  "largest: %u bytes\r\n",
                  info.fragmentation, info.free_blocks_count,
                  info.largest_free_block);

      // Ejecutar defragmentación
      heap_defragment();

      last_defrag = ticks_since_boot;
    }

    // Debug cada 12 checks (1 minuto)
    if (check_count % 12 == 0) {
      log_message(LOG_INFO,
                  "[DEFRAG] Stats - Total: %u defrags, Merges: %u blocks\r\n",
                  defrag_stats.total_defrags, defrag_stats.successful_merges);
    }

    task_yield();
  }
}

// ========================================================================
// ESTADÍSTICAS PÚBLICAS
// ========================================================================

void defrag_print_stats(void) {
  terminal_puts(&main_terminal, "\r\n=== Defragmentation Statistics ===\r\n");
  terminal_printf(&main_terminal, "Total defragmentations: %u\r\n",
                  defrag_stats.total_defrags);
  terminal_printf(&main_terminal, "Blocks merged: %u\r\n",
                  defrag_stats.successful_merges);
  terminal_printf(&main_terminal, "Last defrag: %u ticks ago\r\n",
                  ticks_since_boot - defrag_stats.last_defrag_time);

  if (defrag_stats.largest_block_before > 0) {
    terminal_printf(&main_terminal, "Last improvement: %u -> %u bytes\r\n",
                    defrag_stats.largest_block_before,
                    defrag_stats.largest_block_after);
  }

  heap_info_t current = heap_stats_fast();
  terminal_printf(&main_terminal, "\r\nCurrent heap status:\r\n");
  terminal_printf(&main_terminal, "  Free: %u bytes\r\n", current.free);
  terminal_printf(&main_terminal, "  Free blocks: %u\r\n",
                  current.free_blocks_count);
  terminal_printf(&main_terminal, "  Largest block: %u bytes\r\n",
                  current.largest_free_block);
  terminal_printf(&main_terminal, "  Fragmentation: %.2f%%\r\n",
                  current.fragmentation);
  terminal_puts(&main_terminal, "\r\n");
}

void cmd_defrag_stats(void) { defrag_print_stats(); }

void cmd_force_defrag(void) {
  terminal_puts(&main_terminal, "\r\n=== Manual Defragmentation ===\r\n");

  heap_info_t before = heap_stats_fast();
  terminal_printf(&main_terminal,
                  "Before: %.2f%% fragmentation, %u free blocks\r\n",
                  before.fragmentation, before.free_blocks_count);

  heap_defragment();

  heap_info_t after = heap_stats_fast();
  terminal_printf(&main_terminal,
                  "After: %.2f%% fragmentation, %u free blocks\r\n",
                  after.fragmentation, after.free_blocks_count);
}