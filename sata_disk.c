#include "sata_disk.h"
#include "ahci.h"
#include "dma.h"
#include "kernel.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
// SATA disk instances
static sata_disk_t sata_disks[MAX_SATA_DISKS];
static uint32_t sata_disk_count = 0;
bool sata_initialized = false;
// ========================================================================
// INICIALIZACIÓN
// ========================================================================
bool sata_disk_init(void) {
  if (sata_initialized) {
    return true;
  }

  terminal_puts(&main_terminal, "Initializing SATA disk subsystem...\r\n");

  // Limpiar estructura de discos
  memset(sata_disks, 0, sizeof(sata_disks));
  sata_disk_count = 0;

  // Inicializar AHCI
  if (!ahci_init()) {
    terminal_puts(&main_terminal,
                  "SATA: Failed to initialize AHCI subsystem\r\n");
    return false;
  }

  // Enumerar TODOS los puertos implementados
  terminal_printf(
      &main_terminal,
      "SATA: Scanning for SATA disks (ports_implemented=0x%08x)...\r\n",
      ahci_controller.ports_implemented);

  // Nueva lógica: primero listar todos los puertos
  terminal_puts(&main_terminal, "SATA: Available ports:\r\n");
  for (uint8_t port = 0; port < 32; port++) {
    if (ahci_controller.ports_implemented & (1 << port)) {
      ahci_port_t *ahci_port = &ahci_controller.ports[port];
      const char *type_str = "Unknown";
      switch (ahci_port->device_type) {
      case 1:
        type_str = "SATA";
        break;
      case 2:
        type_str = "ATAPI";
        break;
      case 3:
        type_str = "SEMB";
        break;
      case 4:
        type_str = "Port Multiplier";
        break;
      }
      terminal_printf(&main_terminal,
                      "  Port %2u: %s (present=%d, initialized=%d)\r\n", port,
                      type_str, ahci_port->present, ahci_port->initialized);
    }
  }

  // Ahora inicializar discos SATA
  for (uint8_t port = 0; port < 32 && sata_disk_count < MAX_SATA_DISKS;
       port++) {
    // Verificar si el puerto está implementado
    if (!(ahci_controller.ports_implemented & (1 << port))) {
      continue;
    }

    ahci_port_t *ahci_port = &ahci_controller.ports[port];

    // Solo procesar discos SATA (device_type == 1) que estén presentes
    if (ahci_port->present && ahci_port->device_type == 1) {
      terminal_printf(&main_terminal,
                      "SATA: Found SATA disk on port %u, initializing...\r\n",
                      port);

      sata_disk_t *disk = &sata_disks[sata_disk_count];

      // Esperar un poco para que el disco esté listo
      for (volatile int i = 0; i < 1000000; i++)
        ;

      if (sata_disk_setup(disk, port)) {
        terminal_printf(&main_terminal,
                        "SATA: Disk %u initialized on port %u - Model: '%s', "
                        "Sectors: %llu\r\n",
                        sata_disk_count, port, disk->model, disk->sector_count);
        sata_disk_count++;
      } else {
        terminal_printf(&main_terminal,
                        "SATA: Failed to setup disk on port %u\r\n", port);
      }
    }
  }

  if (sata_disk_count == 0) {
    terminal_puts(&main_terminal, "SATA: No SATA disks found\r\n");

    // Intentar detectar discos IDE-emulados en puertos AHCI
    terminal_puts(&main_terminal,
                  "SATA: Checking for IDE-emulated disks...\r\n");
    for (uint8_t port = 0; port < 32 && sata_disk_count < MAX_SATA_DISKS;
         port++) {
      if (!(ahci_controller.ports_implemented & (1 << port))) {
        continue;
      }

      ahci_port_t *ahci_port = &ahci_controller.ports[port];
      if (ahci_port->present && ahci_port->initialized) {
        terminal_printf(&main_terminal,
                        "SATA: Port %u has device (type=%d), attempting forced "
                        "setup...\r\n",
                        port, ahci_port->device_type);

        // Intentar setup incluso si no es tipo 1
        sata_disk_t *disk = &sata_disks[sata_disk_count];
        if (sata_disk_setup(disk, port)) {
          terminal_printf(&main_terminal,
                          "SATA: Disk %u (type %d) initialized on port %u\r\n",
                          sata_disk_count, ahci_port->device_type, port);
          sata_disk_count++;
        }
      }
    }
  }

  if (sata_disk_count > 0) {
    sata_initialized = true;
    terminal_printf(&main_terminal, "SATA: Initialized %u SATA disk(s)\r\n",
                    sata_disk_count);

    // Create driver instances for the system
    for (uint32_t i = 0; i < sata_disk_count; i++) {
      char name[16];
      snprintf(name, sizeof(name), "sata%d", i);
      driver_instance_t *drv = sata_disk_driver_create(name);
      if (drv) {
        driver_init(drv, NULL);
        driver_start(drv);
      }
    }
    return true;
  }

  return false;
}

