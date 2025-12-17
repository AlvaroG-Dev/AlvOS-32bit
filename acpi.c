#include "acpi.h"
#include "driver_system.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "kernel.h"
#include "memutils.h"
#include "mmu.h"
#include "module_loader.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "task_utils.h"
#include "terminal.h"
#include "vfs.h"

// Global ACPI information
acpi_info_t acpi_info = {0};

// Cache para evitar mapeos repetitivos
typedef struct {
  uint32_t phys_start;
  uint32_t virt_start;
  uint32_t size;
  bool valid;
} mapping_cache_t;

static mapping_cache_t mapping_cache[8] = {0}; // Cache simple con 8 entradas
static int cache_index = 0;

// Estructura para guardar el estado del sistema antes del suspend
typedef struct {
  uint32_t cr0, cr2, cr3, cr4;
  uint32_t gdt_base, gdt_limit;
  uint32_t idt_base, idt_limit;
  uint32_t esp, ebp;
  uint32_t eax, ebx, ecx, edx, esi, edi;
  uint32_t eflags;
  bool valid;
} suspend_context_t;

static suspend_context_t suspend_context = {0};

// Forward declarations
static bool acpi_parse_fadt(acpi_fadt_t *fadt);
static uint8_t acpi_calculate_checksum(void *table, size_t length);
static void acpi_parse_dsdt_for_s5(void);
acpi_rsdp_t *acpi_search_rsdp_in_range(void *start, size_t length);
void acpi_parse_rsdt(void);
void acpi_parse_xsdt(void);

static void acpi_clear_mapping_cache(void) {
  terminal_puts(&main_terminal,
                "ACPI: Clearing mapping cache due to corruption\n");
  memset(mapping_cache, 0, sizeof(mapping_cache));
  cache_index = 0;
}

bool mmu_ensure_physical_accessible_cached(uint32_t phys_start, uint32_t size,
                                           uint32_t *virt_addr) {
  uint32_t aligned_start = ALIGN_4KB_DOWN(phys_start);
  uint32_t aligned_size = ALIGN_4KB_UP(phys_start + size) - aligned_start;

  // DEBUG: Verificar parámetros
  // terminal_printf(&main_terminal, "ACPI Cache: Request phys=0x%08x, size=%u,
  // aligned_phys=0x%08x, aligned_size=%u\n",
  //               phys_start, size, aligned_start, aligned_size);

  // Buscar en el cache primero - CON VALIDACIÓN ROBUSTA
  for (int i = 0; i < 8; i++) {
    if (mapping_cache[i].valid &&
        mapping_cache[i].phys_start <= aligned_start &&
        (mapping_cache[i].phys_start + mapping_cache[i].size) >=
            (aligned_start + aligned_size)) {

      uint32_t offset = phys_start - mapping_cache[i].phys_start;
      uint32_t candidate_virt = mapping_cache[i].virt_start + offset;

      // VERIFICAR que el mapeo sea correcto
      uint32_t verified_phys = mmu_virtual_to_physical(candidate_virt);
      if (verified_phys == phys_start) {
        *virt_addr = candidate_virt;
        // terminal_printf(&main_terminal, "ACPI Cache: Using cached mapping
        // [%d]: phys=0x%08x -> virt=0x%08x (VERIFIED)\n",
        //                i, phys_start, *virt_addr);
        return true;
      } else {
        terminal_printf(&main_terminal,
                        "ACPI Cache: Cached mapping [%d] INVALID: virt=0x%08x "
                        "maps to phys=0x%08x, expected 0x%08x\n",
                        i, candidate_virt, verified_phys, phys_start);
        mapping_cache[i].valid = false; // Invalidar entrada corrupta
        // NO DEVOLVER LA DIRECCIÓN VIRTUAL CORRUPTA - continuar la búsqueda
      }
    }
  }

  // Si no está en cache o la caché es inválida, usar la función normal
  // terminal_printf(&main_terminal, "ACPI Cache: No valid cache entry, calling
  // mmu_ensure_physical_accessible\n");

  if (!mmu_ensure_physical_accessible(phys_start, size, virt_addr)) {
    terminal_printf(
        &main_terminal,
        "ACPI Cache: mmu_ensure_physical_accessible FAILED for phys=0x%08x\n",
        phys_start);
    return false;
  }

  // VERIFICAR el mapeo resultante antes de agregarlo al cache
  uint32_t verified_phys = mmu_virtual_to_physical(*virt_addr);
  if (verified_phys != phys_start) {
    terminal_printf(&main_terminal,
                    "ACPI Cache: CRITICAL - New mapping incorrect: virt=0x%08x "
                    "-> phys=0x%08x, expected 0x%08x\n",
                    *virt_addr, verified_phys, phys_start);
    return false;
  }

  // Agregar al cache - CON CÁLCULO CORRECTO
  mapping_cache[cache_index].phys_start = aligned_start;
  mapping_cache[cache_index].virt_start = ALIGN_4KB_DOWN(*virt_addr);
  mapping_cache[cache_index].size = aligned_size;
  mapping_cache[cache_index].valid = true;

  // terminal_printf(&main_terminal, "ACPI Cache: Added to cache[%d]:
  // phys=0x%08x, virt=0x%08x, size=%u\n",
  //                cache_index, aligned_start,
  //                mapping_cache[cache_index].virt_start, aligned_size);

  cache_index = (cache_index + 1) % 8;

  return true;
}

// Nombres de las tablas ACPI
static struct {
  const char *signature;
  const char *name;
} table_names[] = {{"FACP", "Fixed ACPI Description Table (FADT)"},
                   {"DSDT", "Differentiated System Description Table"},
                   {"SSDT", "Secondary System Description Table"},
                   {"APIC", "Multiple APIC Description Table (MADT)"},
                   {"MCFG", "Memory Mapped Configuration"},
                   {"HPET", "High Precision Event Timer"},
                   {"WAET", "Windows ACPI Emulated Devices Table"},
                   {"SRAT", "System Resource Affinity Table"},
                   {"SLIT", "System Locality Information Table"},
                   {NULL, NULL}};

// ========================================================================
// INICIALIZACIÓN PRINCIPAL
// ========================================================================

