// vmm_complete.c - Virtual Memory Manager Completo

#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "pmm.h"
#include "string.h"

// ============================================================================
// ESTRUCTURAS Y CONSTANTES
// ============================================================================

#define VMM_USER_CODE_START 0x08000000 // 128MB - inicio código usuario
#define VMM_USER_HEAP_START 0x10000000 // 256MB - inicio heap usuario
#define VMM_USER_STACK_TOP 0xBFFFFFFF  // Justo antes del kernel

// Flags para regiones
#define VMM_REGION_CODE 0x01
#define VMM_REGION_DATA 0x02
#define VMM_REGION_HEAP 0x04
#define VMM_REGION_STACK 0x08
#define VMM_REGION_SHARED 0x10

// ============================================================================
// VARIABLES GLOBALES
// ============================================================================

address_space_t kernel_address_space = {0};
static uint32_t next_virtual_addr = VMM_USER_HEAP_START;

// ============================================================================
// FUNCIONES AUXILIARES DE PAGE DIRECTORY
// ============================================================================

/**
 * Aloca un page directory nuevo con memoria física
 */
static uint32_t vmm_alloc_page_directory(void) {
  // Allocar página física para el PD
  void *pd_phys = pmm_alloc_page();
  if (!pd_phys) {
    terminal_puts(&main_terminal, "[VMM] ERROR: Cannot allocate PD\n");
    return 0;
  }

  // Mapear temporalmente en higher-half para inicializar
  uint32_t pd_phys_addr = (uint32_t)pd_phys;
  uint32_t pd_virt = KERNEL_VIRTUAL_BASE + pd_phys_addr;

  // Asegurar que esté mapeado
  if (!mmu_is_mapped(pd_virt)) {
    if (!mmu_map_page(pd_virt, pd_phys_addr, PAGE_PRESENT | PAGE_RW)) {
      pmm_free_page(pd_phys);
      return 0;
    }
  }

  // Limpiar el PD
  uint32_t *pd_ptr = (uint32_t *)pd_virt;
  memset(pd_ptr, 0, PAGE_SIZE);

  log_message(LOG_INFO, "[VMM] Allocated PD at phys=0x%08x, virt=0x%08x",
              pd_phys_addr, pd_virt);

  return pd_phys_addr;
}

/**
 * Copia mapeos del kernel al PD de usuario
 */
static bool vmm_copy_kernel_mappings_to_pd(uint32_t user_pd_phys) {
  uint32_t user_pd_virt = KERNEL_VIRTUAL_BASE + user_pd_phys;

  if (!mmu_is_mapped(user_pd_virt)) {
    terminal_printf(&main_terminal, "[VMM] ERROR: User PD 0x%08x not mapped\n",
                    user_pd_phys);
    return false;
  }

  uint32_t *user_pd = (uint32_t *)user_pd_virt;
  uint32_t *kernel_pd = page_directory;

  // Copiar entradas del kernel (768-1023) - 3GB-4GB
  for (int i = 768; i < 1024; i++) {
    user_pd[i] = kernel_pd[i];
  }

  log_message(LOG_INFO, "[VMM] Copied kernel mappings to PD 0x%08x",
              user_pd_phys);
  return true;
}

// ============================================================================
// GESTIÓN DE REGIONES
// ============================================================================

/**
 * Crea una nueva región de memoria
 */
static vmm_region_t *vmm_create_region(uint32_t virt_start, uint32_t size,
                                       uint32_t flags, uint32_t type) {
  vmm_region_t *region = (vmm_region_t *)kernel_malloc(sizeof(vmm_region_t));
  if (!region) {
    return NULL;
  }

  memset(region, 0, sizeof(vmm_region_t));

  region->virtual_start = ALIGN_4KB_DOWN(virt_start);
  region->virtual_end = ALIGN_4KB_UP(virt_start + size);
  region->physical_start = 0; // Se asignará bajo demanda
  region->flags = flags | type;
  region->next = NULL;
  region->prev = NULL;

  return region;
}

/**
 * Libera una región de memoria
 */