void sata_disk_cleanup(void) {
  if (!sata_initialized) {
    return;
  }
  terminal_puts(&main_terminal, "Cleaning up SATA disk subsystem...\r\n");
  // Cleanup individual disks
  for (uint32_t i = 0; i < sata_disk_count; i++) {
    if (sata_disks[i].initialized) {
      if (sata_disks[i].io_buffer) {
        dma_free_buffer(sata_disks[i].io_buffer);
      }
    }
  }
  ahci_cleanup();
  memset(sata_disks, 0, sizeof(sata_disks));
  sata_disk_count = 0;
  sata_initialized = false;
  terminal_puts(&main_terminal, "SATA: Cleanup complete\r\n");
}
bool sata_disk_setup(sata_disk_t *disk, uint8_t ahci_port) {
  if (!disk ||
      ahci_port >= 32) { // Cambiado a 32 para cubrir todos los puertos posibles
    terminal_printf(&main_terminal,
                    "SATA: Invalid parameters for disk setup\r\n");
    return false;
  }
  // Verificar que el puerto AHCI esté realmente inicializado
  ahci_port_t *ahci_port_ptr = &ahci_controller.ports[ahci_port];
  if (!ahci_port_ptr->present || !ahci_port_ptr->initialized) {
    terminal_printf(
        &main_terminal,
        "SATA: AHCI port %u not ready (present=%d, initialized=%d)\r\n",
        ahci_port, ahci_port_ptr->present, ahci_port_ptr->initialized);
    return false;
  }
  memset(disk, 0, sizeof(sata_disk_t));
  disk->ahci_port = ahci_port;
  terminal_printf(&main_terminal, "SATA: Setting up disk on port %u...\r\n",
                  ahci_port);
  // === INTENTAR SPIN-UP PRIMERO ===
  ahci_spin_up_device(ahci_port);
  // Esperar después del spin-up
  for (volatile int i = 0; i < 2000000; i++)
    ;
  // Allocar buffer de I/O para transfers
  disk->io_buffer = dma_alloc_buffer(SATA_IO_BUFFER_SIZE, 16);
  if (!disk->io_buffer) {
    terminal_printf(&main_terminal,
                    "SATA: Failed to allocate I/O buffer for port %u\r\n",
                    ahci_port);
    return false;
  }
  terminal_printf(&main_terminal,
                  "SATA: I/O buffer allocated at virt=0x%08x, phys=0x%08x\r\n",
                  disk->io_buffer->virtual_address,
                  disk->io_buffer->physical_address);
  // Identificar el dispositivo con reintentos
  uint16_t *identify_data = (uint16_t *)disk->io_buffer->virtual_address;
  bool identify_success = false;
  for (int retry = 0; retry < 3; retry++) {
    terminal_printf(&main_terminal,
                    "SATA: IDENTIFY attempt %d for port %u...\r\n", retry + 1,
                    ahci_port);
    // Limpiar el buffer primero
    memset(identify_data, 0, 512);
    if (ahci_identify_device(ahci_port, identify_data)) {
      identify_success = true;
      break;
    }
    terminal_printf(&main_terminal, "SATA: IDENTIFY failed on attempt %d\r\n",
                    retry + 1);
    // Pequeña pausa entre reintentos
    for (volatile int i = 0; i < 1000000; i++)
      ;
    if (retry == 2) {
      terminal_puts(&main_terminal,
                    "SATA: Retrying spin-up after failed attempts...\r\n");
      ahci_spin_up_device(ahci_port);
    }
  }
  if (!identify_success) {
    terminal_printf(&main_terminal,
                    "SATA: All IDENTIFY attempts failed for port %u\r\n",
                    ahci_port);
    // DEBUG: Verificar estado del puerto
    uint32_t cmd = ahci_port_ptr->port_regs->cmd;
    uint32_t ssts = ahci_port_ptr->port_regs->ssts;
    uint32_t serr = ahci_port_ptr->port_regs->serr;
    uint32_t is = ahci_port_ptr->port_regs->is;
    terminal_printf(&main_terminal,
                    "SATA: Port %u status - CMD=0x%08x, SSTS=0x%08x, "
                    "SERR=0x%08x, IS=0x%08x\r\n",
                    ahci_port, cmd, ssts, serr, is);
    dma_free_buffer(disk->io_buffer);
    return false;
  }
  terminal_puts(&main_terminal,
                "SATA: IDENTIFY successful, parsing data...\r\n");
  // Verificar que los datos de IDENTIFY sean válidos
  if (identify_data[0] == 0x0000 || identify_data[0] == 0xFFFF) {
    terminal_printf(&main_terminal,
                    "SATA: Invalid IDENTIFY data (first word=0x%04x)\r\n",
                    identify_data[0]);
    dma_free_buffer(disk->io_buffer);
    return false;
  }
  // Extraer información del dispositivo
  disk->sector_count_28 = (identify_data[61] << 16) | identify_data[60];
  // Verificar soporte LBA48
  if (identify_data[83] & (1 << 10)) {
    disk->supports_lba48 = true;
    disk->sector_count = ((uint64_t)identify_data[103] << 48) |
                         ((uint64_t)identify_data[102] << 32) |
                         ((uint64_t)identify_data[101] << 16) |
                         identify_data[100];
    terminal_printf(&main_terminal, "SATA: LBA48 supported - sectors: %llu\r\n",
                    disk->sector_count);
  } else {
    disk->supports_lba48 = false;
    disk->sector_count = disk->sector_count_28;
    terminal_printf(&main_terminal, "SATA: LBA28 only - sectors: %u\r\n",
                    disk->sector_count_28);
  }
  // Verificar otras características
  disk->supports_dma = (identify_data[49] & (1 << 8)) != 0;
  disk->supports_ncq = (identify_data[76] & (1 << 8)) != 0;
  // Extraer modelo (palabras 27-46)
  for (int i = 0; i < 20; i++) {
    uint16_t word = identify_data[27 + i];
    disk->model[i * 2] = (word >> 8) & 0xFF;
    disk->model[i * 2 + 1] = word & 0xFF;
  }
  disk->model[40] = '\0';
  // Limpiar espacios al final
  for (int i = 39; i >= 0 && (disk->model[i] == ' ' || disk->model[i] == '\0');
       i--) {
    disk->model[i] = '\0';
  }
  // Extraer número de serie (palabras 10-19)
  for (int i = 0; i < 10; i++) {
    uint16_t word = identify_data[10 + i];
    disk->serial[i * 2] = (word >> 8) & 0xFF;
    disk->serial[i * 2 + 1] = word & 0xFF;
  }
  disk->serial[20] = '\0';
  // Limpiar espacios al final
  for (int i = 19;
       i >= 0 && (disk->serial[i] == ' ' || disk->serial[i] == '\0'); i--) {
    disk->serial[i] = '\0';
  }
  disk->initialized = true;
  disk->present = true;
  terminal_printf(&main_terminal,
                  "SATA: Disk setup complete - Model: '%s', Serial: '%s'\r\n",
                  disk->model, disk->serial);
  return true;
}
// ========================================================================
// OPERACIONES DE DISCO
// ========================================================================
sata_err_t sata_disk_read(uint32_t disk_id, uint64_t lba, uint32_t count,
                          void *buffer) {
  if (!sata_initialized || disk_id >= sata_disk_count || !buffer ||
      count == 0) {
    return SATA_ERR_INVALID_PARAM;
  }
  sata_disk_t *disk = &sata_disks[disk_id];
  if (!disk->initialized || !disk->present) {
    return SATA_ERR_NOT_INITIALIZED;
  }
  if (lba + count > disk->sector_count) {
    return SATA_ERR_LBA_OUT_OF_RANGE;
  }
  // Para transfers grandes, dividir en chunks
  uint32_t sectors_per_transfer = SATA_IO_BUFFER_SIZE / SECTOR_SIZE;
  uint8_t *dest_ptr = (uint8_t *)buffer;
  uint64_t current_lba = lba;
  uint32_t remaining = count;
  while (remaining > 0) {
    uint32_t transfer_count =
        (remaining > sectors_per_transfer) ? sectors_per_transfer : remaining;
    // Usar buffer DMA interno
    if (!ahci_read_sectors(disk->ahci_port, current_lba, transfer_count,
                           disk->io_buffer->virtual_address)) {
      return SATA_ERR_IO_ERROR;
    }
    // Copiar datos al buffer de usuario
    memcpy(dest_ptr, disk->io_buffer->virtual_address,
           transfer_count * SECTOR_SIZE);
    dest_ptr += transfer_count * SECTOR_SIZE;
    current_lba += transfer_count;
    remaining -= transfer_count;
  }
  return SATA_ERR_NONE;
}
sata_err_t sata_disk_write(uint32_t disk_id, uint64_t lba, uint32_t count,
                           const void *buffer) {
  if (!sata_initialized || disk_id >= sata_disk_count || !buffer ||
      count == 0) {
    return SATA_ERR_INVALID_PARAM;
  }
  sata_disk_t *disk = &sata_disks[disk_id];
  if (!disk->initialized || !disk->present) {
    return SATA_ERR_NOT_INITIALIZED;
  }
  if (lba + count > disk->sector_count) {
    return SATA_ERR_LBA_OUT_OF_RANGE;
  }
  // Para transfers grandes, dividir en chunks
  uint32_t sectors_per_transfer = SATA_IO_BUFFER_SIZE / SECTOR_SIZE;
  const uint8_t *src_ptr = (const uint8_t *)buffer;
  uint64_t current_lba = lba;
  uint32_t remaining = count;
  while (remaining > 0) {
    uint32_t transfer_count =
        (remaining > sectors_per_transfer) ? sectors_per_transfer : remaining;
    // Copiar datos del buffer de usuario al buffer DMA
    memcpy(disk->io_buffer->virtual_address, src_ptr,
           transfer_count * SECTOR_SIZE);
    if (!ahci_write_sectors(disk->ahci_port, current_lba, transfer_count,
                            disk->io_buffer->virtual_address)) {
      return SATA_ERR_IO_ERROR;
    }
    src_ptr += transfer_count * SECTOR_SIZE;
    current_lba += transfer_count;
    remaining -= transfer_count;
  }
  return SATA_ERR_NONE;
}