// Función mejorada de inicialización ACPI en acpi.c
void acpi_init(void) {
  terminal_puts(&main_terminal, "Initializing ACPI subsystem...\r\n");

  memset(&acpi_info, 0, sizeof(acpi_info));

  // Limpiar caché de mapeo
  memset(mapping_cache, 0, sizeof(mapping_cache));
  cache_index = 0;

  // Buscar RSDP con manejo de errores mejorado
  acpi_info.rsdp = acpi_find_rsdp();
  if (!acpi_info.rsdp) {
    terminal_puts(&main_terminal,
                  "ACPI: RSDP not found. ACPI not available.\r\n");
    return;
  }

  // terminal_printf(&main_terminal, "ACPI: Found RSDP at 0x%08x\r\n",
  // (uint32_t)acpi_info.rsdp); terminal_printf(&main_terminal, "ACPI: OEM ID:
  // %.6s\r\n", acpi_info.rsdp->oem_id); terminal_printf(&main_terminal, "ACPI:
  // Revision: %u\r\n", acpi_info.rsdp->revision);

  // Verificar integridad del RSDP antes de continuar
  if (!acpi_validate_checksum(acpi_info.rsdp, 20)) {
    terminal_puts(&main_terminal,
                  "ACPI: RSDP checksum invalid, ACPI disabled\r\n");
    acpi_info.rsdp = NULL;
    return;
  }

  // Para ACPI 2.0+, verificar checksum extendido
  if (acpi_info.rsdp->revision >= 2) {
    if (!acpi_validate_checksum(acpi_info.rsdp, acpi_info.rsdp->length)) {
      terminal_puts(&main_terminal,
                    "ACPI: RSDP extended checksum invalid, ACPI disabled\r\n");
      acpi_info.rsdp = NULL;
      return;
    }
  }

  // Determinar versión ACPI
  acpi_info.acpi_version = (acpi_info.rsdp->revision >= 2) ? 2 : 1;

  // Parsear tablas con manejo de errores
  // terminal_puts(&main_terminal, "ACPI: Parsing system tables...\r\n");
  acpi_parse_tables();

  if (acpi_info.table_count == 0) {
    terminal_puts(&main_terminal,
                  "ACPI: No valid tables found, ACPI disabled\r\n");
    acpi_info.rsdp = NULL;
    return;
  }

  // DEBUG: Verificar qué tablas se almacenaron
  terminal_printf(&main_terminal, "ACPI: Successfully stored %u tables:\r\n",
                  acpi_info.table_count);
  for (uint32_t i = 0; i < acpi_info.table_count; i++) {
    if (acpi_info.tables[i]) {
      acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_info.tables[i];
      terminal_printf(&main_terminal, "  [%u] %.4s at 0x%08x, length=%u\r\n", i,
                      header->signature, (uint32_t)header, header->length);
    }
  }

  // Buscar FADT para inicializar PM con mejor manejo de errores
  // terminal_puts(&main_terminal, "ACPI: Searching for FACP table...\r\n");
  acpi_info.fadt = (acpi_fadt_t *)acpi_find_table("FACP");

  if (!acpi_info.fadt) {
    terminal_puts(&main_terminal, "ACPI: FADT not found via acpi_find_table, "
                                  "attempting manual search\r\n");

    // Búsqueda manual más robusta
    for (uint32_t i = 0; i < acpi_info.table_count; i++) {
      if (acpi_info.tables[i]) {
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_info.tables[i];

        // Verificar que el header sea accesible
        if (!mmu_is_mapped((uint32_t)header) ||
            !mmu_is_mapped((uint32_t)header + sizeof(acpi_sdt_header_t) - 1)) {
          // terminal_printf(&main_terminal, "  Table [%u] not properly mapped,
          // skipping\r\n", i);
          continue;
        }

        // terminal_printf(&main_terminal, "  Checking table [%u]: %.4s\r\n", i,
        // header->signature);

        if (memcmp(header->signature, "FACP", 4) == 0) {
          // terminal_printf(&main_terminal, "  Found FACP manually at 0x%08x,
          // length=%u\r\n", (uint32_t)header, header->length);

          // Verificar que toda la tabla esté mapeada
          if (!mmu_is_mapped((uint32_t)header + header->length - 1)) {
            terminal_puts(
                &main_terminal,
                "  FACP table not fully mapped, attempting remap\r\n");

            // Intentar remapear la tabla completa
            uint32_t table_phys = mmu_virtual_to_physical((uint32_t)header);
            if (table_phys == 0) {
              terminal_puts(
                  &main_terminal,
                  "  Cannot determine physical address, skipping\r\n");
              continue;
            }

            uint32_t new_virt = 0;
            if (!mmu_ensure_physical_accessible_cached(
                    table_phys, header->length, &new_virt)) {
              terminal_puts(&main_terminal,
                            "  Failed to remap FACP table, skipping\r\n");
              continue;
            }

            header = (acpi_sdt_header_t *)new_virt;
            // terminal_printf(&main_terminal, "  Remapped FACP to 0x%08x\r\n",
            // new_virt);
          }

          // Verificar checksum antes de usar
          if (!acpi_validate_checksum(header, header->length)) {
            terminal_puts(&main_terminal,
                          "  FACP checksum invalid, skipping\r\n");
            continue;
          }

          acpi_info.fadt = (acpi_fadt_t *)header;
          // terminal_puts(&main_terminal, "  FACP validation passed\r\n");
          break;
        }
      }
    }
  }

  // Intentar parsear FADT si se encontró
  if (acpi_info.fadt) {
    // terminal_puts(&main_terminal, "ACPI: Parsing FADT for power
    // management...\r\n");

    if (acpi_parse_fadt(acpi_info.fadt)) {
      terminal_puts(&main_terminal,
                    "ACPI: Power management initialized successfully\r\n");
    } else {
      terminal_puts(
          &main_terminal,
          "ACPI: Failed to parse FADT, power management unavailable\r\n");
      acpi_info.fadt = NULL;
    }
  } else {
    terminal_puts(&main_terminal,
                  "ACPI: No FADT found, power management unavailable\r\n");
  }

  // Marcar como inicializado solo si tenemos al menos las tablas básicas
  acpi_info.initialized = true;
  // terminal_printf(&main_terminal, "ACPI initialization complete. Version
  // %u.0, %u tables, PM %s.\r\n",
  //                acpi_info.acpi_version,
  //                acpi_info.table_count,
  //                acpi_info.fadt ? "available" : "unavailable");
  //
  //  Limpiar caché si hubo problemas
  if (acpi_info.table_count > 0 && !acpi_info.fadt) {
    terminal_puts(&main_terminal,
                  "ACPI: Clearing mapping cache due to FADT issues\r\n");
    acpi_clear_mapping_cache();
  }
}

// ========================================================================
// BÚSQUEDA Y VALIDACIÓN DE RSDP
// ========================================================================

