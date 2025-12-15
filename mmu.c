#include "mmu.h"
#include "kernel.h"
#include "memutils.h"
#include "string.h"
#include "drawing.h"

extern char _end;
extern char _stack_bottom;
extern char _stack_top;

// Estructuras de paginación
__attribute__((section(".page_tables"), used, aligned(PAGE_SIZE)))
uint32_t page_directory[PAGE_DIRECTORY_ENTRIES];

__attribute__((section(".page_tables"), used, aligned(PAGE_SIZE)))
uint32_t page_tables[PAGE_DIRECTORY_ENTRIES][PAGE_TABLE_ENTRIES];

// Tablas de páginas actualmente en uso
uint32_t used_page_tables[PAGE_DIRECTORY_ENTRIES] = {0};

// ==================== FUNCIONES BÁSICAS ====================

void mmu_load_cr3(uint32_t pd_phys_addr) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys_addr) : "memory");
}

uint32_t mmu_get_current_cr3(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void mmu_enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__(
        "mov %%cr0, %0\n"
        "or $0x80000001, %0\n"  // PG=1, PE=1
        "mov %0, %%cr0\n"
        "jmp 1f\n"
        "1:\n"
        : "=r"(cr0)
        :
        : "memory"
    );
}

// ==================== FUNCIONES DE MAPEO ====================

bool mmu_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    // Alinear direcciones
    virtual_addr = ALIGN_4KB_DOWN(virtual_addr);
    physical_addr = ALIGN_4KB_DOWN(physical_addr);

    // Calcular índices
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;

    // Verificar límites
    if (pd_index >= PAGE_DIRECTORY_ENTRIES || pt_index >= PAGE_TABLE_ENTRIES) {
        return false;
    }

    // Manejar páginas grandes (4MB)
    if (flags & PAGE_4MB) {
        if ((virtual_addr & 0x3FFFFF) || (physical_addr & 0x3FFFFF)) {
            return false; // No alineado a 4MB
        }

        page_directory[pd_index] = physical_addr | flags | PAGE_PRESENT | PAGE_4MB;
        asm volatile("invlpg (%0)" : : "r"(virtual_addr));
        return true;
    }

    // Crear tabla de páginas si no existe
    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        uint32_t pt_phys = (uint32_t)&page_tables[pd_index];

        // Establecer flags para la tabla de páginas
        uint32_t pd_flags = PAGE_PRESENT | PAGE_RW;
        if (flags & PAGE_USER) pd_flags |= PAGE_USER;

        page_directory[pd_index] = pt_phys | pd_flags;
        used_page_tables[pd_index] = 1;

        // Limpiar la nueva tabla de páginas
        memset(&page_tables[pd_index], 0, PAGE_TABLE_ENTRIES * sizeof(uint32_t));

        // Invalidar TLB para toda la tabla
        asm volatile("invlpg (%0)" : : "r"(virtual_addr & 0xFFC00000));
    }

    // Verificar si ya está mapeado con diferentes parámetros
    if ((page_tables[pd_index][pt_index] & PAGE_PRESENT)) {
        uint32_t current_phys = page_tables[pd_index][pt_index] & ~0xFFF;
        uint32_t current_flags = page_tables[pd_index][pt_index] & 0xFFF;

        if (current_phys != physical_addr) {
            return false; // Colisión de mapeo
        }

        // Solo actualizar flags si son diferentes
        if (current_flags != (flags & 0xFFF)) {
            page_tables[pd_index][pt_index] = physical_addr | (flags & 0xFFF);
            asm volatile("invlpg (%0)" : : "r"(virtual_addr));
        }
        return true;
    }

    // Establecer la nueva entrada
    page_tables[pd_index][pt_index] = physical_addr | (flags & 0xFFF);

    // Invalidar entrada TLB específica
    asm volatile("invlpg (%0)" : : "r"(virtual_addr));
    return true;
}

