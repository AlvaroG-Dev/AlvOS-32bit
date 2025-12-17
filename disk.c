#include "disk.h"
#include "ahci.h"
#include "atapi.h"
#include "fat32.h"
#include "io.h"
#include "irq.h"
#include "kernel.h"
#include "partition.h"
#include "sata_disk.h"
#include "string.h"
#include "terminal.h"
#include "usb_disk_wrapper.h"

// PATA (IDE) ports
#define ATA_DATA_PORT 0x1F0
#define ATA_ERROR_PORT 0x1F1
#define ATA_FEATURES_PORT 0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE_SELECT 0x1F6
#define ATA_COMMAND_PORT 0x1F7
#define ATA_STATUS_PORT 0x1F7
#define ATA_DEVCTL 0x3F6     // Device Control (write)
#define ATA_ALT_STATUS 0x3F6 // Alternate Status (read)

// ATA commands
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_SET_FEATURES 0xEF

// ATA status bits
#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_RDY 0x40
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_ERR 0x01

// Error register bits
#define ATA_ERR_ABRT 0x04 // Command aborted
#define ATA_ERR_IDNF 0x10 // ID not found
#define ATA_ERR_UNC 0x40  // Uncorrectable data error

#define ATA_SIGNATURE_LBA_MID 0x00
#define ATA_SIGNATURE_LBA_HIGH 0x00
#define ATAPI_SIGNATURE_LBA_MID 0x14
#define ATAPI_SIGNATURE_LBA_HIGH 0xEB
#define SATA_SIGNATURE_LBA_MID 0x3C
#define SATA_SIGNATURE_LBA_HIGH 0xC3

// Timeout and retries
#define DISK_TIMEOUT_MS 5000
#define DISK_RETRIES 3

#define DISK_LOCK() __asm__ __volatile__("cli")
#define DISK_UNLOCK() __asm__ __volatile__("sti")

static detected_device_t
    detected_devices[4]; // Primary master/slave, Secondary master/slave
static int detected_device_count = 0;

// Global state variables
volatile disk_op_t disk_current_op = DISK_OP_NONE;
volatile void *disk_current_buffer = NULL;
volatile uint32_t disk_remaining_sectors = 0;
volatile disk_err_t disk_error = DISK_ERR_NONE;
volatile uint32_t total_io_ticks = 0;
volatile uint64_t total_io_cycles = 0;

static void disk_reset(disk_t *disk);
static int disk_wait_ready(disk_t *disk, uint32_t start_ticks);
static int disk_wait_drq(disk_t *disk, uint32_t start_ticks);
static int disk_check_error(disk_t *disk, char *error_msg, size_t msg_size);
static int disk_check_presence(disk_t *disk);
static void pio_read16(uint16_t port, void *dst, size_t words);
static void pio_write16(uint16_t port, const void *src, size_t words);
static device_type_t disk_detect_device_type(uint8_t drive_number);
static device_type_t enhanced_disk_detect(uint8_t drive_number);
static device_type_t detect_disk_type_enhanced(uint8_t drive_number);
static device_type_t try_identify_detection(uint8_t drive_number);
static int perform_ide_initialization(disk_t *disk);

uint32_t disk_get_io_ticks(void) { return total_io_ticks; }

uint64_t disk_get_io_cycles(void) { return total_io_cycles; }

static int disk_check_timeout(uint32_t start_ticks) {
  if ((ticks_since_boot - start_ticks) > (DISK_TIMEOUT_MS / 10)) {
    return -1; // Timeout
  }
  return 0;
}

// Función mejorada para detectar tipo de dispositivo
static device_type_t detect_disk_type_enhanced(uint8_t drive_number) {
  uint16_t io_base = (drive_number < 2) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
  uint8_t drive_bit =
      (drive_number < 2) ? (drive_number << 4) : ((drive_number - 2) << 4);

  // 1. Verificar presencia básica
  outb(io_base + ATA_DRIVE_SELECT, 0xA0 | drive_bit);

  // Esperar 400ns - leer alternate status 4 veces
  uint16_t alt_status_port =
      (drive_number < 2) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
  for (int i = 0; i < 4; i++) {
    inb(alt_status_port);
  }

  // Verificar si responde
  uint8_t status = inb(io_base + ATA_STATUS_PORT);
  if (status == 0xFF || status == 0x00) {
    return DEVICE_TYPE_NONE;
  }

  // 2. Enviar DEVICE RESET y leer signature
  outb(io_base + ATA_COMMAND_PORT, 0x08); // DEVICE RESET

  // Esperar un momento
  for (volatile int i = 0; i < 5000; i++)
    ;

  // Leer signature bytes
  uint8_t lba_mid = inb(io_base + ATA_LBA_MID);
  uint8_t lba_high = inb(io_base + ATA_LBA_HIGH);

  terminal_printf(&main_terminal, "DISK: Drive %u signature: 0x%02x%02x\n",
                  drive_number, lba_mid, lba_high);

  // 3. Identificar por signature
  if (lba_mid == ATAPI_SIGNATURE_LBA_MID &&
      lba_high == ATAPI_SIGNATURE_LBA_HIGH) {
    return DEVICE_TYPE_PATAPI_CDROM;
  } else if (lba_mid == SATA_SIGNATURE_LBA_MID &&
             lba_high == SATA_SIGNATURE_LBA_HIGH) {
    // Verificar si realmente es SATA o IDE-emulado
    return DEVICE_TYPE_PATA_DISK; // Tratar como PATA primero
  } else if (lba_mid == ATA_SIGNATURE_LBA_MID &&
             lba_high == ATA_SIGNATURE_LBA_HIGH) {
    return DEVICE_TYPE_PATA_DISK;
  } else {
    // Intentar identificación más precisa con IDENTIFY
    return try_identify_detection(drive_number);
  }
}

// Función para detección mediante comando IDENTIFY
static device_type_t try_identify_detection(uint8_t drive_number) {
  uint16_t io_base = (drive_number < 2) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
  uint8_t drive_bit =
      (drive_number < 2) ? (drive_number << 4) : ((drive_number - 2) << 4);
  uint16_t alt_status_port =
      (drive_number < 2) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;

  // Seleccionar drive
  outb(io_base + ATA_DRIVE_SELECT, 0xA0 | drive_bit);

  // Esperar 400ns
  for (int i = 0; i < 4; i++) {
    inb(alt_status_port);
  }

  // Enviar IDENTIFY
  outb(io_base + ATA_COMMAND_PORT, ATA_CMD_IDENTIFY);

  // Esperar brevemente
  for (volatile int i = 0; i < 1000; i++)
    ;

  // Leer status
  uint8_t status = inb(io_base + ATA_STATUS_PORT);

  if (status == 0) {
    return DEVICE_TYPE_NONE;
  }

  // Si hay error, probablemente sea ATAPI
  if (status & ATA_STATUS_ERR) {
    uint8_t lba_mid = inb(io_base + ATA_LBA_MID);
    uint8_t lba_high = inb(io_base + ATA_LBA_HIGH);

    if (lba_mid == ATAPI_SIGNATURE_LBA_MID &&
        lba_high == ATAPI_SIGNATURE_LBA_HIGH) {
      return DEVICE_TYPE_PATAPI_CDROM;
    }
  }

  // Esperar DRQ
  int timeout = 100000;
  while (timeout-- > 0) {
    status = inb(io_base + ATA_STATUS_PORT);
    if (status & ATA_STATUS_DRQ) {
      break;
    }
    if (status & ATA_STATUS_ERR) {
      return DEVICE_TYPE_PATAPI_CDROM; // ATAPI suele dar error en IDENTIFY
    }
  }

  if (!(status & ATA_STATUS_DRQ)) {
    return DEVICE_TYPE_NONE;
  }

  // Leer datos de IDENTIFY
  uint16_t identify[256];
  for (int i = 0; i < 256; i++) {
    identify[i] = inw(io_base + ATA_DATA_PORT);
  }

  // Verificar configuración (word 0)
  uint16_t config = identify[0];

  if (config & 0x8000) {
    // Bit 15 indica dispositivo ATAPI
    return DEVICE_TYPE_PATAPI_CDROM;
  }

  // Verificar si es SATA revisando palabras específicas
  // Word 80: soporte ATA mayor
  // Word 83: capacidades soportadas
  if (identify[80] != 0) {
    // Tiene comandos ATA mayores, podría ser SATA o IDE moderno
    // Pero en modo legacy, lo tratamos como PATA
    return DEVICE_TYPE_PATA_DISK;
  }

  return DEVICE_TYPE_PATA_DISK;
}