sata_err_t sata_disk_flush(uint32_t disk_id) {
  if (!sata_initialized) {
    terminal_printf(&main_terminal,
                    "SATA: Flush failed - subsystem not initialized\n");
    return SATA_ERR_NOT_INITIALIZED;
  }

  if (disk_id >= sata_disk_count) {
    terminal_printf(&main_terminal,
                    "SATA: Flush failed - invalid disk ID %u (max %u)\n",
                    disk_id, sata_disk_count);
    return SATA_ERR_INVALID_PARAM;
  }

  sata_disk_t *disk = &sata_disks[disk_id];
  if (!disk->initialized || !disk->present) {
    terminal_printf(&main_terminal, "SATA: Flush failed - disk %u not ready\n",
                    disk_id);
    return SATA_ERR_NOT_INITIALIZED;
  }

  terminal_printf(&main_terminal, "SATA: Flushing disk %u (port %u)\n", disk_id,
                  disk->ahci_port);

  // Implementar FLUSH CACHE command si es necesario
  // Por ahora, es un no-op que retorna éxito
  // Nota: AHCI maneja el cache flushing automáticamente en la mayoría de los
  // casos

  return SATA_ERR_NONE;
}

// ========================================================================
// INFORMACIÓN Y UTILIDADES
// ========================================================================
uint32_t sata_disk_get_count(void) { return sata_disk_count; }
sata_disk_t *sata_disk_get_info(uint32_t disk_id) {
  if (!sata_initialized || disk_id >= sata_disk_count) {
    return NULL;
  }
  return &sata_disks[disk_id];
}
uint64_t sata_disk_get_sector_count(uint32_t disk_id) {
  if (!sata_initialized || disk_id >= sata_disk_count) {
    return 0;
  }
  sata_disk_t *disk = &sata_disks[disk_id];
  if (!disk->initialized || !disk->present) {
    return 0;
  }
  return disk->sector_count;
}
bool sata_disk_is_present(uint32_t disk_id) {
  if (!sata_initialized || disk_id >= sata_disk_count) {
    return false;
  }
  return sata_disks[disk_id].present && sata_disks[disk_id].initialized;
}
void sata_disk_list(void) {
  terminal_puts(&main_terminal, "\r\n=== SATA Disks ===\r\n");
  if (!sata_initialized) {
    terminal_puts(&main_terminal, "SATA subsystem not initialized\r\n");
    return;
  }
  if (sata_disk_count == 0) {
    terminal_puts(&main_terminal, "No SATA disks found\r\n");
    return;
  }
  for (uint32_t i = 0; i < sata_disk_count; i++) {
    sata_disk_t *disk = &sata_disks[i];
    terminal_printf(&main_terminal, "Disk %u (AHCI Port %u):\r\n", i,
                    disk->ahci_port);
    terminal_printf(&main_terminal, "  Model: %s\r\n",
                    disk->model[0] ? disk->model : "Unknown");
    terminal_printf(&main_terminal, "  Serial: %s\r\n",
                    disk->serial[0] ? disk->serial : "Unknown");
    // CORREGIR: Cálculo de capacidad
    uint64_t total_bytes = (uint64_t)disk->sector_count * 512ULL;
    uint64_t capacity_mb = total_bytes / (1024ULL * 1024ULL);
    terminal_printf(&main_terminal, "  Capacity: %llu sectors (%llu MB)\r\n",
                    disk->sector_count, capacity_mb);
    // CORREGIR: Strings de características
    terminal_printf(&main_terminal, "  Features: LBA48=%s, DMA=%s, NCQ=%s\r\n",
                    disk->supports_lba48 ? "Yes" : "No",
                    disk->supports_dma ? "Yes" : "No",
                    disk->supports_ncq ? "Yes" : "No");
    terminal_puts(&main_terminal, "\r\n");
  }
}
// ========================================================================
// INTEGRACIÓN CON SISTEMA DE DISCOS EXISTENTE
// ========================================================================
// Wrapper functions para compatibilidad con disk.h existente