acpi_rsdp_t *acpi_find_rsdp(void) {
  // terminal_puts(&main_terminal, "ACPI: Starting RSDP search...\r\n");

  // Primero intentar leer el puntero EBDA
  uint16_t ebda_segment = 0;

  // terminal_printf(&main_terminal, "ACPI: Checking EBDA pointer at
  // 0x040E...\r\n");

  // Buscar la dirección virtual donde está mapeado 0x40E
  uint32_t ebda_ptr_virt = 0;
  if (!mmu_ensure_physical_accessible_cached(0x40E, 2, &ebda_ptr_virt)) {
    terminal_puts(&main_terminal, "ACPI: Cannot access EBDA pointer\r\n");
  } else {
    uint16_t *ebda_ptr = (uint16_t *)ebda_ptr_virt;
    ebda_segment = *ebda_ptr;
    // terminal_printf(&main_terminal, "ACPI: EBDA segment = 0x%04x\r\n",
    // ebda_segment);
  }

  if (ebda_segment >= 0x8000 && ebda_segment < 0xA000) {
    uint32_t ebda_address = ebda_segment << 4;

    // terminal_printf(&main_terminal, "ACPI: Searching EBDA at 0x%08x...\r\n",
    // ebda_address);

    uint32_t ebda_virtual = 0;
    if (mmu_ensure_physical_accessible_cached(ebda_address, 1024,
                                              &ebda_virtual)) {
      // terminal_printf(&main_terminal, "ACPI: EBDA accessible at virtual
      // 0x%08x\r\n", ebda_virtual);
      acpi_rsdp_t *rsdp = acpi_search_rsdp_in_range((void *)ebda_virtual, 1024);
      if (rsdp) {
        // terminal_printf(&main_terminal, "ACPI: Found RSDP in EBDA at physical
        // 0x%08x\r\n", ebda_address);
        return rsdp;
      } else {
        terminal_puts(&main_terminal, "ACPI: RSDP not found in EBDA\r\n");
      }
    } else {
      terminal_printf(&main_terminal,
                      "ACPI: Failed to access EBDA at 0x%08x\r\n",
                      ebda_address);
    }
  } else {
    terminal_printf(
        &main_terminal,
        "ACPI: Invalid EBDA segment 0x%04x, skipping EBDA search\r\n",
        ebda_segment);
  }

  // Buscar en BIOS ROM area
  // terminal_puts(&main_terminal, "ACPI: Searching for RSDP in BIOS ROM area
  // (0xE0000-0xFFFFF)...\r\n");

  uint32_t bios_virtual = 0;
  if (mmu_ensure_physical_accessible_cached(0xE0000, 0x20000, &bios_virtual)) {
    // terminal_printf(&main_terminal, "ACPI: BIOS ROM accessible at virtual
    // 0x%08x\r\n", bios_virtual);
    acpi_rsdp_t *rsdp =
        acpi_search_rsdp_in_range((void *)bios_virtual, 0x20000);
    if (rsdp) {
      // terminal_printf(&main_terminal, "ACPI: Found RSDP in BIOS ROM at
      // physical 0x%08x\r\n", (uint32_t)rsdp - bios_virtual + 0xE0000);
      return rsdp;
    } else {
      terminal_puts(&main_terminal,
                    "ACPI: RSDP not found in BIOS ROM area\r\n");
    }
  } else {
    terminal_puts(&main_terminal, "ACPI: Failed to access BIOS ROM area\r\n");
  }

  return NULL;
}

acpi_rsdp_t *acpi_search_rsdp_in_range(void *start, size_t length) {
  uint8_t *ptr = (uint8_t *)start;
  uint8_t *end = ptr + length;

  // RSDP debe estar alineado a 16 bytes
  for (; ptr < end; ptr += 16) {
    if (memcmp(ptr, "RSD PTR ", 8) == 0) {
      acpi_rsdp_t *rsdp = (acpi_rsdp_t *)ptr;

      // Validar checksum de los primeros 20 bytes
      if (acpi_validate_checksum(rsdp, 20)) {
        // Para ACPI 2.0+, validar checksum extendido
        if (rsdp->revision >= 2 && rsdp->length > 20) {
          if (acpi_validate_checksum(rsdp, rsdp->length)) {
            return rsdp;
          }
        } else {
          return rsdp;
        }
      }
    }
  }

  return NULL;
}

bool acpi_validate_checksum(void *table, size_t length) {
  uint8_t sum = 0;
  uint8_t *bytes = (uint8_t *)table;

  for (size_t i = 0; i < length; i++) {
    sum += bytes[i];
  }

  return sum == 0;
}

// ========================================================================
// PARSEO DE TABLAS (usando cache)
// ========================================================================

void acpi_parse_tables(void) {
  if (acpi_info.acpi_version >= 2 && acpi_info.rsdp->xsdt_address) {
    // Usar XSDT para ACPI 2.0+
    uint64_t xsdt_phys = acpi_info.rsdp->xsdt_address;

    // Solo manejar direcciones de 32 bits por ahora
    if (xsdt_phys > 0xFFFFFFFF) {
      terminal_puts(&main_terminal,
                    "ACPI: XSDT address too high, falling back to RSDT\r\n");
      acpi_parse_rsdt();
      return;
    }

    uint32_t xsdt_virt = 0;
    if (!mmu_ensure_physical_accessible_cached(
            (uint32_t)xsdt_phys, sizeof(acpi_sdt_header_t), &xsdt_virt)) {
      terminal_puts(&main_terminal, "ACPI: Failed to map XSDT header\r\n");
      return;
    }

    acpi_info.xsdt = (acpi_xsdt_t *)xsdt_virt;

    if (memcmp(acpi_info.xsdt->header.signature, "XSDT", 4) != 0) {
      terminal_puts(&main_terminal, "ACPI: Invalid XSDT signature\r\n");
      acpi_parse_rsdt();
      return;
    }

    // Mapear toda la XSDT
    if (!mmu_ensure_physical_accessible_cached(
            (uint32_t)xsdt_phys, acpi_info.xsdt->header.length, &xsdt_virt)) {
      terminal_puts(&main_terminal, "ACPI: Failed to map complete XSDT\r\n");
      return;
    }

    if (!acpi_validate_checksum(acpi_info.xsdt,
                                acpi_info.xsdt->header.length)) {
      terminal_puts(&main_terminal, "ACPI: XSDT checksum invalid\r\n");
      acpi_parse_rsdt();
      return;
    }

    acpi_parse_xsdt();
  } else {
    // Usar RSDT para ACPI 1.0
    acpi_parse_rsdt();
  }
}