static void vmm_free_region(vmm_region_t *region) {
  if (!region)
    return;

  // Liberar memoria física si fue asignada
  if (region->physical_start) {
    uint32_t num_pages =
        (region->virtual_end - region->virtual_start) / PAGE_SIZE;
    pmm_free_pages((void *)(uintptr_t)region->physical_start, num_pages);
  }

  kernel_free(region);
}

/**
 * Encuentra región que contiene una dirección virtual
 */
static vmm_region_t *vmm_find_region(address_space_t *as, uint32_t virt_addr) {
  if (!as || !as->regions) {
    return NULL;
  }

  vmm_region_t *region = as->regions;
  while (region) {
    if (virt_addr >= region->virtual_start && virt_addr < region->virtual_end) {
      return region;
    }
    region = region->next;
  }

  return NULL;
}

/**
 * Inserta región en la lista ordenada
 */
static bool vmm_insert_region(address_space_t *as, vmm_region_t *new_region) {
  if (!as || !new_region) {
    return false;
  }

  // Verificar solapamientos
  vmm_region_t *current = as->regions;
  while (current) {
    if (!(new_region->virtual_end <= current->virtual_start ||
          new_region->virtual_start >= current->virtual_end)) {
      // Hay solapamiento
      log_message(LOG_ERROR,
                  "[VMM] Region overlap: 0x%08x-0x%08x with 0x%08x-0x%08x",
                  new_region->virtual_start, new_region->virtual_end,
                  current->virtual_start, current->virtual_end);
      return false;
    }
    current = current->next;
  }

  // Insertar ordenadamente
  if (!as->regions || new_region->virtual_start < as->regions->virtual_start) {
    new_region->next = as->regions;
    if (as->regions) {
      as->regions->prev = new_region;
    }
    as->regions = new_region;
  } else {
    current = as->regions;
    while (current->next &&
           current->next->virtual_start < new_region->virtual_start) {
      current = current->next;
    }

    new_region->next = current->next;
    new_region->prev = current;
    if (current->next) {
      current->next->prev = new_region;
    }
    current->next = new_region;
  }

  return true;
}

// ============================================================================
// FUNCIONES PÚBLICAS - ADDRESS SPACE
// ============================================================================

/**
 * Inicializa el VMM
 */
void vmm_init(void) {
  memset(&kernel_address_space, 0, sizeof(address_space_t));

  // El kernel usa el page directory global
  kernel_address_space.page_directory = (uint32_t)&page_directory;
  kernel_address_space.regions = NULL;
  kernel_address_space.heap_start = 0;
  kernel_address_space.heap_current = 0;
  kernel_address_space.stack_start = 0;
  kernel_address_space.stack_size = 0;

  log_message(LOG_INFO, "[VMM] Initialized (kernel PD: 0x%08x)",
              kernel_address_space.page_directory);
}

/**
 * Crea un nuevo espacio de direcciones para un proceso
 */
address_space_t *vmm_create_address_space(void) {
  // Allocar estructura
  address_space_t *as =
      (address_space_t *)kernel_malloc(sizeof(address_space_t));
  if (!as) {
    terminal_puts(&main_terminal, "[VMM] ERROR: Cannot allocate AS\n");
    return NULL;
  }

  memset(as, 0, sizeof(address_space_t));

  // Allocar page directory
  as->page_directory = vmm_alloc_page_directory();
  if (!as->page_directory) {
    kernel_free(as);
    return NULL;
  }

  // Copiar mapeos del kernel
  if (!vmm_copy_kernel_mappings_to_pd(as->page_directory)) {
    pmm_free_page((void *)(uintptr_t)as->page_directory);
    kernel_free(as);
    return NULL;
  }

  // Crear región NULL (0x0-0x1000) - inaccesible
  vmm_region_t *null_region = vmm_create_region(0x0, PAGE_SIZE, 0, 0);
  if (null_region) {
    as->regions = null_region;
  }

  log_message(LOG_INFO, "[VMM] Created address space (PD: 0x%08x)",
              as->page_directory);

  return as;
}

/**
 * Destruye un espacio de direcciones
 */