bool mmu_unmap_page(uint32_t virtual_addr) {
    virtual_addr = ALIGN_4KB_DOWN(virtual_addr);
    
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;
    
    if (pd_index >= PAGE_DIRECTORY_ENTRIES || pt_index >= PAGE_TABLE_ENTRIES) {
        return false;
    }

    // Verificar si es una página grande
    if (page_directory[pd_index] & PAGE_4MB) {
        return false; // No se puede desmapear una página individual en un mapeo de 4MB
    }

    if (page_directory[pd_index] & PAGE_PRESENT) {
        page_tables[pd_index][pt_index] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr));
        return true;
    }
    
    return false;
}

bool mmu_map_region(uint32_t virtual_start, uint32_t physical_start, uint32_t size, uint32_t flags) {
    if (size == 0) return false;

    // Alinear los puntos de inicio
    uint32_t aligned_virt_start = ALIGN_4KB_DOWN(virtual_start);
    uint32_t aligned_phys_start = ALIGN_4KB_DOWN(physical_start);
    
    // Ajustar el tamaño
    uint32_t end_offset = (virtual_start + size) - aligned_virt_start;
    uint32_t aligned_size = ALIGN_4KB_UP(end_offset);
    
    // Mapear las páginas
    uint32_t virt_ptr = aligned_virt_start;
    uint32_t phys_ptr = aligned_phys_start;
    uint32_t end = aligned_virt_start + aligned_size;
    
    for (; virt_ptr < end; virt_ptr += PAGE_SIZE, phys_ptr += PAGE_SIZE) {
        if (!mmu_map_page(virt_ptr, phys_ptr, flags)) {
            // Deshacer mapeos si falla
            mmu_unmap_region(aligned_virt_start, virt_ptr - aligned_virt_start);
            return false;
        }
    }
    
    return true;
}

bool mmu_unmap_region(uint32_t virtual_start, uint32_t size) {
    if (size == 0) return false;

    // Alinear el punto de inicio
    uint32_t aligned_virt_start = ALIGN_4KB_DOWN(virtual_start);
    
    // Ajustar el tamaño
    uint32_t end_offset = (virtual_start + size) - aligned_virt_start;
    uint32_t aligned_size = ALIGN_4KB_UP(end_offset);
    
    // Desmapear las páginas
    uint32_t end = aligned_virt_start + aligned_size;
    bool success = true;
    
    for (uint32_t virt_ptr = aligned_virt_start; virt_ptr < end; virt_ptr += PAGE_SIZE) {
        if (!mmu_unmap_page(virt_ptr)) {
            success = false;
        }
    }
    
    return success;
}

// ==================== FUNCIONES DE CONSULTA ====================

uint32_t mmu_virtual_to_physical(uint32_t virtual_addr) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;
    
    if (pd_index >= PAGE_DIRECTORY_ENTRIES || pt_index >= PAGE_TABLE_ENTRIES) {
        return 0;
    }

    // Verificar si es una página grande
    if (page_directory[pd_index] & PAGE_4MB) {
        uint32_t base = page_directory[pd_index] & 0xFFC00000;
        return base + (virtual_addr & 0x3FFFFF);
    }

    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        return 0;
    }
    
    uint32_t pt_entry = page_tables[pd_index][pt_index];
    if (!(pt_entry & PAGE_PRESENT)) {
        return 0;
    }
    
    return (pt_entry & ~0xFFF) + (virtual_addr & 0xFFF);
}

bool mmu_is_mapped(uint32_t virtual_addr) {
    return mmu_virtual_to_physical(virtual_addr) != 0;
}

bool mmu_set_flags(uint32_t virtual_addr, uint32_t flags) {
    virtual_addr = ALIGN_4KB_DOWN(virtual_addr);
    
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;
    
    if (pd_index >= PAGE_DIRECTORY_ENTRIES || pt_index >= PAGE_TABLE_ENTRIES) {
        return false;
    }

    // No se pueden cambiar flags en páginas grandes directamente
    if (page_directory[pd_index] & PAGE_4MB) {
        return false;
    }

    if (page_directory[pd_index] & PAGE_PRESENT) {
        uint32_t phys_addr = page_tables[pd_index][pt_index] & ~0xFFF;
        page_tables[pd_index][pt_index] = phys_addr | (flags & 0xFFF);
        __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr));
        return true;
    }
    
    return false;
}