disk_err_t sata_to_legacy_disk_init(disk_t *disk, uint32_t sata_disk_id) {
  terminal_printf(&main_terminal, "SATA: Attempting to bridge SATA disk %u\n",
                  sata_disk_id);

  if (!disk || sata_disk_id >= sata_disk_count || !sata_initialized) {
    terminal_printf(&main_terminal,
                    "SATA: Initialization failed: disk=%p, sata_disk_id=%u, "
                    "sata_initialized=%d\n",
                    disk, sata_disk_id, sata_initialized);
    return DISK_ERR_NOT_INITIALIZED;
  }

  sata_disk_t *sata_disk = &sata_disks[sata_disk_id];
  if (!sata_disk->present || !sata_disk->initialized) {
    terminal_printf(&main_terminal,
                    "SATA: Disk %u not present or not initialized\n",
                    sata_disk_id);
    return DISK_ERR_DEVICE_NOT_PRESENT;
  }

  // INICIALIZACIÓN COMPLETA DE LA ESTRUCTURA
  memset(disk, 0, sizeof(disk_t)); // ¡IMPORTANTE! Limpiar toda la estructura

  disk->drive_number = 0xC0 + sata_disk_id; // Nuevo rango para SATA
  disk->type = DEVICE_TYPE_SATA_DISK;
  disk->initialized = 1;
  disk->present = 1;
  disk->supports_lba48 = sata_disk->supports_lba48;
  disk->sector_count = sata_disk->sector_count;

  // INICIALIZAR CAMPOS CRÍTICOS PARA PARTICIONES
  disk->is_partition = false;     // No es una partición
  disk->partition_lba_offset = 0; // Offset cero para disco completo
  disk->physical_disk = NULL;     // No apunta a otro disco

  terminal_printf(
      &main_terminal,
      "SATA: Bridged disk %u (port %u) - sectors: %llu, LBA48: %d\n",
      sata_disk_id, sata_disk->ahci_port, disk->sector_count,
      disk->supports_lba48);

  return DISK_ERR_NONE;
}