// Función de inicialización IDE
static int perform_ide_initialization(disk_t *disk) {
  uint16_t io_base =
      (disk->drive_number < 2) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
  uint8_t drive_bit = (disk->drive_number < 2)
                          ? (disk->drive_number << 4)
                          : ((disk->drive_number - 2) << 4);
  uint16_t alt_status_port =
      (disk->drive_number < 2) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;

  // Seleccionar drive
  outb(io_base + ATA_DRIVE_SELECT, 0xA0 | drive_bit);

  // Esperar 400ns
  for (int i = 0; i < 4; i++) {
    inb(alt_status_port);
  }

  // Enviar IDENTIFY
  outb(io_base + ATA_COMMAND_PORT, ATA_CMD_IDENTIFY);

  // Esperar DRQ
  int timeout = 100000;
  uint8_t status;
  while (timeout-- > 0) {
    status = inb(io_base + ATA_STATUS_PORT);
    if (status & ATA_STATUS_DRQ)
      break;
    if (status & ATA_STATUS_ERR)
      return -1;
  }

  if (!(status & ATA_STATUS_DRQ)) {
    return -1;
  }

  // Leer datos
  uint16_t identify[256];
  for (int i = 0; i < 256; i++) {
    identify[i] = inw(io_base + ATA_DATA_PORT);
  }

  // Procesar información
  disk->supports_lba48 = (identify[83] & (1 << 10)) ? 1 : 0;

  if (disk->supports_lba48) {
    disk->sector_count = *((uint64_t *)&identify[100]);
  } else {
    disk->sector_count = *((uint32_t *)&identify[60]);
  }

  disk->initialized = 1;
  disk->present = 1;

  terminal_printf(&main_terminal,
                  "DISK: IDE disk %u initialized: %llu sectors\n",
                  disk->drive_number, disk->sector_count);

  return 0;
}

static void disk_reset(disk_t *disk) {
  outb(ATA_DEVCTL, 0x04); // Set SRST
  for (int i = 0; i < 4; i++)
    inb(ATA_ALT_STATUS);  // ~400ns delay
  outb(ATA_DEVCTL, 0x00); // Clear SRST and enable IRQs
  disk_wait_ready(disk, ticks_since_boot);
}

static int disk_wait_ready(disk_t *disk, uint32_t start_ticks) {
  uint64_t start_cycles = rdtsc();
  uint32_t loop_count = 0;
  uint32_t max_loops = 1000000;
  uint8_t status;

  do {
    status = inb(ATA_ALT_STATUS);
    loop_count++;
    if (loop_count > max_loops) {
      uint64_t cycles_used = rdtsc() - start_cycles;
      terminal_printf(
          &main_terminal,
          "disk_wait_ready: max loops exceeded (%u), cycles: %llu\r\n",
          loop_count, cycles_used);
      total_io_cycles += cycles_used;
      return -1;
    }
    if (disk_check_timeout(start_ticks) != 0) {
      uint64_t cycles_used = rdtsc() - start_cycles;
      terminal_printf(&main_terminal,
                      "disk_wait_ready timeout, loops: %u, cycles: %llu\r\n",
                      loop_count, cycles_used);
      total_io_cycles += cycles_used;
      return -1;
    }
    if (status & ATA_STATUS_ERR) {
      char error_msg[64];
      disk_check_error(disk, error_msg, sizeof(error_msg));
      uint64_t cycles_used = rdtsc() - start_cycles;
      terminal_printf(&main_terminal,
                      "disk_wait_ready error: %s, loops: %u, cycles: %llu\r\n",
                      error_msg, loop_count, cycles_used);
      total_io_cycles += cycles_used;
      return -1;
    }
    __asm__ volatile("pause");
  } while (status & ATA_STATUS_BSY);

  if (!(status & ATA_STATUS_RDY)) {
    uint64_t cycles_used = rdtsc() - start_cycles;
    terminal_printf(
        &main_terminal,
        "disk_wait_ready: drive not ready, loops: %u, cycles: %llu\r\n",
        loop_count, cycles_used);
    total_io_cycles += cycles_used;
    return -1;
  }

  uint64_t cycles_used = rdtsc() - start_cycles;
  total_io_cycles += cycles_used;
  return 0;
}

static int disk_wait_drq(disk_t *disk, uint32_t start_ticks) {
  uint64_t start_cycles = rdtsc();
  uint32_t loop_count = 0;
  uint8_t status;

  do {
    status = inb(ATA_ALT_STATUS);
    loop_count++;
    if (disk_check_timeout(start_ticks) != 0) {
      uint64_t cycles_used = rdtsc() - start_cycles;
      terminal_printf(&main_terminal,
                      "disk_wait_drq timeout, loops: %u, cycles: %llu\r\n",
                      loop_count, cycles_used);
      total_io_cycles += cycles_used;
      total_io_ticks += (ticks_since_boot - start_ticks);
      return -1;
    }
    if (status & ATA_STATUS_ERR) {
      char error_msg[64];
      disk_check_error(disk, error_msg, sizeof(error_msg));
      terminal_printf(&main_terminal,
                      "disk_wait_drq error: %s, loops: %u, ticks: %u\r\n",
                      error_msg, loop_count, ticks_since_boot - start_ticks);
      total_io_ticks += (ticks_since_boot - start_ticks);
      return -1;
    }
    __asm__ volatile("pause");
  } while (!(status & ATA_STATUS_DRQ));

  uint64_t cycles_used = rdtsc() - start_cycles;
  total_io_cycles += cycles_used;
  total_io_ticks += (ticks_since_boot - start_ticks);
  return 0;
}

static int disk_check_error(disk_t *disk, char *error_msg, size_t msg_size) {
  uint8_t status = inb(ATA_ALT_STATUS);
  if (status & ATA_STATUS_ERR) {
    uint8_t error = inb(ATA_ERROR_PORT);
    if (error & ATA_ERR_ABRT) {
      snprintf(error_msg, msg_size, "Command aborted (0x%02x)", error);
    } else if (error & ATA_ERR_IDNF) {
      snprintf(error_msg, msg_size, "ID not found (0x%02x)", error);
    } else if (error & ATA_ERR_UNC) {
      snprintf(error_msg, msg_size, "Uncorrectable data error (0x%02x)", error);
    } else {
      snprintf(error_msg, msg_size, "Unknown error (0x%02x)", error);
    }
    return -1;
  }
  error_msg[0] = '\0';
  return 0;
}

// Nueva función para detectar tipo de disco más precisa
static device_type_t enhanced_disk_detect(uint8_t drive_number) {
  // 1. Primero verificar si es un dispositivo AHCI/SATA
  if (ahci_controller.initialized) {
    // Determinar si este drive_number corresponde a un puerto AHCI
    // Mapeo simple: drive_number 0-31 → puerto AHCI 0-31
    if (drive_number < 32) {
      if (ahci_controller.ports_implemented & (1 << drive_number)) {
        ahci_port_t *port = &ahci_controller.ports[drive_number];
        if (port->present) {
          if (port->device_type == 1) {
            return DEVICE_TYPE_SATA_DISK;
          } else if (port->device_type == 2) {
            return DEVICE_TYPE_SATAPI_CDROM;
          }
        }
      }
    }
  }

  // 2. Si no es AHCI, usar detección IDE tradicional
  return detect_disk_type_enhanced(drive_number);
}