void mmu_map_bios_regions(void) {
    // Mapear región baja de memoria (0x0000-0x7FFF) - Vectores de interrupción y datos BIOS
    // Esta región es crítica para ACPI
    if (!mmu_map_region(0x0000, 0x0000, 0x8000, PAGE_PRESENT | PAGE_RW)) {
        terminal_puts(&main_terminal, "WARNING: Failed to map low BIOS region\r\n");
    }
    
    // Mapear EBDA (Extended BIOS Data Area) - típicamente en 0x80000-0xA0000
    // ACPI busca RSDP aquí
    if (!mmu_map_region(0x80000, 0x80000, 0x20000, PAGE_PRESENT | PAGE_RW)) {
        terminal_puts(&main_terminal, "WARNING: Failed to map EBDA region\r\n");
    }
    
    // Mapear región de ROM BIOS (0xE0000-0xFFFFF) - ACPI también busca RSDP aquí
    if (!mmu_map_region(0xE0000, 0xE0000, 0x20000, PAGE_PRESENT | PAGE_RW)) {
        terminal_puts(&main_terminal, "WARNING: Failed to map BIOS ROM region\r\n");
    }
    
    // Mapear VGA buffer (0xA0000-0xC0000) por si acaso
    if (!mmu_map_region(0xA0000, 0xA0000, 0x20000, PAGE_PRESENT | PAGE_RW)) {
        terminal_puts(&main_terminal, "WARNING: Failed to map VGA region\r\n");
    }
    
    terminal_puts(&main_terminal, "BIOS memory regions mapped for ACPI compatibility\r\n");
}

// ==================== FUNCIONES DE INICIALIZACIÓN ====================

void mmu_init(void) {
    memset(page_directory, 0, sizeof(page_directory));
    memset(page_tables, 0, sizeof(page_tables));
    memset(used_page_tables, 0, sizeof(used_page_tables));

    uint32_t pd_phys = (uint32_t)&page_directory;
    uint32_t pt_phys = (uint32_t)&page_tables;
    
    // Mapear las tablas de páginas primero (identity mapped)
    mmu_map_region(pd_phys, pd_phys, PAGE_SIZE, PAGE_PRESENT | PAGE_RW);
    mmu_map_region(pt_phys, pt_phys, PAGE_DIRECTORY_ENTRIES * PAGE_SIZE, PAGE_PRESENT | PAGE_RW);

    // **MAPEAR KERNEL EN DOS LUGARES:**
    uint32_t kernel_phys_start = 0x100000;
    uint32_t kernel_size = (uint32_t)&_end - kernel_phys_start;
    
    // 1. Identity mapping (para compatibilidad durante boot)
    mmu_map_region(kernel_phys_start, kernel_phys_start, kernel_size, PAGE_PRESENT | PAGE_RW);
    
    // 2. Higher-half mapping (3GB + 1MB)
    uint32_t kernel_virt_start = KERNEL_VIRTUAL_BASE + kernel_phys_start;
    mmu_map_region(kernel_virt_start, kernel_phys_start, kernel_size, PAGE_PRESENT | PAGE_RW);
    
    terminal_printf(&main_terminal, "Kernel mapped:\n");
    terminal_printf(&main_terminal, "  Identity: 0x%08x - 0x%08x\n", 
                   kernel_phys_start, kernel_phys_start + kernel_size);
    terminal_printf(&main_terminal, "  Higher-half: 0x%08x - 0x%08x\n", 
                   kernel_virt_start, kernel_virt_start + kernel_size);
    
    // Mapear stack del kernel (identity + higher-half)
    uint32_t stack_size = (uint32_t)&_stack_top - (uint32_t)&_stack_bottom;
    mmu_map_region((uint32_t)&_stack_bottom, (uint32_t)&_stack_bottom, stack_size, PAGE_PRESENT | PAGE_RW);
    mmu_map_region(KERNEL_VIRTUAL_BASE + (uint32_t)&_stack_bottom, (uint32_t)&_stack_bottom, 
                   stack_size, PAGE_PRESENT | PAGE_RW);
    
    // Mapear heap del kernel (identity + higher-half)
    mmu_map_region((uint32_t)kernel_heap, (uint32_t)kernel_heap, STATIC_HEAP_SIZE, PAGE_PRESENT | PAGE_RW);
    mmu_map_region(KERNEL_VIRTUAL_BASE + (uint32_t)kernel_heap, (uint32_t)kernel_heap, 
                   STATIC_HEAP_SIZE, PAGE_PRESENT | PAGE_RW);

    // Mapear regiones BIOS
    mmu_map_bios_regions();

    // Mapear framebuffer si existe
    if (boot_info.framebuffer) {
        uint32_t fb_phys = boot_info.framebuffer->common.framebuffer_addr;
        uint32_t fb_size = boot_info.framebuffer->common.framebuffer_pitch *
                           boot_info.framebuffer->common.framebuffer_height;

        mmu_map_region(FRAMEBUFFER_BASE, fb_phys, fb_size,
                       PAGE_PRESENT | PAGE_RW | PAGE_WRITETHROUGH | PAGE_CACHE_DISABLE);
    }
    g_framebuffer = (uint32_t*)FRAMEBUFFER_BASE;
    
    mmu_load_cr3((uint32_t)&page_directory);
    mmu_enable_paging();
    
    terminal_puts(&main_terminal, "MMU initialized with higher-half kernel mapping\r\n");
}