void acpi_parse_rsdt(void) {
  uint32_t rsdt_phys = acpi_info.rsdp->rsdt_address;

  uint32_t rsdt_virt = 0;
  if (!mmu_ensure_physical_accessible_cached(
          rsdt_phys, sizeof(acpi_sdt_header_t), &rsdt_virt)) {
    terminal_puts(&main_terminal, "ACPI: Failed to map RSDT header\r\n");
    return;
  }

  acpi_info.rsdt = (acpi_rsdt_t *)rsdt_virt;

  if (memcmp(acpi_info.rsdt->header.signature, "RSDT", 4) != 0) {
    terminal_puts(&main_terminal, "ACPI: Invalid RSDT signature\r\n");
    return;
  }

  // Mapear toda la RSDT
  if (!mmu_ensure_physical_accessible_cached(
          rsdt_phys, acpi_info.rsdt->header.length, &rsdt_virt)) {
    terminal_puts(&main_terminal, "ACPI: Failed to map complete RSDT\r\n");
    return;
  }

  if (!acpi_validate_checksum(acpi_info.rsdt, acpi_info.rsdt->header.length)) {
    terminal_puts(&main_terminal, "ACPI: RSDT checksum invalid\r\n");
    return;
  }

  // Parsear entradas de la RSDT
  uint32_t entry_count =
      (acpi_info.rsdt->header.length - sizeof(acpi_sdt_header_t)) /
      sizeof(uint32_t);

  // terminal_printf(&main_terminal, "ACPI: RSDT contains %u entries\r\n",
  // entry_count);

  for (uint32_t i = 0;
       i < entry_count && acpi_info.table_count < MAX_ACPI_TABLES; i++) {
    uint32_t table_phys = acpi_info.rsdt->sdt_pointers[i];

    uint32_t table_virt = 0;
    if (!mmu_ensure_physical_accessible_cached(
            table_phys, sizeof(acpi_sdt_header_t), &table_virt)) {
      terminal_printf(&main_terminal, "ACPI: Failed to map table %u header\r\n",
                      i);
      continue;
    }

    acpi_sdt_header_t *table_header = (acpi_sdt_header_t *)table_virt;

    // Mapear toda la tabla
    if (!mmu_ensure_physical_accessible_cached(table_phys, table_header->length,
                                               &table_virt)) {
      terminal_printf(&main_terminal,
                      "ACPI: Failed to map complete table %.4s\r\n",
                      table_header->signature);
      continue;
    }

    if (!acpi_validate_checksum(table_header, table_header->length)) {
      terminal_printf(&main_terminal,
                      "ACPI: Table %.4s has invalid checksum\r\n",
                      table_header->signature);
      continue;
    }

    acpi_info.tables[acpi_info.table_count] = (void *)table_virt;
    acpi_info.table_count++;

    // terminal_printf(&main_terminal, "ACPI: Found table %.4s (%s)\r\n",
    //                table_header->signature,
    //                acpi_get_table_name(table_header->signature));
  }
}

void acpi_parse_xsdt(void) {
  // Similar a RSDT pero con punteros de 64 bits
  uint32_t entry_count =
      (acpi_info.xsdt->header.length - sizeof(acpi_sdt_header_t)) /
      sizeof(uint64_t);

  // terminal_printf(&main_terminal, "ACPI: XSDT contains %u entries\r\n",
  // entry_count);

  for (uint32_t i = 0;
       i < entry_count && acpi_info.table_count < MAX_ACPI_TABLES; i++) {
    uint64_t table_phys_64 = acpi_info.xsdt->sdt_pointers[i];

    // Solo manejar direcciones de 32 bits por ahora
    if (table_phys_64 > 0xFFFFFFFF) {
      terminal_printf(&main_terminal,
                      "ACPI: Table %u address too high, skipping\r\n", i);
      continue;
    }

    uint32_t table_phys = (uint32_t)table_phys_64;

    uint32_t table_virt = 0;
    if (!mmu_ensure_physical_accessible_cached(
            table_phys, sizeof(acpi_sdt_header_t), &table_virt)) {
      terminal_printf(&main_terminal, "ACPI: Failed to map table %u header\r\n",
                      i);
      continue;
    }

    acpi_sdt_header_t *table_header = (acpi_sdt_header_t *)table_virt;

    // Mapear toda la tabla
    if (!mmu_ensure_physical_accessible_cached(table_phys, table_header->length,
                                               &table_virt)) {
      terminal_printf(&main_terminal,
                      "ACPI: Failed to map complete table %.4s\r\n",
                      table_header->signature);
      continue;
    }

    if (!acpi_validate_checksum(table_header, table_header->length)) {
      terminal_printf(&main_terminal,
                      "ACPI: Table %.4s has invalid checksum\r\n",
                      table_header->signature);
      continue;
    }

    acpi_info.tables[acpi_info.table_count] = (void *)table_virt;
    acpi_info.table_count++;

    // terminal_printf(&main_terminal, "ACPI: Found table %.4s (%s)\r\n",
    //                table_header->signature,
    //                acpi_get_table_name(table_header->signature));
  }
}

// ========================================================================
// GESTIÓN DE ENERGÍA
// ========================================================================

static bool acpi_parse_fadt(acpi_fadt_t *fadt) {
  // terminal_printf(&main_terminal, "ACPI: acpi_parse_fadt called with
  // fadt=%p\r\n", fadt);

  if (!fadt) {
    terminal_puts(&main_terminal, "ACPI: FADT is NULL\r\n");
    return false;
  }

  // **Validar que la FADT esté mapeada correctamente**
  uint32_t fadt_phys = mmu_virtual_to_physical((uint32_t)fadt);
  if (fadt_phys == 0) {
    terminal_puts(&main_terminal, "ACPI: ERROR - FADT virtual address not "
                                  "mapped to physical memory!\r\n");
    return false;
  }

  // Verificar que realmente sea una FADT
  if (memcmp(fadt->header.signature, "FACP", 4) != 0) {
    terminal_printf(&main_terminal, "ACPI: Invalid FADT signature: %.4s\r\n",
                    fadt->header.signature);
    return false;
  }

  // **Validar checksum de la FADT**
  if (!acpi_validate_checksum(fadt, fadt->header.length)) {
    terminal_puts(&main_terminal, "ACPI: FADT checksum invalid\r\n");
    return false;
  }

  // terminal_puts(&main_terminal, "ACPI: FADT validation passed, continuing
  // parsing...\r\n");

  // terminal_puts(&main_terminal, "ACPI: Parsing FADT...\r\n");

  acpi_pm_info_t *pm = &acpi_info.pm_info;

  // Extraer información de puertos de PM
  pm->pm1a_control_port = (uint16_t)fadt->pm1a_control_block;
  pm->pm1b_control_port = (uint16_t)fadt->pm1b_control_block;
  pm->pm1a_status_port = (uint16_t)fadt->pm1a_event_block;
  pm->pm1b_status_port = (uint16_t)fadt->pm1b_event_block;
  pm->pm2_control_port = (uint16_t)fadt->pm2_control_block;
  pm->smi_command_port = (uint16_t)fadt->smi_command_port;
  pm->acpi_enable_value = fadt->acpi_enable;
  pm->acpi_disable_value = fadt->acpi_disable;

  // terminal_printf(&main_terminal, "ACPI: PM1A Control: 0x%x, Status:
  // 0x%x\r\n",
  //                pm->pm1a_control_port, pm->pm1a_status_port);

  if (pm->pm1b_control_port) {
    // terminal_printf(&main_terminal, "ACPI: PM1B Control: 0x%x, Status:
    // 0x%x\r\n",
    //                pm->pm1b_control_port, pm->pm1b_status_port);
  }

  // terminal_printf(&main_terminal, "ACPI: SMI Command Port: 0x%x\r\n",
  // pm->smi_command_port);

  // Parsear DSDT para encontrar valores de S5
  if (fadt->dsdt_address) {
    // terminal_printf(&main_terminal, "ACPI: DSDT at 0x%08x\r\n",
    // fadt->dsdt_address); acpi_parse_dsdt_for_s5();
  }

  // Si no encontramos S5 en DSDT, usar valores por defecto
  if (pm->s5_sleep_type_a == 0 && pm->s5_sleep_type_b == 0) {
    pm->s5_sleep_type_a = 7; // Valor típico para S5
    pm->s5_sleep_type_b = 7;
    // terminal_puts(&main_terminal, "ACPI: Using default S5 sleep values
    // (7,7)\r\n");
  }
  if (fadt->header.revision >= 3) { // ACPI 2.0+ FADT revision starts at 3
    pm->reset_reg = fadt->reset_register;
    pm->reset_value = fadt->reset_value;
    // terminal_printf(&main_terminal, "ACPI: Found reset register (space_id=%u,
    // addr=0x%08x, value=0x%02x)\r\n",
    //                 pm->reset_reg.address_space_id,
    //                 (uint32_t)pm->reset_reg.address, pm->reset_value);
  }

  return true;
}