void vmm_destroy_address_space(address_space_t *as) {
  if (!as)
    return;

  // Liberar todas las regiones
  vmm_region_t *region = as->regions;
  while (region) {
    vmm_region_t *next = region->next;
    vmm_free_region(region);
    region = next;
  }

  // Liberar page directory y sus page tables
  // TODO: Liberar page tables individuales del proceso

  if (as->page_directory) {
    pmm_free_page((void *)(uintptr_t)as->page_directory);
  }

  kernel_free(as);

  log_message(LOG_INFO, "[VMM] Destroyed address space");
}

// ============================================================================
// MAPEO DE REGIONES
// ============================================================================

/**
 * Mapea una región en el espacio de direcciones
 */
bool vmm_map_region(address_space_t *as, uint32_t virt_start, uint32_t size,
                    uint32_t flags) {
  if (!as || size == 0) {
    return false;
  }

  uint32_t aligned_start = ALIGN_4KB_DOWN(virt_start);
  uint32_t aligned_end = ALIGN_4KB_UP(virt_start + size);
  uint32_t aligned_size = aligned_end - aligned_start;

  // Crear región
  vmm_region_t *region =
      vmm_create_region(aligned_start, aligned_size, flags, VMM_REGION_DATA);
  if (!region) {
    return false;
  }

  // Allocar memoria física
  uint32_t num_pages = aligned_size / PAGE_SIZE;
  void *phys_base = pmm_alloc_pages(num_pages);

  if (!phys_base) {
    kernel_free(region);
    terminal_printf(&main_terminal, "[VMM] ERROR: Cannot allocate %u pages\n",
                    num_pages);
    return false;
  }

  region->physical_start = (uint32_t)phys_base;

  // Mapear páginas en el page directory del proceso
  // Necesitamos switchear temporalmente al PD del proceso
  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  bool success = true;
  uint32_t virt_addr = region->virtual_start;
  uint32_t phys_addr = region->physical_start;

  for (uint32_t i = 0; i < num_pages; i++) {
    if (!mmu_map_page(virt_addr, phys_addr, flags)) {
      terminal_printf(&main_terminal,
                      "[VMM] ERROR: Failed to map page 0x%08x\n", virt_addr);
      success = false;
      break;
    }
    virt_addr += PAGE_SIZE;
    phys_addr += PAGE_SIZE;
  }

  // Restaurar PD original
  mmu_load_cr3(old_cr3);

  if (!success) {
    pmm_free_pages((void *)(uintptr_t)region->physical_start, num_pages);
    kernel_free(region);
    return false;
  }

  // Insertar región en la lista
  if (!vmm_insert_region(as, region)) {
    // Deshacer mapeo
    mmu_load_cr3(as->page_directory);
    for (uint32_t i = 0; i < num_pages; i++) {
      mmu_unmap_page(region->virtual_start + i * PAGE_SIZE);
    }
    mmu_load_cr3(old_cr3);

    pmm_free_pages((void *)(uintptr_t)region->physical_start, num_pages);
    kernel_free(region);
    return false;
  }

  log_message(LOG_INFO, "[VMM] Mapped region: 0x%08x-0x%08x -> phys 0x%08x",
              region->virtual_start, region->virtual_end,
              region->physical_start);

  return true;
}

/**
 * Desmapea una región
 */
bool vmm_unmap_region(address_space_t *as, uint32_t virt_start, uint32_t size) {
  if (!as)
    return false;

  uint32_t aligned_start = ALIGN_4KB_DOWN(virt_start);

  vmm_region_t *region = vmm_find_region(as, aligned_start);
  if (!region) {
    terminal_printf(&main_terminal, "[VMM] ERROR: No region at 0x%08x\n",
                    aligned_start);
    return false;
  }

  // Desmapar páginas
  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  uint32_t num_pages =
      (region->virtual_end - region->virtual_start) / PAGE_SIZE;

  for (uint32_t i = 0; i < num_pages; i++) {
    mmu_unmap_page(region->virtual_start + i * PAGE_SIZE);
  }

  mmu_load_cr3(old_cr3);

  // Remover de la lista
  if (region->prev) {
    region->prev->next = region->next;
  } else {
    as->regions = region->next;
  }

  if (region->next) {
    region->next->prev = region->prev;
  }

  vmm_free_region(region);

  log_message(LOG_INFO, "[VMM] Unmapped region at 0x%08x", aligned_start);
  return true;
}