// ==================== FUNCIONES DE PRUEBA ====================

void mmu_run_tests(void) {
    bool all_passed = true;
    
    if (!mmu_test_basic_mapping()) {
        put_string("MMU Basic Mapping Test FAILED\n");
        all_passed = false;
    }
    
    if (!mmu_test_region_mapping()) {
        put_string("MMU Region Mapping Test FAILED\n");
        all_passed = false;
    }
    
    if (!mmu_test_permissions()) {
        put_string("MMU Permissions Test FAILED\n");
        all_passed = false;
    }
    
    if (all_passed) {
        put_string("All MMU tests PASSED\n");
    }
}

bool mmu_test_basic_mapping(void) {
    // Test 1: Mapeo básico de una página
    uint32_t test_virt = 0xC000000;  // 16MB virtual
    uint32_t test_phys = 0xF00000;   // 2MB físico
    
    if (!mmu_map_page(test_virt, test_phys, PAGE_PRESENT | PAGE_RW)) {
        return false;
    }
    
    if (mmu_virtual_to_physical(test_virt) != test_phys) {
        return false;
    }
    
    // Test 2: Desmapear
    if (!mmu_unmap_page(test_virt)) {
        return false;
    }
    
    if (mmu_virtual_to_physical(test_virt) != 0) {
        return false;
    }
    
    return true;
}

bool mmu_test_region_mapping(void) {
    // Test: Mapeo de una región de 4 páginas
    uint32_t test_virt = 0xC600000;  // 32MB virtual
    uint32_t test_phys = 0xC10000;   // 3MB físico
    uint32_t region_size = 4 * PAGE_SIZE;
    
    if (!mmu_map_region(test_virt, test_phys, region_size, PAGE_PRESENT | PAGE_RW)) {
        return false;
    }
    
    // Verificar cada página
    for (uint32_t i = 0; i < region_size; i += PAGE_SIZE) {
        if (mmu_virtual_to_physical(test_virt + i) != (test_phys + i)) {
            return false;
        }
    }
    
    // Desmapear
    if (!mmu_unmap_region(test_virt, region_size)) {
        return false;
    }
    
    // Verificar que estén desmapeadas
    for (uint32_t i = 0; i < region_size; i += PAGE_SIZE) {
        if (mmu_virtual_to_physical(test_virt + i) != 0) {
            return false;
        }
    }
    
    return true;
}