static int disk_check_presence(disk_t *disk) {
  device_type_t dev_type = detect_disk_type_enhanced(disk->drive_number);

  switch (dev_type) {
  case DEVICE_TYPE_NONE:
    terminal_printf(&main_terminal, "DISK: No device on drive %u\n",
                    disk->drive_number);
    return -1;

  case DEVICE_TYPE_PATAPI_CDROM:
    terminal_printf(&main_terminal, "DISK: ATAPI CDROM on drive %u\n",
                    disk->drive_number);
    // ATAPI se maneja por separado
    return -1;

  case DEVICE_TYPE_PATA_DISK:
    terminal_printf(&main_terminal, "DISK: PATA disk on drive %u\n",
                    disk->drive_number);
    disk->type = DEVICE_TYPE_PATA_DISK;

    // Intentar inicializar como IDE
    if (perform_ide_initialization(disk) == 0) {
      return 0;
    }

    // Si falla, podría ser SATA en modo legacy
    terminal_printf(&main_terminal,
                    "DISK: IDE init failed, checking if SATA via AHCI...\n");

    // Verificar si hay controlador AHCI activo
    if (ahci_controller.initialized) {
      // Buscar este dispositivo en puertos AHCI
      for (uint8_t port = 0; port < 32; port++) {
        if (ahci_controller.ports_implemented & (1 << port)) {
          ahci_port_t *ahci_port = &ahci_controller.ports[port];
          if (ahci_port->present && ahci_port->device_type == 1) {
            // Este es un disco SATA
            terminal_printf(&main_terminal,
                            "DISK: Found matching SATA disk on AHCI port %u\n",
                            port);
            disk->type = DEVICE_TYPE_SATA_DISK;
            // Marcar como no inicializado para IDE
            disk->initialized = 0;
            return -1; // Debe manejarse por SATA subsystem
          }
        }
      }
    }
    return -1;

  default:
    terminal_printf(&main_terminal,
                    "DISK: Unknown device type %d on drive %u\n", dev_type,
                    disk->drive_number);
    return -1;
  }
}

static device_type_t disk_detect_device_type(uint8_t drive_number) {
  // Seleccionar drive
  outb(ATA_DRIVE_SELECT, 0xA0 | (drive_number << 4));

  // Esperar 400ns
  for (int i = 0; i < 4; i++) {
    inb(ATA_ALT_STATUS);
  }

  // Verificar si existe un dispositivo
  uint8_t status = inb(ATA_STATUS_PORT);
  if (status == 0xFF || status == 0x00) {
    return DEVICE_TYPE_NONE;
  }

  // Limpiar registros
  outb(ATA_SECTOR_COUNT, 0xAB); // Valor de prueba
  outb(ATA_LBA_LOW, 0xCD);

  // Leer de vuelta para verificar
  uint8_t sc = inb(ATA_SECTOR_COUNT);
  uint8_t lba_low = inb(ATA_LBA_LOW);

  if (sc != 0xAB || lba_low != 0xCD) {
    // No hay dispositivo o no responde correctamente
    return DEVICE_TYPE_NONE;
  }

  // Enviar comando DEVICE RESET para identificación
  outb(ATA_COMMAND_PORT, 0x08); // DEVICE RESET

  // Esperar
  for (volatile int i = 0; i < 100000; i++)
    ;

  // Leer signature bytes
  uint8_t lba_mid = inb(ATA_LBA_MID);
  uint8_t lba_high = inb(ATA_LBA_HIGH);

  terminal_printf(&main_terminal,
                  "DISK: Device signature: LBA_MID=0x%02x, LBA_HIGH=0x%02x\r\n",
                  lba_mid, lba_high);

  // Identificar por signature
  if (lba_mid == ATAPI_SIGNATURE_LBA_MID &&
      lba_high == ATAPI_SIGNATURE_LBA_HIGH) {
    terminal_puts(&main_terminal, "DISK: Detected ATAPI device\r\n");
    return DEVICE_TYPE_PATAPI_CDROM;
  }

  if (lba_mid == SATA_SIGNATURE_LBA_MID &&
      lba_high == SATA_SIGNATURE_LBA_HIGH) {
    terminal_puts(&main_terminal,
                  "DISK: Detected SATA device via PATA bridge\r\n");
    return DEVICE_TYPE_SATA_DISK;
  }

  if (lba_mid == ATA_SIGNATURE_LBA_MID && lba_high == ATA_SIGNATURE_LBA_HIGH) {
    terminal_puts(&main_terminal, "DISK: Detected PATA disk\r\n");
    return DEVICE_TYPE_PATA_DISK;
  }

  terminal_printf(&main_terminal,
                  "DISK: Unknown device type (sig: 0x%02x%02x)\r\n", lba_mid,
                  lba_high);
  return DEVICE_TYPE_UNKNOWN;
}

static void pio_read16(uint16_t port, void *dst, size_t words) {
  __asm__ __volatile__("cld; rep insw"
                       : "+D"(dst), "+c"(words)
                       : "d"(port)
                       : "memory");
}

static void pio_write16(uint16_t port, const void *src, size_t words) {
  __asm__ __volatile__("cld; rep outsw"
                       : "+S"(src), "+c"(words)
                       : "d"(port)
                       : "memory");
}

static int disk_prepare_command(disk_t *disk, uint64_t lba, uint32_t count,
                                uint8_t cmd) {
  if (count == 0 || count > 255)
    return -1; // Limit to 255 sectors per operation

  DISK_LOCK();

  // Clear pending IRQ
  inb(ATA_STATUS_PORT);
  // Select drive and LBA mode
  if (disk->supports_lba48) {
    // LBA48 command
    outb(ATA_DRIVE_SELECT, 0x40 | (disk->drive_number << 4)); // LBA bit set
    for (int i = 0; i < 4; i++)
      inb(ATA_ALT_STATUS); // ~400ns delay

    // Send LBA48 parameters
    outb(ATA_SECTOR_COUNT, (count >> 8) & 0xFF); // High byte
    outb(ATA_LBA_LOW, (lba >> 24) & 0xFF);
    outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 40) & 0xFF);
    outb(ATA_SECTOR_COUNT, count & 0xFF); // Low byte
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
  } else {
    // LBA28 command
    if (lba > 0xFFFFFFFULL || lba + (uint64_t)count > 0x10000000ULL)
      return -1; // LBA28 limit
    outb(ATA_DRIVE_SELECT,
         0xE0 | (disk->drive_number << 4) | ((lba >> 24) & 0x0F));
    for (int i = 0; i < 4; i++)
      inb(ATA_ALT_STATUS); // ~400ns delay
    outb(ATA_SECTOR_COUNT, count & 0xFF);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
  }

  if (disk_wait_ready(disk, ticks_since_boot) != 0) {
    DISK_UNLOCK();
    return -1;
  }

  outb(ATA_COMMAND_PORT, cmd);
  DISK_UNLOCK();
  return 0;
}

disk_err_t disk_init(disk_t *disk, uint8_t drive_number) {
  if (!disk) {
    terminal_puts(&main_terminal, "Invalid disk pointer\r\n");
    return DISK_ERR_INVALID_PARAM;
  }

  memset(disk, 0, sizeof(disk_t));
  disk->drive_number = drive_number;

  if (disk_check_presence(disk) != 0) {
    disk->present = 0;
    return DISK_ERR_DEVICE_NOT_PRESENT;
  }
  disk->present = 1;

  // Enable IRQs
  for (int i = 0; i < 4; i++)
    inb(ATA_ALT_STATUS); // ~400ns delay

  int retries = DISK_RETRIES;
  while (retries-- > 0) {
    // Clear registers
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND_PORT, ATA_CMD_IDENTIFY);

    if (disk_wait_ready(disk, ticks_since_boot) != 0) {
      disk_reset(disk);
      continue;
    }

    if (disk_wait_drq(disk, ticks_since_boot) != 0) {
      disk_reset(disk);
      continue;
    }

    uint16_t buffer[256];
    pio_read16(ATA_DATA_PORT, buffer, 256);

    // Check if device exists and data is valid
    if (buffer[0] == 0 || buffer[0] == 0xFFFF) {
      terminal_puts(&main_terminal, "Invalid disk identification\r\n");
      disk_reset(disk);
      continue;
    }

    // Check LBA48 support (word 83, bit 10)
    disk->supports_lba48 = (buffer[83] & (1 << 10)) != 0;

    // Get sector count
    if (disk->supports_lba48) {
      disk->sector_count = *((uint64_t *)&buffer[100]); // Words 100-103
    } else {
      disk->sector_count = *((uint32_t *)&buffer[60]); // Words 60-61
    }

    disk->initialized = 1;
    disk->physical_disk = NULL;
    char msg[64];
    snprintf(msg, sizeof(msg),
             "Disk initialized: %llu sectors, LBA48: %u, drive: 0x%x\r\n",
             disk->sector_count, disk->supports_lba48, drive_number);
    terminal_puts(&main_terminal, msg);
    return DISK_ERR_NONE;
  }

  terminal_puts(&main_terminal, "Failed to initialize disk after retries\r\n");
  return DISK_ERR_ATA;
}