static void acpi_parse_dsdt_for_s5(void) {
  // Esta es una implementación muy simplificada
  // En un sistema real, necesitarías un parser completo de AML

  acpi_fadt_t *fadt = acpi_info.fadt;
  if (!fadt || !fadt->dsdt_address) {
    return;
  }

  uint32_t dsdt_phys = fadt->dsdt_address;

  uint32_t dsdt_virt = 0;
  if (!mmu_ensure_physical_accessible_cached(
          dsdt_phys, sizeof(acpi_sdt_header_t), &dsdt_virt)) {
    return;
  }

  acpi_sdt_header_t *dsdt_header = (acpi_sdt_header_t *)dsdt_virt;

  if (memcmp(dsdt_header->signature, "DSDT", 4) != 0) {
    return;
  }

  if (!mmu_ensure_physical_accessible_cached(dsdt_phys, dsdt_header->length,
                                             &dsdt_virt)) {
    return;
  }

  // Búsqueda muy simple de la secuencia "_S5_"
  // En un sistema real necesitarías un parser AML completo
  uint8_t *dsdt_data = (uint8_t *)dsdt_virt;
  uint32_t dsdt_size = dsdt_header->length;

  for (uint32_t i = 0; i < dsdt_size - 4; i++) {
    if (memcmp(&dsdt_data[i], "_S5_", 4) == 0) {
      // Encontrado _S5_, buscar los valores después
      // Esta es una implementación muy básica
      for (uint32_t j = i + 4; j < dsdt_size - 2 && j < i + 20; j++) {
        if (dsdt_data[j] == 0x12) { // Package op
          if (j + 4 < dsdt_size) {
            acpi_info.pm_info.s5_sleep_type_a = dsdt_data[j + 3];
            acpi_info.pm_info.s5_sleep_type_b = dsdt_data[j + 4];
            // terminal_printf(&main_terminal, "ACPI: Found S5 sleep types: %u,
            // %u\r\n", acpi_info.pm_info.s5_sleep_type_a,
            // acpi_info.pm_info.s5_sleep_type_b);
            return;
          }
        }
      }
      break;
    }
  }

  terminal_puts(&main_terminal, "ACPI: S5 sleep types not found in DSDT\r\n");
}

bool acpi_enable(void) {
  if (!acpi_info.initialized || !acpi_info.fadt) {
    terminal_puts(&main_terminal,
                  "ACPI: ACPI not initialized or FADT not found\r\n");
    return false;
  }

  acpi_pm_info_t *pm = &acpi_info.pm_info;

  // Verificar si hay puertos PM disponibles
  if (!pm->pm1a_control_port) {
    terminal_puts(&main_terminal, "ACPI: No PM1A control port available\r\n");
    return false;
  }

  // Check si ACPI ya está habilitado
  uint16_t pm1_control = inw(pm->pm1a_control_port);
  if (pm1_control & ACPI_PM1_CNT_SCI_EN) {
    pm->acpi_enabled = true;
    pm->sci_enabled = true;
    terminal_puts(&main_terminal, "ACPI: ACPI already enabled\r\n");
    return true;
  }

  // Habilitar ACPI via SMI command si está disponible
  if (pm->smi_command_port && pm->acpi_enable_value) {
    // terminal_printf(&main_terminal, "ACPI: Enabling ACPI via SMI (port 0x%x,
    // value 0x%x)\r\n",
    //                pm->smi_command_port, pm->acpi_enable_value);

    outb(pm->smi_command_port, pm->acpi_enable_value);

    // Esperar a que ACPI se habilite (timeout de 1 segundo)
    for (int i = 0; i < 100; i++) {
      pm1_control = inw(pm->pm1a_control_port);
      if (pm1_control & ACPI_PM1_CNT_SCI_EN) {
        pm->acpi_enabled = true;
        pm->sci_enabled = true;
        terminal_puts(&main_terminal,
                      "ACPI: ACPI enabled successfully via SMI\r\n");
        return true;
      }

      // Esperar ~10ms
      for (volatile int j = 0; j < 100000; j++)
        ;
    }

    terminal_puts(&main_terminal,
                  "ACPI: Timeout waiting for ACPI to enable via SMI\r\n");
  }

  // Si SMI no funciona, intentar habilitar directamente
  terminal_puts(&main_terminal, "ACPI: Attempting direct ACPI enable\r\n");
  pm1_control |= ACPI_PM1_CNT_SCI_EN;
  outw(pm->pm1a_control_port, pm1_control);

  // Verificar si funcionó
  pm1_control = inw(pm->pm1a_control_port);
  if (pm1_control & ACPI_PM1_CNT_SCI_EN) {
    pm->acpi_enabled = true;
    pm->sci_enabled = true;
    terminal_puts(&main_terminal,
                  "ACPI: ACPI enabled successfully (direct)\r\n");
    return true;
  }

  terminal_puts(&main_terminal, "ACPI: Failed to enable ACPI\r\n");
  return false;
}