disk_err_t sata_to_legacy_disk_read(disk_t *disk, uint64_t lba, uint32_t count,
                                    void *buffer) {
  if (!disk || !disk->initialized || disk->type != DEVICE_TYPE_SATA_DISK) {
    terminal_printf(&main_terminal,
                    "SATA: Invalid disk for read: disk=%p, init=%d, type=%d\n",
                    disk, disk ? disk->initialized : 0, disk ? disk->type : -1);
    return DISK_ERR_NOT_INITIALIZED;
  }

  // CORRECCIÓN: Calcular correctamente el ID del disco SATA (igual que en
  // write)
  uint32_t sata_disk_id = 0;

  if (disk->drive_number >= 0xC0 && disk->drive_number <= 0xCF) {
    // Nuevo rango: 0xC0-0xCF → ID 0-15
    sata_disk_id = disk->drive_number - 0xC0;
  } else if (disk->drive_number >= 0x80 && disk->drive_number <= 0x8F) {
    // Rango antiguo: 0x80-0x8F → ID 0-15
    sata_disk_id = disk->drive_number - 0x80;
  } else {
    terminal_printf(&main_terminal,
                    "SATA: Invalid drive_number for SATA: 0x%02x\n",
                    disk->drive_number);
    return DISK_ERR_INVALID_PARAM;
  }

  // terminal_printf(
  //     &main_terminal,
  //     "SATA: Read request - drive=0x%02x, disk_id=%u, LBA=%llu, count=%u\n",
  //     disk->drive_number, sata_disk_id, lba, count);

  if (sata_disk_id >= sata_disk_get_count()) {
    terminal_printf(&main_terminal, "SATA: Invalid disk ID %u (max %u)\n",
                    sata_disk_id, sata_disk_get_count());
    return DISK_ERR_INVALID_PARAM;
  }

  sata_err_t result = sata_disk_read(sata_disk_id, lba, count, buffer);

  switch (result) {
  case SATA_ERR_NONE:
    return DISK_ERR_NONE;
  case SATA_ERR_INVALID_PARAM:
    return DISK_ERR_INVALID_PARAM;
  case SATA_ERR_NOT_INITIALIZED:
    return DISK_ERR_NOT_INITIALIZED;
  case SATA_ERR_LBA_OUT_OF_RANGE:
    return DISK_ERR_LBA_OUT_OF_RANGE;
  case SATA_ERR_IO_ERROR:
    return DISK_ERR_ATA;
  default:
    terminal_printf(&main_terminal, "SATA: Unknown error %d\n", result);
    return DISK_ERR_ATA;
  }
}