disk_err_t disk_init_from_partition(disk_t *partition_disk,
                                    disk_t *physical_disk,
                                    partition_info_t *partition) {
  if (!partition_disk || !physical_disk || !partition) {
    terminal_puts(&main_terminal,
                  "DISK: Invalid parameters for partition wrapper\r\n");
    return DISK_ERR_INVALID_PARAM;
  }

  if (!physical_disk->initialized || !physical_disk->present) {
    terminal_puts(&main_terminal, "DISK: Physical disk not ready\r\n");
    return DISK_ERR_NOT_INITIALIZED;
  }

  // Copiar estructura base del disco físico
  memcpy(partition_disk, physical_disk, sizeof(disk_t));

  // Configurar como partición
  partition_disk->partition_lba_offset = partition->lba_start;
  partition_disk->sector_count = partition->sector_count;
  partition_disk->is_partition = true;
  partition_disk->physical_disk = physical_disk;

  terminal_printf(
      &main_terminal,
      "DISK: Created partition wrapper - LBA offset: %llu, sectors: %llu\r\n",
      partition->lba_start, partition->sector_count);

  return DISK_ERR_NONE;
}

disk_err_t disk_read(disk_t *disk, uint64_t lba, uint32_t count, void *buffer) {
  if (!disk || !disk->initialized || !buffer || count == 0) {
    return DISK_ERR_INVALID_PARAM;
  }
  if (lba + count > disk->sector_count) {
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  uint8_t *buf = (uint8_t *)buffer;
  uint32_t sectors_done = 0;
  uint32_t global_start = ticks_since_boot;
  int retries = DISK_RETRIES;
  char error_msg[64]; // Declare error_msg here

  while (retries-- > 0 && sectors_done < count) {
    uint32_t sectors_to_process = count - sectors_done;
    if (sectors_to_process > 255)
      sectors_to_process = 255;

    uint8_t cmd =
        disk->supports_lba48 ? ATA_CMD_READ_SECTORS_EXT : ATA_CMD_READ_SECTORS;
    if (disk_prepare_command(disk, lba + sectors_done, sectors_to_process,
                             cmd) != 0) {
      disk_reset(disk);
      continue;
    }

    for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
      if (disk_wait_drq(disk, ticks_since_boot) != 0) {
        disk_reset(disk);
        break;
      }

      if (disk_check_error(disk, error_msg, sizeof(error_msg)) != 0) {
        terminal_printf(&main_terminal, "Read error: %s\r\n", error_msg);
        break;
      }

      pio_read16(ATA_DATA_PORT, buf, SECTOR_SIZE / 2);
      buf += SECTOR_SIZE;
      sectors_done++;
    }

    if (sectors_done == count &&
        disk_check_error(disk, error_msg, sizeof(error_msg)) == 0) {
      total_io_ticks += (ticks_since_boot - global_start);
      return DISK_ERR_NONE;
    }
  }

  total_io_ticks += (ticks_since_boot - global_start);
  return sectors_done == count ? DISK_ERR_NONE : DISK_ERR_ATA;
}

disk_err_t disk_write(disk_t *disk, uint64_t lba, uint32_t count,
                      const void *buffer) {
  if (!disk || !disk->initialized || !buffer || count == 0) {
    return DISK_ERR_INVALID_PARAM;
  }
  if (lba + count > disk->sector_count) {
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  const uint8_t *buf = (const uint8_t *)buffer;
  uint32_t sectors_done = 0;
  uint32_t global_start = ticks_since_boot;
  int retries = DISK_RETRIES;
  char error_msg[64]; // Declare error_msg here

  while (retries-- > 0 && sectors_done < count) {
    uint32_t sectors_to_process = count - sectors_done;
    if (sectors_to_process > 255)
      sectors_to_process = 255;

    uint8_t cmd = disk->supports_lba48 ? ATA_CMD_WRITE_SECTORS_EXT
                                       : ATA_CMD_WRITE_SECTORS;
    if (disk_prepare_command(disk, lba + sectors_done, sectors_to_process,
                             cmd) != 0) {
      disk_reset(disk);
      continue;
    }

    for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
      if (disk_wait_drq(disk, ticks_since_boot) != 0) {
        disk_reset(disk);
        break;
      }

      pio_write16(ATA_DATA_PORT, buf, SECTOR_SIZE / 2);
      buf += SECTOR_SIZE;
      sectors_done++;

      // Wait for DRQ to clear
      if (disk_wait_ready(disk, ticks_since_boot) != 0) {
        break;
      }
    }

    // Flush cache once after all sectors
    if (sectors_done == count) {
      outb(ATA_COMMAND_PORT, ATA_CMD_CACHE_FLUSH);
      if (disk_wait_ready(disk, ticks_since_boot) != 0) {
        disk_reset(disk);
        continue;
      }
    }

    if (sectors_done == count &&
        disk_check_error(disk, error_msg, sizeof(error_msg)) == 0) {
      total_io_ticks += (ticks_since_boot - global_start);
      return DISK_ERR_NONE;
    }
  }

  total_io_ticks += (ticks_since_boot - global_start);
  return sectors_done == count ? DISK_ERR_NONE : DISK_ERR_ATA;
}

disk_err_t disk_flush(disk_t *disk) {
  if (!disk || !disk_is_initialized(disk)) {
    terminal_puts(&main_terminal,
                  "DISK: Cannot flush, disk not initialized\r\n");
    return DISK_ERR_NOT_INITIALIZED;
  }

  // Seleccionar el drive
  outb(0x1F6, 0xA0 | (disk->drive_number << 4)); // Master/Slave
  io_wait();

  // Enviar comando CACHE FLUSH
  outb(0x1F7, 0xE7); // FLUSH CACHE para ATA
  io_wait();

  // Esperar a que el disco termine
  uint8_t status;
  for (int i = 0; i < 100000; i++) {
    status = inb(0x1F7);
    if (!(status & 0x80))
      break; // BSY=0 → listo
  }

  status = inb(0x1F7);
  if (status & 0x01) { // ERR=1
    terminal_puts(&main_terminal, "DISK: Error flushing disk cache\r\n");
    return DISK_ERR_ATA;
  }

  terminal_puts(&main_terminal, "DISK: Disk flush completed successfully\r\n");
  return DISK_ERR_NONE;
}

uint64_t disk_get_sector_count(disk_t *disk) {
  if (!disk || !disk->initialized) {
    return 0;
  }
  return disk->sector_count;
}

int disk_is_initialized(disk_t *disk) { return disk && disk->initialized; }

disk_err_t disk_init_atapi(disk_t *disk, uint32_t atapi_device_id) {
  if (!disk || atapi_device_id >= atapi_get_device_count()) {
    terminal_puts(&main_terminal, "DISK: Invalid ATAPI device ID\r\n");
    return DISK_ERR_INVALID_PARAM;
  }

  atapi_device_t *atapi_dev = atapi_get_device_info(atapi_device_id);
  if (!atapi_dev || !atapi_dev->present) {
    terminal_puts(&main_terminal, "DISK: ATAPI device not present\r\n");
    return DISK_ERR_DEVICE_NOT_PRESENT;
  }

  // Initialize disk structure
  memset(disk, 0, sizeof(disk_t));
  disk->drive_number = 0xE0 + atapi_device_id; // ATAPI devices start at 0xE0
  disk->type = DEVICE_TYPE_PATA_DISK;          // ATAPI uses IDE interface
  disk->initialized = 1;
  disk->present = 1;
  disk->supports_lba48 = 0; // ATAPI uses different addressing
  disk->physical_disk = NULL;

  // Check if media is present and read capacity
  if (atapi_check_media(atapi_device_id)) {
    uint32_t sector_count = 0;
    uint32_t sector_size = 0;

    if (atapi_read_capacity(atapi_device_id, &sector_count, &sector_size) ==
        ATAPI_ERR_NONE) {
      // Convert ATAPI sectors (2048 bytes) to standard sectors (512 bytes)
      disk->sector_count = (uint64_t)sector_count * (sector_size / 512);
      terminal_printf(&main_terminal,
                      "DISK: ATAPI disk initialized - %u sectors (%u MB)\r\n",
                      (uint32_t)disk->sector_count,
                      (uint32_t)((disk->sector_count * 512) / (1024 * 1024)));
    } else {
      terminal_puts(&main_terminal,
                    "DISK: ATAPI media present but capacity read failed\r\n");
      disk->sector_count = 0;
    }
  } else {
    terminal_puts(&main_terminal,
                  "DISK: ATAPI device initialized but no media present\r\n");
    disk->sector_count = 0;
  }

  return DISK_ERR_NONE;
}