bool acpi_disable(void) {
  if (!acpi_info.initialized || !acpi_info.fadt) {
    return false;
  }

  acpi_pm_info_t *pm = &acpi_info.pm_info;

  if (pm->smi_command_port && pm->acpi_disable_value) {
    outb(pm->smi_command_port, pm->acpi_disable_value);
    pm->acpi_enabled = false;
    pm->sci_enabled = false;
    return true;
  }

  return false;
}

void acpi_power_off(void) {
  // terminal_printf(&main_terminal, "ACPI: Debug - initialized=%d,
  // fadt=%p\r\n",
  //                acpi_info.initialized, acpi_info.fadt);

  if (!acpi_info.initialized) {
    boot_log_warn("ACPI: Power off failed - ACPI not initialized\r\n");
    boot_log_error();
    return;
  }

  if (!acpi_info.fadt) {
    boot_log_warn("ACPI: Power off failed - FADT not found\r\n");
    boot_log_error();
    return;
  }

  acpi_pm_info_t *pm = &acpi_info.pm_info;

  boot_log_info("ACPI: Initiating ACPI power off sequence...\r\n");

  // Verificar que tenemos los recursos necesarios
  if (!pm->pm1a_control_port) {
    boot_log_info("ACPI: No PM1A control port available for power off\r\n");
    boot_log_error();
    goto fallback_methods;
  }

  // Asegurar que ACPI está habilitado
  if (!pm->acpi_enabled) {
    boot_log_info("ACPI: Enabling ACPI for power off...\r\n");
    if (!acpi_enable()) {
      boot_log_warn("ACPI: Cannot enable ACPI for power off\r\n");
      boot_log_error();
      goto fallback_methods;
    }
  }

  // Mostrar información de debug
  boot_log_info("ACPI: Using S5 sleep types A=%u, B=%u\r\n",
                pm->s5_sleep_type_a, pm->s5_sleep_type_b);
  boot_log_info("ACPI: PM1A Control Port: 0x%x\r\n", pm->pm1a_control_port);

  // Preparar para S5 (soft power off)
  uint16_t sleep_type_a = (pm->s5_sleep_type_a << 10);
  uint16_t sleep_type_b = (pm->s5_sleep_type_b << 10);

  boot_log_info("ACPI: Writing sleep command to PM1A...\r\n");

  // Deshabilitar interrupciones antes del apagado
  __asm__ volatile("cli");

  if (pm->pm1a_control_port) {
    uint16_t pm1_control = inw(pm->pm1a_control_port);
    pm1_control &= ~ACPI_PM1_CNT_SLP_TYP; // Clear sleep type
    pm1_control |= sleep_type_a;          // Set S5 sleep type A
    pm1_control |= ACPI_PM1_CNT_SLP_EN;   // Enable sleep

    boot_log_info("ACPI: Writing 0x%04x to PM1A port 0x%x\r\n", pm1_control,
                  pm->pm1a_control_port);

    outw(pm->pm1a_control_port, pm1_control);

    // Si hay un segundo controlador PM1B, también configurarlo
    if (pm->pm1b_control_port) {
      boot_log_info("ACPI: Also writing to PM1B port 0x%x\r\n",
                    pm->pm1b_control_port);
      uint16_t pm1b_control = inw(pm->pm1b_control_port);
      pm1b_control &= ~ACPI_PM1_CNT_SLP_TYP;
      pm1b_control |= sleep_type_b;
      pm1b_control |= ACPI_PM1_CNT_SLP_EN;
      outw(pm->pm1b_control_port, pm1b_control);
    }

    // Esperar un momento para que el comando tenga efecto
    for (volatile int i = 0; i < 1000000; i++)
      ;

    // Si llegamos aquí, el apagado ACPI falló
    boot_log_warn(
        "ACPI: ACPI power off command sent but system did not power off\r\n");
    boot_log_error();
  }

fallback_methods:
  // Fallback: intentar apagado por otros métodos
  boot_log_warn("ACPI: Attempting fallback shutdown methods...\r\n");

  // Método 1: QEMU/Bochs (funciona en muchos emuladores)
  boot_log_warn("ACPI: Trying QEMU/Bochs method (port 0x604)...\r\n");
  outw(0x604, 0x2000);
  for (volatile int i = 0; i < 100000; i++)
    ;

  // Método 2: VirtualBox
  boot_log_warn("ACPI: Trying VirtualBox method (port 0x4004)...\r\n");
  outw(0x4004, 0x3400);
  for (volatile int i = 0; i < 100000; i++)
    ;

  // Si nada funciona, halt
  boot_log_warn("ACPI: All shutdown methods failed. System halted.\r\n");
  boot_log_error();
  while (1) {
    __asm__ volatile("cli; hlt");
  }
}

// Crear callback para desmontar
struct reboot_callback_data {
  int count;
  int errors;
} reboot_data = {0, 0};