// ============================================================================
// STACK Y HEAP DE USUARIO
// ============================================================================

/**
 * Allocar stack de usuario
 */
bool vmm_allocate_stack(address_space_t *as, uint32_t size) {
  if (!as)
    return false;

  uint32_t aligned_size = ALIGN_4KB_UP(size);

  // Stack crece hacia abajo desde VMM_USER_STACK_TOP
  uint32_t stack_bottom = VMM_USER_STACK_TOP - aligned_size + 1;

  // Crear región de stack
  vmm_region_t *stack_region =
      vmm_create_region(stack_bottom, aligned_size,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER, VMM_REGION_STACK);
  if (!stack_region) {
    return false;
  }

  // Allocar memoria física
  uint32_t num_pages = aligned_size / PAGE_SIZE;
  void *phys_base = pmm_alloc_pages(num_pages);

  if (!phys_base) {
    kernel_free(stack_region);
    return false;
  }

  stack_region->physical_start = (uint32_t)phys_base;

  // Mapear
  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  uint32_t virt_addr = stack_region->virtual_start;
  uint32_t phys_addr = stack_region->physical_start;

  for (uint32_t i = 0; i < num_pages; i++) {
    if (!mmu_map_page(virt_addr, phys_addr,
                      PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
      mmu_load_cr3(old_cr3);
      pmm_free_pages((void *)(uintptr_t)stack_region->physical_start,
                     num_pages);
      kernel_free(stack_region);
      return false;
    }
    virt_addr += PAGE_SIZE;
    phys_addr += PAGE_SIZE;
  }

  mmu_load_cr3(old_cr3);

  // Insertar región
  if (!vmm_insert_region(as, stack_region)) {
    mmu_load_cr3(as->page_directory);
    for (uint32_t i = 0; i < num_pages; i++) {
      mmu_unmap_page(stack_region->virtual_start + i * PAGE_SIZE);
    }
    mmu_load_cr3(old_cr3);
    pmm_free_pages((void *)(uintptr_t)stack_region->physical_start, num_pages);
    kernel_free(stack_region);
    return false;
  }

  as->stack_start = stack_bottom;
  as->stack_size = aligned_size;

  log_message(LOG_INFO, "[VMM] Allocated user stack: 0x%08x-0x%08x (%u KB)",
              stack_bottom, VMM_USER_STACK_TOP, aligned_size / 1024);

  return true;
}

/**
 * Allocar heap inicial de usuario
 */
bool vmm_allocate_heap(address_space_t *as, uint32_t initial_size) {
  if (!as)
    return false;

  uint32_t aligned_size = ALIGN_4KB_UP(initial_size);

  // Heap comienza en VMM_USER_HEAP_START
  vmm_region_t *heap_region =
      vmm_create_region(VMM_USER_HEAP_START, aligned_size,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER, VMM_REGION_HEAP);
  if (!heap_region) {
    return false;
  }

  // Allocar memoria física
  uint32_t num_pages = aligned_size / PAGE_SIZE;
  void *phys_base = pmm_alloc_pages(num_pages);

  if (!phys_base) {
    kernel_free(heap_region);
    return false;
  }

  heap_region->physical_start = (uint32_t)phys_base;

  // Mapear
  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  uint32_t virt_addr = heap_region->virtual_start;
  uint32_t phys_addr = heap_region->physical_start;

  for (uint32_t i = 0; i < num_pages; i++) {
    if (!mmu_map_page(virt_addr, phys_addr,
                      PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
      mmu_load_cr3(old_cr3);
      pmm_free_pages((void *)(uintptr_t)heap_region->physical_start, num_pages);
      kernel_free(heap_region);
      return false;
    }
    virt_addr += PAGE_SIZE;
    phys_addr += PAGE_SIZE;
  }

  mmu_load_cr3(old_cr3);

  // Insertar región
  if (!vmm_insert_region(as, heap_region)) {
    mmu_load_cr3(as->page_directory);
    for (uint32_t i = 0; i < num_pages; i++) {
      mmu_unmap_page(heap_region->virtual_start + i * PAGE_SIZE);
    }
    mmu_load_cr3(old_cr3);
    pmm_free_pages((void *)(uintptr_t)heap_region->physical_start, num_pages);
    kernel_free(heap_region);
    return false;
  }

  as->heap_start = VMM_USER_HEAP_START;
  as->heap_current = VMM_USER_HEAP_START;

  log_message(LOG_INFO, "[VMM] Allocated user heap: 0x%08x (%u KB)",
              VMM_USER_HEAP_START, aligned_size / 1024);

  return true;
}

/**
 * Implementar brk() - cambiar fin del heap
 */
void *vmm_brk(address_space_t *as, void *addr) {
  if (!as) {
    return NULL;
  }

  // Si addr es NULL, retornar break actual
  if (!addr) {
    return (void *)as->heap_current;
  }

  uint32_t new_brk = ALIGN_4KB_UP((uint32_t)addr);
  uint32_t old_brk = as->heap_current;

  // No permitir reducir el heap (sbrk negativo)
  if (new_brk < as->heap_start) {
    terminal_printf(&main_terminal,
                    "[VMM] ERROR: brk below heap start (0x%08x < 0x%08x)\n",
                    new_brk, as->heap_start);
    return (void *)-1;
  }

  // Si no cambia, retornar actual
  if (new_brk == old_brk) {
    return addr;
  }

  // Expandir heap
  if (new_brk > old_brk) {
    uint32_t expand_size = new_brk - old_brk;

    if (!vmm_map_region(as, old_brk, expand_size,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
      terminal_printf(&main_terminal,
                      "[VMM] ERROR: Cannot expand heap by %u bytes\n",
                      expand_size);
      return (void *)-1;
    }

    as->heap_current = new_brk;

    log_message(LOG_INFO, "[VMM] Heap expanded: 0x%08x -> 0x%08x (+%u bytes)",
                old_brk, new_brk, expand_size);
  }

  return addr;
}

// ============================================================================
// SWITCH DE ADDRESS SPACE
// ============================================================================

/**
 * Cambiar al address space del proceso
 */
void vmm_switch_address_space(address_space_t *as) {
  if (!as)
    return;

  mmu_load_cr3(as->page_directory);

  log_message(LOG_INFO, "[VMM] Switched to AS (PD: 0x%08x)",
              as->page_directory);
}

// ============================================================================
// DEBUG
// ============================================================================

/**
 * Mostrar información del address space
 */
void vmm_debug_info(address_space_t *as, Terminal *term) {
  char msg[256];

  if (!as) {
    terminal_puts(term, "[VMM] Address space: NULL\n");
    return;
  }

  terminal_puts(term, "\n=== Virtual Memory Manager ===\n");

  snprintf(msg, sizeof(msg), "Page Directory: 0x%08x\n", as->page_directory);
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "Heap: 0x%08x (current: 0x%08x)\n", as->heap_start,
           as->heap_current);
  terminal_puts(term, msg);

  snprintf(msg, sizeof(msg), "Stack: 0x%08x (size: %u KB)\n", as->stack_start,
           as->stack_size / 1024);
  terminal_puts(term, msg);

  terminal_puts(term, "\nRegions:\n");

  vmm_region_t *region = as->regions;
  uint32_t region_count = 0;

  while (region) {
    const char *type = "Unknown";

    if (region->flags & VMM_REGION_STACK)
      type = "Stack";
    else if (region->flags & VMM_REGION_HEAP)
      type = "Heap";
    else if (region->flags & VMM_REGION_CODE)
      type = "Code";
    else if (region->flags & VMM_REGION_DATA)
      type = "Data";
    else if (region->virtual_start == 0)
      type = "NULL";

    snprintf(msg, sizeof(msg),
             "  Region %u: 0x%08x-0x%08x (%u KB) %s [%c%c%c]\n", region_count++,
             region->virtual_start, region->virtual_end,
             (region->virtual_end - region->virtual_start) / 1024, type,
             (region->flags & PAGE_PRESENT) ? 'P' : '-',
             (region->flags & PAGE_RW) ? 'W' : 'R',
             (region->flags & PAGE_USER) ? 'U' : 'K');
    terminal_puts(term, msg);

    region = region->next;
  }

  terminal_puts(term, "\n");
}