bool disk_is_atapi(disk_t *disk) {
  return disk && disk->drive_number >= 0xE0 && disk->drive_number < 0xE8;
}

bool disk_atapi_media_present(disk_t *disk) {
  if (!disk_is_atapi(disk)) {
    return false;
  }

  uint32_t atapi_id = disk->drive_number - 0xE0;
  return atapi_check_media(atapi_id);
}

disk_err_t disk_atapi_eject(disk_t *disk) {
  if (!disk_is_atapi(disk)) {
    return DISK_ERR_INVALID_PARAM;
  }

  uint32_t atapi_id = disk->drive_number - 0xE0;
  atapi_err_t result = atapi_eject(atapi_id);

  switch (result) {
  case ATAPI_ERR_NONE:
    disk->sector_count = 0;
    return DISK_ERR_NONE;
  case ATAPI_ERR_INVALID_PARAM:
    return DISK_ERR_INVALID_PARAM;
  case ATAPI_ERR_NOT_INITIALIZED:
    return DISK_ERR_NOT_INITIALIZED;
  default:
    return DISK_ERR_ATAPI;
  }
}

disk_err_t disk_atapi_load(disk_t *disk) {
  if (!disk_is_atapi(disk)) {
    return DISK_ERR_INVALID_PARAM;
  }

  uint32_t atapi_id = disk->drive_number - 0xE0;
  atapi_err_t result = atapi_load(atapi_id);

  if (result == ATAPI_ERR_NONE) {
    // Re-read capacity after loading media
    uint32_t sector_count = 0;
    uint32_t sector_size = 0;

    if (atapi_read_capacity(atapi_id, &sector_count, &sector_size) ==
        ATAPI_ERR_NONE) {
      disk->sector_count = (uint64_t)sector_count * (sector_size / 512);
    }
  }

  switch (result) {
  case ATAPI_ERR_NONE:
    return DISK_ERR_NONE;
  case ATAPI_ERR_INVALID_PARAM:
    return DISK_ERR_INVALID_PARAM;
  case ATAPI_ERR_NOT_INITIALIZED:
    return DISK_ERR_NOT_INITIALIZED;
  default:
    return DISK_ERR_ATAPI;
  }
}

// Actualizar disk_read_dispatch para incluir USB:
disk_err_t disk_read_dispatch(disk_t *disk, uint64_t lba, uint32_t count,
                              void *buffer) {
  if (!disk || !disk->initialized) {
    return DISK_ERR_NOT_INITIALIZED;
  }

  // NUEVO: Aplicar offset si es partición
  uint64_t actual_lba = lba;
  if (disk->is_partition) {
    actual_lba = lba + disk->partition_lba_offset;

    // Validar límites de la partición
    if (lba + count > disk->sector_count) {
      terminal_printf(
          &main_terminal,
          "DISK: Read beyond partition bounds (LBA %llu + %u > %llu)\r\n", lba,
          count, disk->sector_count);
      return DISK_ERR_LBA_OUT_OF_RANGE;
    }
  }

  if (disk_is_atapi(disk)) {
    uint32_t atapi_id = disk->drive_number - 0xE0;

    if (!atapi_check_media(atapi_id)) {
      return DISK_ERR_ATAPI;
    }

    // ATAPI usa actual_lba con offset aplicado
    uint32_t atapi_lba = actual_lba / 4;
    uint32_t atapi_count = (count + 3) / 4;

    uint8_t *atapi_buffer = (uint8_t *)kernel_malloc(atapi_count * 2048);
    if (!atapi_buffer) {
      return DISK_ERR_ATAPI;
    }

    atapi_err_t result =
        atapi_read_sectors(atapi_id, atapi_lba, atapi_count, atapi_buffer);

    if (result == ATAPI_ERR_NONE) {
      uint32_t offset = (actual_lba % 4) * 512;
      memcpy(buffer, atapi_buffer + offset, count * 512);
      kernel_free(atapi_buffer);
      return DISK_ERR_NONE;
    }

    kernel_free(atapi_buffer);

    switch (result) {
    case ATAPI_ERR_NO_MEDIA:
      return DISK_ERR_ATAPI;
    case ATAPI_ERR_TIMEOUT:
      return DISK_ERR_TIMEOUT;
    case ATAPI_ERR_LBA_OUT_OF_RANGE:
      return DISK_ERR_LBA_OUT_OF_RANGE;
    default:
      return DISK_ERR_ATAPI;
    }
  }

  if (disk_is_usb(disk)) {
    // USB usa actual_lba directamente
    return usb_disk_read(disk, actual_lba, count, buffer);
  }

  if (disk->type == DEVICE_TYPE_SATA_DISK) {
    uint32_t sata_disk_id;

    // Determinar ID basado en drive_number
    if (disk->drive_number >= 0xC0 && disk->drive_number < 0xD0) {
      sata_disk_id = disk->drive_number - 0xC0; // Nuevo rango
    } else if (disk->drive_number >= 0x80 && disk->drive_number < 0x90) {
      sata_disk_id =
          disk->drive_number - 0x80; // Rango antiguo (backward compatible)
    } else {
      return DISK_ERR_INVALID_PARAM;
    }

    return sata_to_legacy_disk_read(disk, actual_lba, count, buffer);
  }

  // IDE: usar función directa con actual_lba
  // NOTA: No usar disk_read() recursivamente, usar la implementación real
  if (!disk || !disk->initialized || !buffer || count == 0) {
    return DISK_ERR_INVALID_PARAM;
  }

  // Bounds check for physical/legacy disk
  uint64_t limit = disk->sector_count;
  if (disk->is_partition && disk->physical_disk) {
    limit = disk->physical_disk->sector_count;
  }

  if (actual_lba + count > limit) {
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  uint8_t *buf = (uint8_t *)buffer;
  uint32_t sectors_done = 0;
  uint32_t global_start = ticks_since_boot;
  int retries = DISK_RETRIES;
  char error_msg[64];

  while (retries-- > 0 && sectors_done < count) {
    uint32_t sectors_to_process = count - sectors_done;
    if (sectors_to_process > 255)
      sectors_to_process = 255;

    uint8_t cmd =
        disk->supports_lba48 ? ATA_CMD_READ_SECTORS_EXT : ATA_CMD_READ_SECTORS;
    if (disk_prepare_command(disk, actual_lba + sectors_done,
                             sectors_to_process, cmd) != 0) {
      disk_reset(disk);
      continue;
    }

    for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
      if (disk_wait_drq(disk, ticks_since_boot) != 0) {
        disk_reset(disk);
        break;
      }

      if (disk_check_error(disk, error_msg, sizeof(error_msg)) != 0) {
        terminal_printf(&main_terminal, "Read error: %s\r\n", error_msg);
        break;
      }

      pio_read16(ATA_DATA_PORT, buf, SECTOR_SIZE / 2);
      buf += SECTOR_SIZE;
      sectors_done++;
    }

    if (sectors_done == count &&
        disk_check_error(disk, error_msg, sizeof(error_msg)) == 0) {
      total_io_ticks += (ticks_since_boot - global_start);
      return DISK_ERR_NONE;
    }
  }

  total_io_ticks += (ticks_since_boot - global_start);
  return sectors_done == count ? DISK_ERR_NONE : DISK_ERR_ATA;
}

