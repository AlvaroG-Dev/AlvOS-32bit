#ifndef MMU_H
#define MMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flags de página
#define PAGE_PRESENT 0x001
#define PAGE_RW 0x002
#define PAGE_USER 0x004
#define PAGE_WRITETHROUGH 0x008
#define PAGE_CACHE_DISABLE 0x010
#define PAGE_ACCESSED 0x020
#define PAGE_DIRTY 0x040
#define PAGE_GLOBAL 0x100
#define PAGE_4MB 0x080 // Para páginas grandes (4MB)

// Constantes
#define PAGE_DIRECTORY_ENTRIES 1024
#define PAGE_TABLE_ENTRIES 1024
#define PAGE_SIZE 4096
#define PAGE_SIZE_4MB (4 * 1024 * 1024)
#define KERNEL_VIRTUAL_BASE 0xC0000000 // 3GB
#define FRAMEBUFFER_BASE 0xE0000000
// Niveles de privilegio
#define KERNEL_PRIVILEGE 0
#define USER_PRIVILEGE 3
#define MODULE_VIRTUAL_BASE 0xF0000000

// Estructura para direcciones virtuales/físicas
typedef struct {
  uint32_t virtual_addr;
  uint32_t physical_addr;
  uint32_t size;
  uint32_t flags;
} memory_map_t;

// Macros de alineación
#define ALIGN_4KB_DOWN(addr) ((addr) & ~0xFFF)
#define ALIGN_4KB_UP(addr) (((addr) + 0xFFF) & ~0xFFF)
#define ALIGN_4MB_DOWN(addr) ((addr) & ~0x3FFFFF)
#define ALIGN_4MB_UP(addr) (((addr) + 0x3FFFFF) & ~0x3FFFFF)

// Prototipos de funciones
void mmu_init(void);
void mmu_load_cr3(uint32_t pd_phys_addr);
void mmu_enable_paging(void);
bool mmu_map_page(uint32_t virtual_addr, uint32_t physical_addr,
                  uint32_t flags);
bool mmu_unmap_page(uint32_t virtual_addr);
bool mmu_map_region(uint32_t virtual_start, uint32_t physical_start,
                    uint32_t size, uint32_t flags);
bool mmu_unmap_region(uint32_t virtual_start, uint32_t size);
bool mmu_set_flags(uint32_t virtual_addr, uint32_t flags);
uint32_t mmu_virtual_to_physical(uint32_t virtual_addr);
bool mmu_is_mapped(uint32_t virtual_addr);

// Funciones de prueba
void mmu_run_tests(void);
bool mmu_test_basic_mapping(void);
bool mmu_test_region_mapping(void);
bool mmu_test_permissions(void);
bool mmu_ensure_physical_mapped(uint32_t phys_start, uint32_t size);
bool mmu_verify_mapping(uint32_t virtual_addr, uint32_t size);
uint32_t mmu_find_virtual_for_physical(uint32_t phys_addr);
bool mmu_ensure_physical_accessible(uint32_t phys_start, uint32_t size,
                                    uint32_t *virt_addr);

// Funciones para modo usuario
void mmu_switch_to_user_pd(uint32_t user_pd);
uint32_t mmu_get_kernel_pd(void);
bool mmu_copy_kernel_mappings(uint32_t *user_pd);
uint32_t mmu_get_current_cr3(void);
uint32_t mmu_get_page_flags(uint32_t virtual_addr);
bool mmu_set_page_user(uint32_t virtual_addr);

// Variables globales
extern uint32_t page_directory[PAGE_DIRECTORY_ENTRIES];
extern uint32_t page_tables[PAGE_DIRECTORY_ENTRIES][PAGE_TABLE_ENTRIES];
extern uint32_t used_page_tables[PAGE_DIRECTORY_ENTRIES];

#endif // MMU_H