bool mmu_test_permissions(void) {
    // Test: Verificar flags de permisos
    uint32_t test_virt = 0xFF00000;  // 48MB virtual
    uint32_t test_phys = 0xDE0000;   // 4MB físico
    
    // Mapear con permisos RW
    if (!mmu_map_page(test_virt, test_phys, PAGE_PRESENT | PAGE_RW)) {
        return false;
    }
    
    // Verificar flags
    uint32_t pd_index = test_virt >> 22;
    uint32_t pt_index = (test_virt >> 12) & 0x3FF;
    uint32_t flags = page_tables[pd_index][pt_index] & 0xFFF;
    
    if ((flags & (PAGE_PRESENT | PAGE_RW)) != (PAGE_PRESENT | PAGE_RW)) {
        return false;
    }
    
    // Cambiar a solo lectura
    if (!mmu_set_flags(test_virt, PAGE_PRESENT)) {
        return false;
    }
    
    flags = page_tables[pd_index][pt_index] & 0xFFF;
    if ((flags & PAGE_RW) != 0) {
        return false;
    }
    
    // Limpiar
    mmu_unmap_page(test_virt);
    return true;
}

bool mmu_ensure_physical_mapped(uint32_t phys_start, uint32_t size) {
    // Validaciones más específicas en lugar de rechazar todo por debajo de 1MB
    
    // Rechazar direcciones claramente inválidas (primeros 4KB son problemáticos)
    if (phys_start < 0x1000) {
        terminal_printf(&main_terminal, "ERROR: Physical address 0x%08x too low (below 4KB)\n", phys_start);
        return false;
    }
    
    // Permitir regiones BIOS/ACPI importantes pero validar que no sean NULL
    if (phys_start == 0 || size == 0) {
        terminal_printf(&main_terminal, "ERROR: Invalid parameters: phys=0x%08x, size=%u\n", phys_start, size);
        return false;
    }
    
    // Verificar overflow
    if (phys_start + size < phys_start) {
        terminal_printf(&main_terminal, "ERROR: Address overflow: phys=0x%08x, size=%u\n", phys_start, size);
        return false;
    }

    uint32_t aligned_start = ALIGN_4KB_DOWN(phys_start);
    uint32_t end_phys = phys_start + size;
    uint32_t aligned_end = ALIGN_4KB_UP(end_phys);
    uint32_t aligned_size = aligned_end - aligned_start;
    
    // Usar región de mapeo directo del kernel (KERNEL_VIRTUAL_BASE)
    uint32_t virt_base = KERNEL_VIRTUAL_BASE + aligned_start;
    
    terminal_printf(&main_terminal, "Mapping physical region: phys=0x%08x, size=%u, virt=0x%08x\n",
                   aligned_start, aligned_size, virt_base);
    
    for (uint32_t offset = 0; offset < aligned_size; offset += PAGE_SIZE) {
        uint32_t current_phys = aligned_start + offset;
        uint32_t current_virt = virt_base + offset;
        
        // Verificar si ya está mapeado
        if (!mmu_is_mapped(current_virt)) {
            //terminal_printf(&main_terminal, "Mapping page: phys=0x%08x -> virt=0x%08x\n", 
            //               current_phys, current_virt);
            if (!mmu_map_page(current_virt, current_phys, PAGE_PRESENT | PAGE_RW)) {
                terminal_printf(&main_terminal, "ERROR: Failed to map page at phys=0x%08x\n", current_phys);
                return false;
            }
        } else {
            // Verificar que el mapeo existente sea correcto
            uint32_t mapped_phys = mmu_virtual_to_physical(current_virt);
            if (mapped_phys != current_phys) {
                terminal_printf(&main_terminal, "WARNING: Address conflict at virt=0x%08x: mapped=0x%08x, requested=0x%08x\n",
                               current_virt, mapped_phys, current_phys);
                // Intentar remapear
                if (!mmu_map_page(current_virt, current_phys, PAGE_PRESENT | PAGE_RW)) {
                    terminal_printf(&main_terminal, "ERROR: Failed to remap conflicting page\n");
                    return false;
                }
            }
        }
    }
    return true;
}