disk_err_t disk_write_dispatch(disk_t *disk, uint64_t lba, uint32_t count,
                               const void *buffer) {
  if (!disk || !disk->initialized) {
    terminal_printf(&main_terminal,
                    "DISK: Write dispatch - disk not initialized\n");
    return DISK_ERR_NOT_INITIALIZED;
  }

  // terminal_printf(&main_terminal,
  //                 "DISK: Write dispatch called - drive=0x%02x, type=%d, "
  //                 "LBA=%llu, count=%u\n",
  //                 disk->drive_number, disk->type, lba, count);

  // ============================================================
  // 1. Aplicar offset si es partición (CON VALIDACIONES MEJORADAS)
  // ============================================================
  uint64_t actual_lba = lba;
  if (disk->is_partition) {
    // Validar que partition_lba_offset sea razonable
    if (disk->partition_lba_offset > 0xFFFFFFFF) {
      terminal_printf(&main_terminal,
                      "DISK: ERROR: partition_lba_offset too large: %llu\n",
                      disk->partition_lba_offset);
      return DISK_ERR_INVALID_PARAM;
    }

    actual_lba = lba + disk->partition_lba_offset;

    // Validación mejorada de límites de partición
    if (lba >= disk->sector_count) {
      terminal_printf(&main_terminal,
                      "DISK: ERROR: LBA %llu >= partition sector count %llu\n",
                      lba, disk->sector_count);
      return DISK_ERR_LBA_OUT_OF_RANGE;
    }

    if (lba + count > disk->sector_count) {
      terminal_printf(
          &main_terminal,
          "DISK: Write beyond partition bounds (LBA %llu + %u > %llu)\n", lba,
          count, disk->sector_count);
      return DISK_ERR_LBA_OUT_OF_RANGE;
    }

    // terminal_printf(&main_terminal,
    //                 "DISK: Partition write - offset: %llu, actual LBA:
    //                 %llu\n", disk->partition_lba_offset, actual_lba);

    // Si es partición, usar el disco físico para verificar límites globales
    if (disk->physical_disk) {
      uint64_t physical_limit = disk->physical_disk->sector_count;
      if (actual_lba + count > physical_limit) {
        terminal_printf(&main_terminal,
                        "DISK: ERROR: Write beyond physical disk bounds (%llu "
                        "+ %u > %llu)\n",
                        actual_lba, count, physical_limit);
        return DISK_ERR_LBA_OUT_OF_RANGE;
      }
    }
  }

  // ============================================================
  // 2. DETECCIÓN MEJORADA DEL TIPO DE DISCO
  // ============================================================

  // Primero verificar dispositivos especiales
  if (disk_is_atapi(disk)) {
    terminal_puts(&main_terminal,
                  "DISK: Write not supported on ATAPI device\n");
    return DISK_ERR_ATAPI;
  }

  // Verificar USB
  if (disk_is_usb(disk)) {
    terminal_printf(&main_terminal, "DISK: Routing to USB disk\n");
    return usb_disk_write(disk, actual_lba, count, buffer);
  }

  // DETECCIÓN MEJORADA DE SATA
  bool is_sata = false;
  uint32_t sata_disk_id = 0;

  // Método 1: Por tipo explícito
  if (disk->type == DEVICE_TYPE_SATA_DISK) {
    is_sata = true;
  }

  // Método 2: Por rango de drive_number (CORREGIDO)
  if (!is_sata) {
    if (disk->drive_number >= 0xC0 && disk->drive_number <= 0xCF) {
      // Nuevo rango para SATA: 0xC0-0xCF (para evitar conflictos con IDE)
      is_sata = true;
      sata_disk_id = disk->drive_number - 0xC0;
      terminal_printf(
          &main_terminal,
          "DISK: Detected SATA by drive_number 0x%02x -> ID %u (new range)\n",
          disk->drive_number, sata_disk_id);
    } else if (disk->drive_number >= 0x80 && disk->drive_number <= 0x8F) {
      // Rango antiguo para compatibilidad
      is_sata = true;
      sata_disk_id = disk->drive_number - 0x80;
      terminal_printf(
          &main_terminal,
          "DISK: Detected SATA by drive_number 0x%02x -> ID %u (old range)\n",
          disk->drive_number, sata_disk_id);
    }
  }

  // Método 3: Por subsistema AHCI si está disponible
  if (!is_sata && ahci_controller.initialized) {
    // Verificar si este drive_number corresponde a un disco AHCI
    if (sata_disk_get_count() > 0) {
      // Asumir que drive_numbers bajos son IDE y altos son SATA
      if (disk->drive_number >= 0x80) {
        is_sata = true;
        sata_disk_id = disk->drive_number & 0x0F; // Tomar bits bajos
        terminal_printf(&main_terminal,
                        "DISK: Inferred SATA from AHCI presence -> ID %u\n",
                        sata_disk_id);
      }
    }
  }

  // ============================================================
  // 3. MANEJO DE DISCOS SATA
  // ============================================================
  if (is_sata) {
    // terminal_printf(&main_terminal,
    //                 "DISK: Routing to SATA (disk_id=%u, actual_lba=%llu)\n",
    //                 sata_disk_id, actual_lba);

    // Verificar que el subsistema SATA esté inicializado
    if (!sata_initialized) {
      terminal_printf(&main_terminal, "DISK: SATA subsystem not initialized\n");
      // Fallback a IDE si SATA no está disponible
      terminal_printf(&main_terminal, "DISK: Falling back to IDE emulation\n");
      is_sata = false;
      goto ide_write_path;
    }

    // Verificar que el disco SATA existe
    if (sata_disk_id >= sata_disk_get_count()) {
      terminal_printf(&main_terminal,
                      "DISK: Invalid SATA disk ID %u (max %u)\n", sata_disk_id,
                      sata_disk_get_count());
      return DISK_ERR_INVALID_PARAM;
    }

    sata_disk_t *sata_disk = sata_disk_get_info(sata_disk_id);
    if (!sata_disk || !sata_disk->initialized || !sata_disk->present) {
      terminal_printf(
          &main_terminal,
          "DISK: SATA disk %u not available (init=%d, present=%d)\n",
          sata_disk_id, sata_disk ? sata_disk->initialized : 0,
          sata_disk ? sata_disk->present : 0);
      return DISK_ERR_NOT_INITIALIZED;
    }

    // Verificar límites del disco SATA
    if (actual_lba + count > sata_disk->sector_count) {
      terminal_printf(&main_terminal,
                      "DISK: SATA LBA out of range (%llu + %u > %llu)\n",
                      actual_lba, count, sata_disk->sector_count);
      return DISK_ERR_LBA_OUT_OF_RANGE;
    }

    // Llamar a función SATA
    disk_err_t result =
        sata_to_legacy_disk_write(disk, actual_lba, count, buffer);

    if (result == DISK_ERR_NONE) {
      // terminal_printf(&main_terminal, "DISK: SATA write successful\n");
      return DISK_ERR_NONE;
    } else {
      terminal_printf(&main_terminal, "DISK: SATA write failed with error %d\n",
                      result);

      // Diagnóstico detallado del puerto AHCI
      if (sata_disk->ahci_port < 32) {
        ahci_port_t *port = &ahci_controller.ports[sata_disk->ahci_port];
        if (port->present) {
          terminal_printf(&main_terminal,
                          "DISK: AHCI Port %u status: CMD=0x%08x, SSTS=0x%08x, "
                          "SERR=0x%08x\n",
                          sata_disk->ahci_port, port->port_regs->cmd,
                          port->port_regs->ssts, port->port_regs->serr);
        }
      }

      // NO hacer fallback automático a IDE - si es SATA, debe funcionar con
      // SATA
      return result;
    }
  }

  // ============================================================
  // 4. MANEJO DE DISCOS IDE (FALLBACK)
  // ============================================================
ide_write_path:
  terminal_printf(&main_terminal, "DISK: Using IDE write path\n");

  // Validaciones básicas
  if (!buffer || count == 0) {
    return DISK_ERR_INVALID_PARAM;
  }

  // Calcular límite correctamente
  uint64_t limit = disk->sector_count;
  if (disk->is_partition && disk->physical_disk) {
    limit = disk->physical_disk->sector_count;
  } else if (!disk->is_partition) {
    // Para discos no particionados, usar su propio límite
    limit = disk->sector_count;
  }

  // Verificación final de límites
  if (actual_lba >= limit) {
    terminal_printf(&main_terminal,
                    "DISK: ERROR: Start LBA %llu >= disk limit %llu\n",
                    actual_lba, limit);
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  if (actual_lba + count > limit) {
    terminal_printf(
        &main_terminal,
        "DISK: Write out of bounds (lba=%llu, count=%u, limit=%llu)\n",
        actual_lba, count, limit);
    return DISK_ERR_LBA_OUT_OF_RANGE;
  }

  // Implementación IDE
  const uint8_t *buf = (const uint8_t *)buffer;
  uint32_t sectors_done = 0;
  uint32_t global_start = ticks_since_boot;
  int retries = DISK_RETRIES;
  char error_msg[64];

  // terminal_printf(&main_terminal,
  //                 "DISK: Starting IDE write loop (retries=%d)\n", retries);

  while (retries-- > 0 && sectors_done < count) {
    uint32_t sectors_to_process = count - sectors_done;
    if (sectors_to_process > 255)
      sectors_to_process = 255;

    // terminal_printf(&main_terminal, "DISK: Attempt %d - sectors %u-%u of
    // %u\n",
    //                 DISK_RETRIES - retries, sectors_done,
    //                 sectors_done + sectors_to_process - 1, count);

    uint8_t cmd = disk->supports_lba48 ? ATA_CMD_WRITE_SECTORS_EXT
                                       : ATA_CMD_WRITE_SECTORS;

    // terminal_printf(&main_terminal, "DISK: Using command 0x%02X
    // (LBA48=%d)\n",
    //                 cmd, disk->supports_lba48);

    if (disk_prepare_command(disk, actual_lba + sectors_done,
                             sectors_to_process, cmd) != 0) {
      terminal_puts(&main_terminal,
                    "DISK: Prepare command failed, resetting\n");
      disk_reset(disk);
      continue;
    }

    for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
      // terminal_printf(&main_terminal, "DISK: Writing sector %llu\n",
      //                 actual_lba + sectors_done + sector);

      if (disk_wait_drq(disk, ticks_since_boot) != 0) {
        terminal_puts(&main_terminal, "DISK: DRQ wait failed, resetting\n");
        disk_reset(disk);
        break;
      }

      pio_write16(ATA_DATA_PORT, buf, SECTOR_SIZE / 2);
      buf += SECTOR_SIZE;
      sectors_done++;

      if (disk_wait_ready(disk, ticks_since_boot) != 0) {
        terminal_puts(&main_terminal, "DISK: Ready wait failed after write\n");
        break;
      }
    }

    if (sectors_done == count) {
      terminal_puts(&main_terminal,
                    "DISK: All sectors written, flushing cache\n");
      outb(ATA_COMMAND_PORT, ATA_CMD_CACHE_FLUSH);
      if (disk_wait_ready(disk, ticks_since_boot) != 0) {
        terminal_puts(&main_terminal, "DISK: Cache flush failed, resetting\n");
        disk_reset(disk);
        continue;
      }
    }

    if (sectors_done == count) {
      // Verificar errores
      if (disk_check_error(disk, error_msg, sizeof(error_msg)) == 0) {
        total_io_ticks += (ticks_since_boot - global_start);
        terminal_printf(&main_terminal, "DISK: Write successful in %u ticks\n",
                        ticks_since_boot - global_start);
        return DISK_ERR_NONE;
      } else {
        terminal_printf(&main_terminal, "DISK: Error after write: %s\n",
                        error_msg);
        // Continuar con otro intento
      }
    }
  }

  total_io_ticks += (ticks_since_boot - global_start);

  if (sectors_done == count) {
    terminal_puts(&main_terminal, "DISK: Write completed\n");
    return DISK_ERR_NONE;
  } else {
    terminal_printf(&main_terminal,
                    "DISK: Write failed - only %u of %u sectors written\n",
                    sectors_done, count);
    return DISK_ERR_ATA;
  }
}