void acpi_reboot(void) {
  terminal_puts(&main_terminal, "ACPI: Initiating system reboot...\r\n");
  serial_write_string(COM1_BASE, "ACPI: Initiating system reboot...\r\n");
  __asm__ volatile("cli");

  // 1. Log system state
  terminal_printf(&main_terminal, "System reboot initiated\r\n");
  serial_write_string(COM1_BASE, "System reboot initiated\r\n");

  // 2. Stop the scheduler
  if (scheduler.scheduler_enabled) {
    scheduler_stop();
    terminal_printf(&main_terminal, "Scheduler stopped\r\n");
    serial_write_string(COM1_BASE, "Scheduler stopped\r\n");
  }

  // 3. Terminate all tasks (except idle)
  task_t *current = scheduler.task_list;
  if (current) {
    do {
      task_t *next = current->next;
      if (current != scheduler.idle_task) {
        terminal_printf(&main_terminal, "Terminating task %s (ID: %u)\r\n",
                        current->name, current->task_id);
        serial_write_string(COM1_BASE, "Terminating task\r\n");
        task_destroy(current);
      }
      current = next;
    } while (current != scheduler.task_list);
  }
  task_cleanup_zombies();
  terminal_printf(&main_terminal, "All tasks terminated (except idle)\r\n");
  serial_write_string(COM1_BASE, "All tasks terminated (except idle)\r\n");

  // 4. Clean up driver system
  driver_system_cleanup();
  terminal_printf(&main_terminal, "Driver system cleaned up\r\n");
  serial_write_string(COM1_BASE, "Driver system cleaned up\r\n");

  // 5. Unmount all filesystems
  vfs_list_mounts(unmount_callback, &reboot_data);

  if (reboot_data.errors > 0) {
    terminal_printf(&main_terminal,
                    "Warning: %d filesystems failed to unmount\n",
                    reboot_data.errors);
  }
  disk_flush_dispatch(&main_disk);
  terminal_printf(&main_terminal, "All filesystems unmounted\r\n");
  serial_write_string(COM1_BASE, "All filesystems unmounted\r\n");

  // 6. Clean up modules
  module_loader_cleanup();
  terminal_printf(&main_terminal, "Modules cleaned up\r\n");
  serial_write_string(COM1_BASE, "Modules cleaned up\r\n");

  // 7. Disable PICs
  outb(PIC1_DATA, 0xFF); // Mask all IRQs on master PIC
  outb(PIC2_DATA, 0xFF); // Mask all IRQs on slave PIC
  terminal_printf(&main_terminal, "PICs disabled\r\n");
  serial_write_string(COM1_BASE, "PICs disabled\r\n");

  // 8. Brief delay to ensure all I/O completes
  for (volatile int i = 0; i < 1000000; i++)
    ;

  // 9. Attempt ACPI reset
  bool acpi_reset_success = false;
  terminal_puts(&main_terminal,
                "ACPI: Checking ACPI reset availability...\r\n");
  serial_write_string(COM1_BASE,
                      "ACPI: Checking ACPI reset availability...\r\n");

  // Log why ACPI reset might not be attempted
  if (!acpi_info.initialized) {
    terminal_puts(&main_terminal, "ACPI: Not initialized\r\n");
    serial_write_string(COM1_BASE, "ACPI: Not initialized\r\n");
  } else if (!acpi_info.fadt) {
    terminal_puts(&main_terminal, "ACPI: FADT not found\r\n");
    serial_write_string(COM1_BASE, "ACPI: FADT not found\r\n");
  } else if (acpi_info.acpi_version < 2) {
    terminal_printf(&main_terminal,
                    "ACPI: Version %u is too low (need 2.0+)\r\n",
                    acpi_info.acpi_version);
    serial_write_string(COM1_BASE, "ACPI: Version too low\r\n");
  } else if (acpi_info.fadt->header.revision < 3) {
    terminal_printf(&main_terminal,
                    "ACPI: FADT revision %u is too low (need 3+)\r\n",
                    acpi_info.fadt->header.revision);
    serial_write_string(COM1_BASE, "ACPI: FADT revision too low\r\n");
  } else {
    acpi_pm_info_t *pm = &acpi_info.pm_info;
    if (pm->reset_reg.address == 0 || pm->reset_value == 0) {
      terminal_puts(&main_terminal,
                    "ACPI: Reset register or value not set\r\n");
      serial_write_string(COM1_BASE,
                          "ACPI: Reset register or value not set\r\n");
    } else {
      terminal_puts(&main_terminal, "ACPI: Attempting ACPI reset...\r\n");
      serial_write_string(COM1_BASE, "ACPI: Attempting ACPI reset...\r\n");

      // Enable ACPI if not already enabled
      if (!pm->acpi_enabled) {
        terminal_puts(&main_terminal, "ACPI: Enabling ACPI for reset...\r\n");
        serial_write_string(COM1_BASE, "ACPI: Enabling ACPI for reset...\r\n");
        if (acpi_enable()) {
          terminal_puts(&main_terminal, "ACPI: Enabled for reset\r\n");
          serial_write_string(COM1_BASE, "ACPI: Enabled for reset\r\n");
        } else {
          terminal_puts(&main_terminal, "ACPI: Failed to enable for reset\r\n");
          serial_write_string(COM1_BASE,
                              "ACPI: Failed to enable for reset\r\n");
        }
      }

      // Perform the reset
      uint32_t addr =
          (uint32_t)pm->reset_reg.address; // Assume <4GB for 32-bit OS
      uint8_t value = pm->reset_value;
      switch (pm->reset_reg.address_space_id) {
      case 1: // System I/O
        terminal_printf(&main_terminal,
                        "ACPI: Writing 0x%02x to I/O port 0x%08x\r\n", value,
                        addr);
        serial_write_string(COM1_BASE, "ACPI: Writing to I/O port\r\n");
        switch (pm->reset_reg.access_size) {
        case 1: // Byte access
          outb(addr, value);
          break;
        case 2: // Word access
          outw(addr, (uint16_t)value);
          break;
        case 3: // DWord access
          outl(addr, (uint32_t)value);
          break;
        default:
          terminal_puts(&main_terminal,
                        "ACPI: Unsupported access size for I/O reset\r\n");
          serial_write_string(COM1_BASE, "ACPI: Unsupported access size\r\n");
          break;
        }
        acpi_reset_success = true;
        break;

      case 0: // System Memory
        terminal_printf(&main_terminal,
                        "ACPI: Writing 0x%02x to memory 0x%08x\r\n", value,
                        addr);
        serial_write_string(COM1_BASE, "ACPI: Writing to memory\r\n");
        uint32_t virt_addr = 0;
        size_t write_size = pm->reset_reg.register_bit_width / 8;
        if (write_size == 0)
          write_size = 1; // Default to byte
        if (mmu_ensure_physical_accessible_cached(addr, write_size,
                                                  &virt_addr)) {
          switch (write_size) {
          case 1:
            *(volatile uint8_t *)virt_addr = value;
            break;
          case 2:
            *(volatile uint16_t *)virt_addr = (uint16_t)value;
            break;
          case 4:
            *(volatile uint32_t *)virt_addr = (uint32_t)value;
            break;
          default:
            terminal_puts(&main_terminal,
                          "ACPI: Unsupported write size for memory reset\r\n");
            serial_write_string(COM1_BASE, "ACPI: Unsupported write size\r\n");
            break;
          }
          acpi_reset_success = true;
        } else {
          terminal_puts(&main_terminal,
                        "ACPI: Failed to map memory for reset\r\n");
          serial_write_string(COM1_BASE, "ACPI: Failed to map memory\r\n");
        }
        break;

      default:
        terminal_puts(&main_terminal,
                      "ACPI: Unsupported address space for reset\r\n");
        serial_write_string(COM1_BASE, "ACPI: Unsupported address space\r\n");
        break;
      }

      // Wait for reset to take effect (~100ms)
      for (volatile int i = 0; i < 10000000; i++)
        ;
    }
  }

  // 10. Fallback methods if ACPI reset not available or failed
  if (!acpi_reset_success) {
    terminal_puts(&main_terminal,
                  "ACPI: Falling back to legacy reboot methods...\r\n");
    serial_write_string(COM1_BASE,
                        "ACPI: Falling back to legacy reboot methods...\r\n");

    // Method 1: Keyboard controller reset
    terminal_puts(&main_terminal,
                  "ACPI: Trying keyboard controller reset...\r\n");
    serial_write_string(COM1_BASE,
                        "ACPI: Trying keyboard controller reset...\r\n");
    uint8_t good = 0x02;
    for (int i = 0; i < 1000; i++) { // Limit retries to avoid infinite loop
      good = inb(0x64);
      if (!(good & 0x02)) { // Check if output buffer is empty
        outb(0x64, 0xFE);   // Send reset command
        break;
      }
      for (volatile int j = 0; j < 10000; j++)
        ; // Small delay
    }

    // Wait for reset (~50ms)
    for (volatile int i = 0; i < 5000000; i++)
      ;

    // Method 2: Triple fault
    terminal_puts(&main_terminal, "ACPI: Trying triple fault method...\r\n");
    serial_write_string(COM1_BASE, "ACPI: Trying triple fault method...\r\n");
    __asm__ volatile("cli");

    // Load invalid IDT to cause triple fault
    struct {
      uint16_t limit;
      uint32_t base;
    } __attribute__((packed)) invalid_idt = {0, 0};

    __asm__ volatile("lidt %0" : : "m"(invalid_idt));
    __asm__ volatile("int $0x03");

    // Wait briefly (~50ms)
    for (volatile int i = 0; i < 5000000; i++)
      ;
  }
  // 11. If all methods fail, halt the system
  terminal_puts(&main_terminal,
                "ACPI: All reboot methods failed. System halted.\r\n");
  serial_write_string(COM1_BASE,
                      "ACPI: All reboot methods failed. System halted.\r\n");
  while (1) {
    __asm__ volatile("cli; hlt");
  }
}