bool mmu_verify_mapping(uint32_t virtual_addr, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t test_addr = virtual_addr + i;
        if (!mmu_is_mapped(test_addr)) {
            return false;
        }
        
        // Intentar lectura de prueba
        volatile uint32_t* ptr = (volatile uint32_t*)test_addr;
        uint32_t value;
        __asm__ __volatile__ (
            "movl (%1), %0\n"
            : "=r" (value)
            : "r" (ptr)
            : "memory"
        );
    }
    return true;
}

uint32_t mmu_find_virtual_for_physical(uint32_t phys_addr) {
    phys_addr = ALIGN_4KB_DOWN(phys_addr);
    
    // Buscar primero en la región de mapeo directo esperada
    uint32_t expected_virt = KERNEL_VIRTUAL_BASE + phys_addr;
    if (mmu_is_mapped(expected_virt) && 
        mmu_virtual_to_physical(expected_virt) == phys_addr) {
        return expected_virt;
    }
    
    // Buscar en regiones alternativas conocidas
    uint32_t search_ranges[] = {
        0xC0000000, 0xD0000000,  // Kernel space
        0xE0000000, 0xF0000000,  // Device/alternate mapping space
        0x0000000                // End marker
    };
    
    for (int range_idx = 0; search_ranges[range_idx] != 0; range_idx++) {
        uint32_t search_base = search_ranges[range_idx];
        uint32_t search_end = search_ranges[range_idx + 1];
        if (search_end == 0) search_end = search_base + 0x10000000; // 256MB por defecto
        
        for (uint32_t search_addr = search_base; search_addr < search_end; search_addr += PAGE_SIZE) {
            if (mmu_is_mapped(search_addr)) {
                uint32_t mapped_phys = mmu_virtual_to_physical(search_addr);
                if (mapped_phys == phys_addr) {
                    return search_addr;
                }
            }
        }
    }
    
    return 0; // No encontrado
}