disk_err_t sata_to_legacy_disk_write(disk_t *disk, uint64_t lba, uint32_t count,
                                     const void *buffer) {
  if (!disk || !disk->initialized || disk->type != DEVICE_TYPE_SATA_DISK) {
    terminal_printf(&main_terminal,
                    "SATA: Invalid disk for write: disk=%p, init=%d, type=%d\n",
                    disk, disk ? disk->initialized : 0, disk ? disk->type : -1);
    return DISK_ERR_NOT_INITIALIZED;
  }

  // CORRECCIÓN: Calcular correctamente el ID del disco SATA
  uint32_t sata_disk_id = 0;

  if (disk->drive_number >= 0xC0 && disk->drive_number <= 0xCF) {
    // Nuevo rango: 0xC0-0xCF → ID 0-15
    sata_disk_id = disk->drive_number - 0xC0;
  } else if (disk->drive_number >= 0x80 && disk->drive_number <= 0x8F) {
    // Rango antiguo: 0x80-0x8F → ID 0-15
    sata_disk_id = disk->drive_number - 0x80;
  } else {
    terminal_printf(&main_terminal,
                    "SATA: Invalid drive_number for SATA: 0x%02x\n",
                    disk->drive_number);
    return DISK_ERR_INVALID_PARAM;
  }

  // terminal_printf(
  //     &main_terminal,
  //     "SATA: Write request - drive=0x%02x, disk_id=%u, LBA=%llu, count=%u\n",
  //     disk->drive_number, sata_disk_id, lba, count);

  if (sata_disk_id >= sata_disk_get_count()) {
    terminal_printf(&main_terminal, "SATA: Invalid disk ID %u (max %u)\n",
                    sata_disk_id, sata_disk_get_count());
    return DISK_ERR_INVALID_PARAM;
  }

  // Verificar que el disco SATA existe
  sata_disk_t *sata_disk = sata_disk_get_info(sata_disk_id);
  if (!sata_disk) {
    terminal_printf(&main_terminal, "SATA: No disk info for ID %u\n",
                    sata_disk_id);
    return DISK_ERR_DEVICE_NOT_PRESENT;
  }

  if (!sata_disk->initialized || !sata_disk->present) {
    terminal_printf(&main_terminal,
                    "SATA: Disk %u not initialized (init=%d, present=%d)\n",
                    sata_disk_id, sata_disk->initialized, sata_disk->present);
    return DISK_ERR_NOT_INITIALIZED;
  }

  // Verificar límites
  if (lba + count > sata_disk->sector_count) {
    terminal_printf(&main_terminal,
                    "SATA: LBA out of range (%llu + %u > %llu)\n", lba, count,
                    sata_disk->sector_count);
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  // Escribir usando AHCI
  // terminal_printf(&main_terminal,
  //                "SATA: Calling ahci_write_sectors (port=%u, lba=%llu)\n",
  //                sata_disk->ahci_port, lba);

  bool success = ahci_write_sectors(sata_disk->ahci_port, lba, count, buffer);

  if (!success) {
    terminal_printf(&main_terminal, "SATA: ahci_write_sectors failed\n");

    // Diagnosticar el puerto AHCI
    ahci_port_t *port = &ahci_controller.ports[sata_disk->ahci_port];
    terminal_printf(&main_terminal,
                    "SATA: Port %u status - CMD=0x%08x, TFD=0x%08x, IS=0x%08x, "
                    "SSTS=0x%08x\n",
                    sata_disk->ahci_port, port->port_regs->cmd,
                    port->port_regs->tfd, port->port_regs->is,
                    port->port_regs->ssts);

    // Decodificar SSTS para diagnóstico
    uint32_t ssts = port->port_regs->ssts;
    uint8_t det = ssts & 0xF;
    uint8_t spd = (ssts >> 4) & 0xF;
    uint8_t ipm = (ssts >> 8) & 0xF;

    const char *det_str[] = {"No device", "Device present", "Phy offline",
                             "Device present & link established"};
    const char *ipm_str[] = {"No power", "Active", "Partial", "Slumber",
                             "DevSleep"};

    terminal_printf(&main_terminal,
                    "SATA: SSTS decoded - DET=%u(%s), SPD=%u, IPM=%u", det,
                    det_str[det & 0x3], spd, ipm);
    if (ipm < 5) {
      terminal_printf(&main_terminal, "(%s)\n", ipm_str[ipm]);
    } else {
      terminal_puts(&main_terminal, "\n");
    }

    return DISK_ERR_ATA;
  }

  // terminal_printf(&main_terminal, "SATA: Write successful\n");
  return DISK_ERR_NONE;
}

// Test function para verificar funcionamiento
bool sata_disk_test(uint32_t disk_id) {
  if (!sata_initialized || disk_id >= sata_disk_count) {
    return false;
  }
  terminal_printf(&main_terminal, "Testing SATA disk %u...\r\n", disk_id);
  sata_disk_t *disk = &sata_disks[disk_id];
  if (!disk->present || !disk->initialized) {
    terminal_puts(&main_terminal, "Disk not available\r\n");
    return false;
  }
  // Allocar buffers de prueba
  uint8_t *write_buffer = (uint8_t *)kernel_malloc(SECTOR_SIZE);
  uint8_t *read_buffer = (uint8_t *)kernel_malloc(SECTOR_SIZE);
  if (!write_buffer || !read_buffer) {
    terminal_puts(&main_terminal, "Failed to allocate test buffers\r\n");
    if (write_buffer)
      kernel_free(write_buffer);
    if (read_buffer)
      kernel_free(read_buffer);
    return false;
  }
  // Llenar buffer de escritura con patrón de prueba
  for (int i = 0; i < SECTOR_SIZE; i++) {
    write_buffer[i] = i & 0xFF;
  }
  // Elegir un LBA seguro para prueba (sector 1000)
  uint64_t test_lba = 1000;
  if (test_lba >= disk->sector_count) {
    terminal_puts(&main_terminal, "Disk too small for test\r\n");
    kernel_free(write_buffer);
    kernel_free(read_buffer);
    return false;
  }
  // Leer sector original primero
  uint8_t *original_buffer = (uint8_t *)kernel_malloc(SECTOR_SIZE);
  if (!original_buffer) {
    terminal_puts(&main_terminal, "Failed to allocate original buffer\r\n");
    kernel_free(write_buffer);
    kernel_free(read_buffer);
    return false;
  }
  if (sata_disk_read(disk_id, test_lba, 1, original_buffer) != SATA_ERR_NONE) {
    terminal_puts(&main_terminal, "Failed to read original sector\r\n");
    kernel_free(write_buffer);
    kernel_free(read_buffer);
    kernel_free(original_buffer);
    return false;
  }
  // Escribir patrón de prueba
  if (sata_disk_write(disk_id, test_lba, 1, write_buffer) != SATA_ERR_NONE) {
    terminal_puts(&main_terminal, "Write test failed\r\n");
    kernel_free(write_buffer);
    kernel_free(read_buffer);
    kernel_free(original_buffer);
    return false;
  }
  // Leer de vuelta
  if (sata_disk_read(disk_id, test_lba, 1, read_buffer) != SATA_ERR_NONE) {
    terminal_puts(&main_terminal, "Read test failed\r\n");
    kernel_free(write_buffer);
    kernel_free(read_buffer);
    kernel_free(original_buffer);
    return false;
  }
  // Verificar datos
  bool test_passed = true;
  for (int i = 0; i < SECTOR_SIZE; i++) {
    if (read_buffer[i] != write_buffer[i]) {
      test_passed = false;
      break;
    }
  }
  // Restaurar sector original
  sata_disk_write(disk_id, test_lba, 1, original_buffer);
  kernel_free(write_buffer);
  kernel_free(read_buffer);
  kernel_free(original_buffer);
  if (test_passed) {
    terminal_puts(&main_terminal, "SATA disk test PASSED\r\n");
  } else {
    terminal_puts(&main_terminal, "SATA disk test FAILED - data mismatch\r\n");
  }
  return test_passed;
}

void sata_disk_debug_port(uint8_t port_num) {
  terminal_printf(&main_terminal, "\r\n=== SATA Port %u Detailed Debug ===\r\n",
                  port_num);
  ahci_port_t *port = &ahci_controller.ports[port_num];
  if (!port->present) {
    terminal_puts(&main_terminal, "Port not present\r\n");
    return;
  }
  // Leer todos los registros importantes
  terminal_printf(&main_terminal, "CMD:   0x%08x\n", port->port_regs->cmd);
  terminal_printf(&main_terminal, "SSTS:  0x%08x\n", port->port_regs->ssts);
  terminal_printf(&main_terminal, "SERR:  0x%08x\n", port->port_regs->serr);
  terminal_printf(&main_terminal, "IS:    0x%08x\n", port->port_regs->is);
  terminal_printf(&main_terminal, "CI:    0x%08x\n", port->port_regs->ci);
  terminal_printf(&main_terminal, "SACT:  0x%08x\n", port->port_regs->sact);
  terminal_printf(&main_terminal, "SIG:   0x%08x\n", port->port_regs->sig);
  // Decodificar SERR
  uint32_t serr = port->port_regs->serr;
  terminal_puts(&main_terminal, "SERR decoded:\n");
  if (serr & 0x04000000)
    terminal_puts(&main_terminal, "  - Interface CRC Error\n");
  if (serr & 0x00010000)
    terminal_puts(&main_terminal, "  - Diagnostic Failure\n");
  if (serr & 0x00000100)
    terminal_puts(&main_terminal, "  - Persistent CC or CS\n");
  if (serr & 0x00000001)
    terminal_puts(&main_terminal, "  - Internal Error\n");
  // Decodificar SSTS
  uint32_t ssts = port->port_regs->ssts;
  uint8_t det = ssts & 0xF;
  uint8_t spd = (ssts >> 4) & 0xF;
  uint8_t ipm = (ssts >> 8) & 0xF;
  terminal_printf(&main_terminal, "SSTS: DET=%u, SPD=%u, IPM=%u\n", det, spd,
                  ipm);
  const char *det_str[] = {"No device", "Device present", "Phy offline",
                           "Device present & link established"};
  const char *ipm_str[] = {"No power", "Active", "Partial", "Slumber",
                           "DevSleep"};
  terminal_printf(&main_terminal, "DET: %s\n", det_str[det & 0x3]);
  if (ipm < 5)
    terminal_printf(&main_terminal, "IPM: %s\n", ipm_str[ipm]);
}

// ========================================================================
// DRIVER SYSTEM INTEGRATION
// ========================================================================
#include "driver_system.h"

static int sata_disk_driver_init(driver_instance_t *drv, void *config) {
  (void)config;
  if (!drv)
    return -1;
  if (!sata_disk_init()) {
    return -1;
  }
  return 0;
}

static int sata_disk_driver_start(driver_instance_t *drv) {
  if (!drv)
    return -1;
  terminal_printf(&main_terminal,
                  "SATA disk driver: Started. Found %u disks.\r\n",
                  sata_disk_count);
  return 0;
}

static int sata_disk_driver_stop(driver_instance_t *drv) {
  if (!drv)
    return -1;
  // No hay mucho que parar aquí, tal vez flush cache
  return 0;
}

static int sata_disk_driver_cleanup(driver_instance_t *drv) {
  if (!drv)
    return -1;
  sata_disk_cleanup();
  return 0;
}

static int sata_disk_driver_ioctl(driver_instance_t *drv, uint32_t cmd,
                                  void *arg) {
  if (!drv)
    return -1;

  switch (cmd) {
  case 0x2001: // List disks
    sata_disk_list();
    return 0;
  case 0x2002: { // Get disk count
    uint32_t *count = (uint32_t *)arg;
    if (count)
      *count = sata_disk_count;
    return 0;
  }
  default:
    return -1;
  }
}

static driver_ops_t sata_disk_driver_ops = {.init = sata_disk_driver_init,
                                            .start = sata_disk_driver_start,
                                            .stop = sata_disk_driver_stop,
                                            .cleanup = sata_disk_driver_cleanup,
                                            .ioctl = sata_disk_driver_ioctl,
                                            .load_data = NULL};

static driver_type_info_t sata_disk_driver_type = {.type = DRIVER_TYPE_SATA,
                                                   .type_name = "sata_disk",
                                                   .version = "1.0.0",
                                                   .priv_data_size = 0,
                                                   .default_ops =
                                                       &sata_disk_driver_ops,
                                                   .validate_data = NULL,
                                                   .print_info = NULL};

int sata_disk_driver_register_type(void) {
  return driver_register_type(&sata_disk_driver_type);
}

driver_instance_t *sata_disk_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_SATA, name);
}