// Actualizar disk_flush_dispatch para incluir USB:
disk_err_t disk_flush_dispatch(disk_t *disk) {
  if (!disk || !disk->initialized) {
    terminal_printf(&main_terminal,
                    "DISK: Flush dispatch - disk not initialized\n");
    return DISK_ERR_NOT_INITIALIZED;
  }

  terminal_printf(&main_terminal,
                  "DISK: Flush dispatch - drive=0x%02x, type=%d\n",
                  disk->drive_number, disk->type);

  if (disk_is_atapi(disk)) {
    // ATAPI devices don't need flushing (read-only)
    terminal_puts(&main_terminal, "DISK: ATAPI device, flush not needed\n");
    return DISK_ERR_NONE;
  }

  // USB device
  if (disk_is_usb(disk)) {
    terminal_puts(&main_terminal, "DISK: USB device flush\n");
    return usb_disk_flush(disk);
  }

  // SATA device
  if (disk->type == DEVICE_TYPE_SATA_DISK) {
    // CORRECCIÓN: Calcular correctamente el ID del disco SATA
    uint32_t sata_disk_id = 0;

    if (disk->drive_number >= 0xC0 && disk->drive_number <= 0xCF) {
      sata_disk_id = disk->drive_number - 0xC0;
    } else if (disk->drive_number >= 0x80 && disk->drive_number <= 0x8F) {
      sata_disk_id = disk->drive_number - 0x80;
    } else {
      terminal_printf(&main_terminal,
                      "DISK: Invalid drive_number for SATA flush: 0x%02x\n",
                      disk->drive_number);
      return DISK_ERR_INVALID_PARAM;
    }

    terminal_printf(&main_terminal, "DISK: SATA flush for disk ID %u\n",
                    sata_disk_id);

    sata_err_t result = sata_disk_flush(sata_disk_id);

    switch (result) {
    case SATA_ERR_NONE:
      terminal_puts(&main_terminal, "DISK: SATA flush successful\n");
      return DISK_ERR_NONE;
    case SATA_ERR_INVALID_PARAM:
      terminal_printf(&main_terminal, "DISK: Invalid SATA disk ID %u\n",
                      sata_disk_id);
      return DISK_ERR_INVALID_PARAM;
    case SATA_ERR_NOT_INITIALIZED:
      terminal_printf(&main_terminal, "DISK: SATA disk %u not initialized\n",
                      sata_disk_id);
      return DISK_ERR_NOT_INITIALIZED;
    case SATA_ERR_IO_ERROR:
      terminal_printf(&main_terminal, "DISK: SATA flush I/O error\n");
      return DISK_ERR_ATA;
    default:
      terminal_printf(&main_terminal, "DISK: Unknown SATA flush error %d\n",
                      result);
      return DISK_ERR_ATA;
    }
  }

  // IDE device (default)
  terminal_puts(&main_terminal, "DISK: IDE device flush\n");
  return disk_flush(disk);
}

void diagnose_disk_format(disk_t *disk) {
  terminal_printf(&main_terminal, "\n=== Disk Format Diagnosis ===\n");
  terminal_printf(&main_terminal, "Disk Type: ");

  if (disk->type == DEVICE_TYPE_SATA_DISK) {
    terminal_puts(&main_terminal, "SATA\n");
  } else {
    terminal_puts(&main_terminal, "IDE\n");
  }

  terminal_printf(&main_terminal, "Drive Number: 0x%02x\n", disk->drive_number);
  terminal_printf(&main_terminal, "Sector Count: %llu\n", disk->sector_count);
  terminal_printf(&main_terminal, "LBA48 Support: %s\n",
                  disk->supports_lba48 ? "Yes" : "No");

  uint8_t *boot_sector = (uint8_t *)kernel_malloc(512);
  if (!boot_sector) {
    terminal_puts(&main_terminal, "Failed to allocate boot sector buffer\n");
    return;
  }

  // Leer boot sector
  disk_err_t result = disk_read_dispatch(disk, 0, 1, boot_sector);
  if (result != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "Failed to read boot sector (error %d)\n",
                    result);
    kernel_free(boot_sector);
    return;
  }

  // Mostrar información del boot sector
  terminal_printf(&main_terminal, "Boot signature: 0x%02X%02X\n",
                  boot_sector[510], boot_sector[511]);

  // Mostrar OEM name
  terminal_printf(&main_terminal, "OEM Name: ");
  for (int i = 3; i < 11; i++) {
    if (boot_sector[i] >= 32 && boot_sector[i] <= 126) {
      terminal_putchar(&main_terminal, boot_sector[i]);
    } else {
      terminal_putchar(&main_terminal, '?');
    }
  }
  terminal_puts(&main_terminal, "\n");

  // Mostrar FS type
  terminal_printf(&main_terminal, "FS Type: ");
  for (int i = 82; i < 90; i++) {
    if (boot_sector[i] >= 32 && boot_sector[i] <= 126) {
      terminal_putchar(&main_terminal, boot_sector[i]);
    } else {
      terminal_putchar(&main_terminal, '?');
    }
  }
  terminal_puts(&main_terminal, "\n");

  if (check_fat32_signature(boot_sector)) {
    terminal_puts(&main_terminal, "Disk appears to have FAT32 filesystem\n");

    uint16_t bytes_per_sector = *(uint16_t *)&boot_sector[11];
    uint8_t sectors_per_cluster = boot_sector[13];
    uint32_t sectors_per_fat = *(uint32_t *)&boot_sector[36];

    terminal_printf(&main_terminal, "Bytes per sector: %u\n", bytes_per_sector);
    terminal_printf(&main_terminal, "Sectors per cluster: %u\n",
                    sectors_per_cluster);
    terminal_printf(&main_terminal, "Sectors per FAT: %u\n", sectors_per_fat);
  } else {
    terminal_puts(&main_terminal,
                  "Disk does not appear to have FAT32 filesystem\n");
  }

  terminal_printf(&main_terminal, "\n=== Trying Partition Read ===\n");

  // Leer partition table
  partition_table_t pt;
  part_err_t perr = partition_read_table(&main_disk, &pt);
  if (perr != PART_OK) {
    terminal_printf(&main_terminal, "ERROR: partition_read_table failed: %d\n",
                    perr);
    return;
  }

  terminal_printf(&main_terminal, "Partition count: %u\n", pt.partition_count);

  if (pt.partition_count > 0) {
    partition_info_t *p = &pt.partitions[0];
    terminal_printf(&main_terminal, "Partition 0:\n");
    terminal_printf(&main_terminal, "  LBA Start: %u\n", p->lba_start);
    terminal_printf(&main_terminal, "  Sector Count: %u\n", p->sector_count);
    terminal_printf(&main_terminal, "  Type: 0x%02X\n", p->type);

    // Leer VBR de la partición
    uint8_t *vbr_buf = (uint8_t *)kernel_malloc(512);
    if (vbr_buf) {
      disk_err_t derr =
          disk_read_dispatch(&main_disk, p->lba_start, 1, vbr_buf);
      if (derr == DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "\nVBR first 64 bytes:\n");
        for (int i = 0; i < 64; i += 16) {
          terminal_printf(&main_terminal, "%04x: ", i);
          for (int j = 0; j < 16; j++) {
            terminal_printf(&main_terminal, "%02x ", vbr_buf[i + j]);
          }
          terminal_printf(&main_terminal, "\n");
        }

        // Check FAT32 signature
        if (check_fat32_signature(vbr_buf)) {
          terminal_printf(&main_terminal, "Valid FAT32 signature found!\n");
        } else {
          terminal_printf(&main_terminal, "No valid FAT32 signature\n");
        }
      } else {
        terminal_printf(&main_terminal,
                        "ERROR: Failed to read VBR (error %d)\n", derr);
      }
      kernel_free(vbr_buf);
    }
  }

  kernel_free(boot_sector);
}