void acpi_suspend(void) {
  if (!acpi_info.initialized || !acpi_info.fadt) {
    terminal_puts(&main_terminal, "ACPI: Suspend not available\r\n");
    return;
  }

  terminal_puts(&main_terminal,
                "ACPI: Suspend to RAM not fully implemented\r\n");
  terminal_puts(&main_terminal,
                "ACPI: This would require saving system state\r\n");

  // En un sistema real, necesitarías:
  // 1. Guardar estado de todos los drivers
  // 2. Guardar contexto del CPU
  // 3. Configurar wake events
  // 4. Entrar en estado S3

  // Por ahora, solo mostrar que la funcionalidad está disponible
  acpi_pm_info_t *pm = &acpi_info.pm_info;
  if (pm->pm1a_control_port) {
    terminal_printf(&main_terminal,
                    "ACPI: Would use PM1A port 0x%x for suspend\r\n",
                    pm->pm1a_control_port);
  }
}

// ========================================================================
// FUNCIONES DE UTILIDAD Y INFORMACIÓN
// ========================================================================

void *acpi_find_table(const char *signature) {
  if (!acpi_info.initialized || !signature) {
    return NULL;
  }

  for (uint32_t i = 0; i < acpi_info.table_count; i++) {
    acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_info.tables[i];
    if (memcmp(header->signature, signature, 4) == 0) {
      return header;
    }
  }

  return NULL;
}

void acpi_list_tables(void) {
  terminal_puts(&main_terminal, "\r\n=== ACPI Tables ===\r\n");

  if (!acpi_info.initialized) {
    terminal_puts(&main_terminal, "ACPI not initialized\r\n");
    return;
  }

  terminal_printf(&main_terminal, "ACPI Version: %u.0\r\n",
                  acpi_info.acpi_version);
  terminal_printf(&main_terminal, "Tables found: %u\r\n\r\n",
                  acpi_info.table_count);

  for (uint32_t i = 0; i < acpi_info.table_count; i++) {
    acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_info.tables[i];

    terminal_printf(&main_terminal, "%.4s: %s\r\n", header->signature,
                    acpi_get_table_name(header->signature));
    terminal_printf(&main_terminal, "  Length: %u bytes\r\n", header->length);
    terminal_printf(&main_terminal, "  Revision: %u\r\n", header->revision);
    terminal_printf(&main_terminal, "  OEM: %.6s %.8s\r\n", header->oem_id,
                    header->oem_table_id);
    terminal_puts(&main_terminal, "\r\n");
  }

  // Mostrar información de power management
  if (acpi_info.fadt) {
    terminal_puts(&main_terminal, "=== Power Management Info ===\r\n");
    acpi_pm_info_t *pm = &acpi_info.pm_info;

    terminal_printf(&main_terminal, "ACPI Enabled: %s\r\n",
                    pm->acpi_enabled ? "Yes" : "No");
    terminal_printf(&main_terminal, "SCI Enabled: %s\r\n",
                    pm->sci_enabled ? "Yes" : "No");

    if (pm->pm1a_control_port) {
      terminal_printf(&main_terminal, "PM1A Control: 0x%x\r\n",
                      pm->pm1a_control_port);
      terminal_printf(&main_terminal, "PM1A Status: 0x%x\r\n",
                      pm->pm1a_status_port);
    }

    if (pm->pm1b_control_port) {
      terminal_printf(&main_terminal, "PM1B Control: 0x%x\r\n",
                      pm->pm1b_control_port);
      terminal_printf(&main_terminal, "PM1B Status: 0x%x\r\n",
                      pm->pm1b_status_port);
    }

    if (pm->smi_command_port) {
      terminal_printf(&main_terminal, "SMI Command Port: 0x%x\r\n",
                      pm->smi_command_port);
      terminal_printf(&main_terminal, "ACPI Enable Value: 0x%x\r\n",
                      pm->acpi_enable_value);
      terminal_printf(&main_terminal, "ACPI Disable Value: 0x%x\r\n",
                      pm->acpi_disable_value);
    }

    terminal_printf(&main_terminal, "S5 Sleep Type A: %u\r\n",
                    pm->s5_sleep_type_a);
    terminal_printf(&main_terminal, "S5 Sleep Type B: %u\r\n",
                    pm->s5_sleep_type_b);
  }

  terminal_puts(&main_terminal, "\r\n");
}

const char *acpi_get_table_name(const char *signature) {
  for (int i = 0; table_names[i].signature != NULL; i++) {
    if (memcmp(signature, table_names[i].signature, 4) == 0) {
      return table_names[i].name;
    }
  }
  return "Unknown Table";
}

bool acpi_is_supported(void) {
  return acpi_info.initialized && acpi_info.rsdp != NULL;
}

uint8_t acpi_get_version(void) { return acpi_info.acpi_version; }

// ========================================================================
// FUNCIONES HELPER ADICIONALES
// ========================================================================

static uint8_t acpi_calculate_checksum(void *table, size_t length) {
  uint8_t sum = 0;
  uint8_t *bytes = (uint8_t *)table;

  for (size_t i = 0; i < length; i++) {
    sum += bytes[i];
  }

  return sum;
}