// Mejorar mmu_ensure_physical_accessible en mmu.c
bool mmu_ensure_physical_accessible(uint32_t phys_start, uint32_t size, uint32_t* virt_addr) {
    // Validar parámetros básicos
    if (size == 0) {
        terminal_printf(&main_terminal, "ERROR: Invalid size 0 for phys=0x%08x\n", phys_start);
        return false;
    }
    
    // PERMITIR direcciones bajas pero con validación específica
    if (phys_start < 0x500) { // Permitir vectores de interrupción y datos BIOS básicos
        // Estas son direcciones críticas del sistema que ACPI puede necesitar
        //terminal_printf(&main_terminal, "WARNING: Accessing very low memory 0x%08x (size=%u)\n", phys_start, size);
    }
    
    // Verificar overflow
    if (phys_start + size < phys_start) {
        terminal_printf(&main_terminal, "ERROR: Address overflow: phys=0x%08x, size=%u\n", phys_start, size);
        return false;
    }

    uint32_t aligned_start = ALIGN_4KB_DOWN(phys_start);
    uint32_t end_phys = phys_start + size;
    uint32_t aligned_end = ALIGN_4KB_UP(end_phys);
    uint32_t aligned_size = aligned_end - aligned_start;
    
    // Buscar si ya está mapeada
    uint32_t found_virt = mmu_find_virtual_for_physical(aligned_start);
    
    if (found_virt != 0) {
        //terminal_printf(&main_terminal, "Physical 0x%08x already mapped at virtual 0x%08x\n", 
        //               aligned_start, found_virt);
        *virt_addr = found_virt + (phys_start - aligned_start);
        
        // VERIFICAR que el mapeo sea válido para todo el rango solicitado
        for (uint32_t offset = 0; offset < aligned_size; offset += PAGE_SIZE) {
            uint32_t test_virt = found_virt + offset;
            uint32_t test_phys = mmu_virtual_to_physical(test_virt);
            if (test_phys != (aligned_start + offset)) {
                terminal_printf(&main_terminal, "ERROR: Mapping inconsistency at offset 0x%x: virt=0x%08x -> phys=0x%08x, expected=0x%08x\n",
                               offset, test_virt, test_phys, aligned_start + offset);
                return false;
            }
        }
        return true;
    }
    
    // Usar región de mapeo directo del kernel (KERNEL_VIRTUAL_BASE)
    uint32_t target_virt = KERNEL_VIRTUAL_BASE + aligned_start;
    
    // Verificar si la región virtual está disponible
    for (uint32_t offset = 0; offset < aligned_size; offset += PAGE_SIZE) {
        uint32_t check_virt = target_virt + offset;
        if (mmu_is_mapped(check_virt)) {
            uint32_t existing_phys = mmu_virtual_to_physical(check_virt);
            if (existing_phys != (aligned_start + offset)) {
                // Conflicto: buscar otra región virtual
                terminal_printf(&main_terminal, "Virtual address conflict at 0x%08x, searching alternative\n", check_virt);
                
                // Buscar región alternativa
                uint32_t alt_base = 0xD0000000; // Región alternativa
                for (uint32_t try_base = alt_base; try_base < 0xF0000000; try_base += 0x100000) {
                    bool region_free = true;
                    for (uint32_t test_offset = 0; test_offset < aligned_size; test_offset += PAGE_SIZE) {
                        if (mmu_is_mapped(try_base + test_offset)) {
                            region_free = false;
                            break;
                        }
                    }
                    if (region_free) {
                        target_virt = try_base;
                        terminal_printf(&main_terminal, "Using alternative virtual base 0x%08x\n", target_virt);
                        break;
                    }
                }
                
                if (target_virt == KERNEL_VIRTUAL_BASE + aligned_start) {
                    terminal_printf(&main_terminal, "ERROR: No available virtual address space found\n");
                    return false;
                }
                break;
            }
        }
    }
    
    //terminal_printf(&main_terminal, "Mapping new region: phys=0x%08x, size=%u, virt=0x%08x\n",
    //               aligned_start, aligned_size, target_virt);
    
    // Mapear página por página con verificación
    for (uint32_t offset = 0; offset < aligned_size; offset += PAGE_SIZE) {
        uint32_t current_phys = aligned_start + offset;
        uint32_t current_virt = target_virt + offset;
        
        if (!mmu_map_page(current_virt, current_phys, PAGE_PRESENT | PAGE_RW)) {
            terminal_printf(&main_terminal, "ERROR: Failed to map page phys=0x%08x -> virt=0x%08x\n", 
                           current_phys, current_virt);
            
            // Deshacer mapeos parciales
            for (uint32_t cleanup_offset = 0; cleanup_offset < offset; cleanup_offset += PAGE_SIZE) {
                mmu_unmap_page(target_virt + cleanup_offset);
            }
            return false;
        }
        
        // Verificar inmediatamente el mapeo
        uint32_t verify_phys = mmu_virtual_to_physical(current_virt);
        if (verify_phys != current_phys) {
            terminal_printf(&main_terminal, "ERROR: Mapping verification failed: virt=0x%08x -> phys=0x%08x, expected=0x%08x\n",
                           current_virt, verify_phys, current_phys);
            
            // Deshacer mapeos
            for (uint32_t cleanup_offset = 0; cleanup_offset <= offset; cleanup_offset += PAGE_SIZE) {
                mmu_unmap_page(target_virt + cleanup_offset);
            }
            return false;
        }
    }
    
    *virt_addr = target_virt + (phys_start - aligned_start);
    //terminal_printf(&main_terminal, "Successfully mapped: final virt=0x%08x for phys=0x%08x\n", *virt_addr, phys_start);
    return true;
}


// Cambiar a page directory de usuario
void mmu_switch_to_user_pd(uint32_t user_pd) {
    mmu_load_cr3(user_pd);
}

// Obtener page directory actual del kernel
uint32_t mmu_get_kernel_pd(void) {
    return (uint32_t)&page_directory;
}

// Copiar mapeos del kernel al espacio de usuario
bool mmu_copy_kernel_mappings(uint32_t* user_pd) {
    if (!user_pd) return false;
    
    // Copiar primeros 768 entries (0-3GB) del kernel
    for (int i = 0; i < 768; i++) {
        user_pd[i] = page_directory[i];
    }
    
    return true;
}