void disk_scan_all_buses(void) {
  terminal_puts(&main_terminal, "\r\n=== Scanning all IDE/SATA buses ===\r\n");

  detected_device_count = 0;
  memset(detected_devices, 0, sizeof(detected_devices));

  const char *bus_names[] = {"Primary", "Secondary"};
  const char *drive_names[] = {"Master", "Slave"};

  // Escanear primary y secondary bus
  for (uint8_t bus = 0; bus < 2; bus++) {
    uint16_t io_base = (bus == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;

    terminal_printf(&main_terminal, "\r\nScanning %s bus (0x%03x):\r\n",
                    bus_names[bus], io_base);

    for (uint8_t drive = 0; drive < 2; drive++) {
      terminal_printf(&main_terminal, "  %s: ", drive_names[drive]);

      uint8_t drive_number = (bus << 1) | drive;
      device_type_t dev_type = enhanced_disk_detect(drive_number);

      detected_device_t *dev = &detected_devices[detected_device_count];
      dev->bus = bus;
      dev->drive = drive;
      dev->type = dev_type;
      dev->present = (dev_type != DEVICE_TYPE_NONE);

      switch (dev_type) {
      case DEVICE_TYPE_NONE:
        terminal_puts(&main_terminal, "Not present\r\n");
        snprintf(dev->description, sizeof(dev->description), "Empty");
        break;

      case DEVICE_TYPE_PATA_DISK:
        terminal_puts(&main_terminal, "PATA Hard Disk\r\n");
        snprintf(dev->description, sizeof(dev->description), "PATA Disk");
        detected_device_count++;
        break;

      case DEVICE_TYPE_PATAPI_CDROM:
        terminal_puts(&main_terminal, "ATAPI CD/DVD Drive\r\n");
        snprintf(dev->description, sizeof(dev->description), "ATAPI CDROM");
        detected_device_count++;
        break;

      case DEVICE_TYPE_SATA_DISK:
        terminal_puts(&main_terminal,
                      "SATA Disk (legacy mode) - Should use AHCI!\r\n");
        snprintf(dev->description, sizeof(dev->description), "SATA via Bridge");
        detected_device_count++;
        break;

      case DEVICE_TYPE_SATAPI_CDROM:
        terminal_puts(&main_terminal, "SATA ATAPI CD/DVD Drive\r\n");
        snprintf(dev->description, sizeof(dev->description), "SATAPI CDROM");
        detected_device_count++;
        break;

      default:
        terminal_puts(&main_terminal, "Unknown device type\r\n");
        snprintf(dev->description, sizeof(dev->description), "Unknown");
        break;
      }
    }
  }

  terminal_printf(&main_terminal, "\r\nTotal devices detected: %d\r\n",
                  detected_device_count);
}

// ====================================================================
// NUEVA FUNCIÓN: Listar todos los dispositivos detectados
// ====================================================================

void disk_list_detected_devices(void) {
  terminal_puts(&main_terminal, "\r\n=== Detected Storage Devices ===\r\n");

  if (detected_device_count == 0) {
    terminal_puts(&main_terminal, "No devices detected on IDE buses\r\n");
  } else {
    const char *bus_names[] = {"Primary", "Secondary"};
    const char *drive_names[] = {"Master", "Slave"};

    for (int i = 0; i < 4; i++) {
      if (detected_devices[i].present) {
        terminal_printf(&main_terminal, "  %s %s: %s\r\n",
                        bus_names[detected_devices[i].bus],
                        drive_names[detected_devices[i].drive],
                        detected_devices[i].description);
      }
    }
  }

  // Listar dispositivos AHCI/SATA nativos
  if (sata_disk_get_count() > 0) {
    terminal_puts(&main_terminal, "\r\n=== AHCI SATA Devices ===\r\n");
    for (uint32_t i = 0; i < sata_disk_get_count(); i++) {
      sata_disk_t *disk = sata_disk_get_info(i);
      if (disk) {
        terminal_printf(&main_terminal, "  SATA %u (Port %u): %s\r\n", i,
                        disk->ahci_port,
                        disk->model[0] ? disk->model : "Unknown");
      }
    }
  }

  // Listar dispositivos ATAPI
  if (atapi_get_device_count() > 0) {
    terminal_puts(&main_terminal, "\r\n=== ATAPI Devices ===\r\n");
    for (uint32_t i = 0; i < atapi_get_device_count(); i++) {
      atapi_device_t *dev = atapi_get_device_info(i);
      if (dev && dev->present) {
        terminal_printf(&main_terminal, "  ATAPI %u: %s\r\n", i,
                        dev->model[0] ? dev->model : "Unknown");
      }
    }
  }

  terminal_puts(&main_terminal, "\r\n");
}

void cmd_lsblk(void) {
  terminal_puts(&main_terminal, "\r\n=== Block Devices ===\r\n\r\n");

  // SATA Disks
  if (sata_disk_get_count() > 0) {
    terminal_puts(&main_terminal, "SATA Disks:\r\n");
    for (uint32_t i = 0; i < sata_disk_get_count(); i++) {
      sata_disk_t *disk = sata_disk_get_info(i);
      if (disk && disk->present) {
        uint64_t size_mb = (disk->sector_count * 512) / (1024 * 1024);
        terminal_printf(&main_terminal, "  sata%u: %llu MB - %s (Port %u)\r\n",
                        i, size_mb, disk->model, disk->ahci_port);
      }
    }
    terminal_puts(&main_terminal, "\r\n");
  }

  // IDE Disks
  terminal_puts(&main_terminal, "IDE Devices:\r\n");
  disk_list_detected_devices();

  // ATAPI Devices
  if (atapi_get_device_count() > 0) {
    terminal_puts(&main_terminal, "\r\nATAPI Devices:\r\n");
    for (uint32_t i = 0; i < atapi_get_device_count(); i++) {
      atapi_device_t *dev = atapi_get_device_info(i);
      if (dev && dev->present) {
        terminal_printf(&main_terminal, "  atapi%u: %s %s\r\n", i, dev->model,
                        dev->media_present ? "(media present)" : "(no media)");
      }
    }
  }

  terminal_puts(&main_terminal, "\r\n");
}