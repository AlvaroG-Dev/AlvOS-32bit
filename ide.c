// ide.c - Driver de discos IDE/ATA
#include "ide.h"
#include "io.h"
#include "kernel.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "driver_system.h"

extern Terminal main_terminal;

// Estado global del driver IDE
static ide_driver_priv_t *ide_priv = NULL;

// ========================================================================
// FUNCIONES DE BAJO NIVEL
// ========================================================================

void ide_select_drive(ide_disk_t *disk) {
    uint8_t drive_select = 0xA0 | (disk->drive << 4);
    outb(disk->io_base + ATA_DRIVE_SELECT, drive_select);
}

void ide_400ns_delay(ide_disk_t *disk) {
    for (int i = 0; i < 4; i++) {
        inb(disk->io_ctrl + ATA_ALT_STATUS);
    }
}

uint8_t ide_read_status(ide_disk_t *disk) {
    return inb(disk->io_base + ATA_STATUS_PORT);
}

int ide_wait_ready(ide_disk_t *disk) {
    uint32_t timeout = 1000000; // ~1 segundo
    
    while (timeout--) {
        uint8_t status = ide_read_status(disk);
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_RDY)) {
            return 0;
        }
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        __asm__ volatile("pause");
    }
    
    terminal_printf(&main_terminal, "IDE: Timeout waiting for drive ready\r\n");
    return -1;
}

int ide_wait_drq(ide_disk_t *disk) {
    uint32_t timeout = 1000000;
    
    while (timeout--) {
        uint8_t status = ide_read_status(disk);
        if (status & ATA_STATUS_DRQ) {
            return 0;
        }
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        __asm__ volatile("pause");
    }
    
    terminal_printf(&main_terminal, "IDE: Timeout waiting for DRQ\r\n");
    return -1;
}

int ide_check_error(ide_disk_t *disk, char *error_msg, size_t msg_size) {
    uint8_t status = ide_read_status(disk);
    if (status & ATA_STATUS_ERR) {
        uint8_t error = inb(disk->io_base + ATA_ERROR_PORT);
        if (error_msg) {
            if (error & ATA_ERR_ABRT) {
                snprintf(error_msg, msg_size, "Command aborted (0x%02x)", error);
            } else if (error & ATA_ERR_IDNF) {
                snprintf(error_msg, msg_size, "ID not found (0x%02x)", error);
            } else if (error & ATA_ERR_UNC) {
                snprintf(error_msg, msg_size, "Uncorrectable data error (0x%02x)", error);
            } else {
                snprintf(error_msg, msg_size, "Unknown error (0x%02x)", error);
            }
        }
        return -1;
    }
    return 0;
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

// ========================================================================
// DETECCIÓN DE DISPOSITIVOS - CORREGIDO
// ========================================================================

ide_device_type_t ide_detect_device_type(uint8_t bus, uint8_t drive) {
    uint16_t io_base = (bus == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    uint16_t ctrl_port = (bus == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    uint8_t drive_select = 0xA0 | (drive << 4);
    
    terminal_printf(&main_terminal, "IDE: Detecting bus %u, drive %u (io_base=0x%03x)\r\n", 
                    bus, drive, io_base);
    
    // 1. Seleccionar drive
    outb(io_base + ATA_DRIVE_SELECT, drive_select);
    
    // 2. Esperar 400ns (leer alternate status 4 veces)
    for (int i = 0; i < 4; i++) {
        inb(ctrl_port + ATA_ALT_STATUS);
    }
    
    // 3. Verificar si el dispositivo responde
    uint8_t status = inb(io_base + ATA_STATUS_PORT);
    terminal_printf(&main_terminal, "IDE: Initial status: 0x%02x\r\n", status);
    
    if (status == 0xFF || status == 0x00) {
        terminal_puts(&main_terminal, "IDE: No device responding\r\n");
        return IDE_TYPE_NONE;
    }
    
    // 4. Enviar DEVICE RESET (0x08) para limpiar el estado
    outb(io_base + ATA_COMMAND_PORT, 0x08); // DEVICE RESET
    
    // 5. Esperar un momento (más tiempo para dispositivos lentos)
    for (volatile int i = 0; i < 100000; i++);
    
    // 6. Leer signature bytes (LBA_MID y LBA_HIGH)
    uint8_t lba_mid = inb(io_base + ATA_LBA_MID);
    uint8_t lba_high = inb(io_base + ATA_LBA_HIGH);
    
    terminal_printf(&main_terminal, "IDE: Signature bytes: LBA_MID=0x%02x, LBA_HIGH=0x%02x\r\n",
                    lba_mid, lba_high);
    
    // 7. Identificar por signature (CORREGIDO)
    if (lba_mid == ATAPI_SIGNATURE_LBA_MID && lba_high == ATAPI_SIGNATURE_LBA_HIGH) {
        terminal_puts(&main_terminal, "  -> ATAPI CD/DVD device (0x14EB)\r\n");
        return IDE_TYPE_PATAPI_CDROM;
    } else if (lba_mid == ATA_SIGNATURE_LBA_MID && lba_high == ATA_SIGNATURE_LBA_HIGH) {
        terminal_puts(&main_terminal, "  -> ATA disk device (0x0000)\r\n");
        return IDE_TYPE_PATA_DISK;
    } else if (lba_mid == SATA_SIGNATURE_LBA_MID && lba_high == SATA_SIGNATURE_LBA_HIGH) {
        terminal_puts(&main_terminal, "  -> SATA device in legacy mode (0x3CC3)\r\n");
        return IDE_TYPE_PATA_DISK; // Tratar como ATA por ahora
    } else {
        // Intentar identificación más profunda
        terminal_printf(&main_terminal, "  -> Unknown signature, trying IDENTIFY...\r\n");
        
        // 8. Intentar comando IDENTIFY para determinar tipo
        outb(io_base + ATA_DRIVE_SELECT, drive_select);
        for (int i = 0; i < 4; i++) inb(ctrl_port + ATA_ALT_STATUS);
        
        outb(io_base + ATA_COMMAND_PORT, ATA_CMD_IDENTIFY);
        
        // Esperar brevemente
        for (volatile int i = 0; i < 5000; i++);
        
        status = inb(io_base + ATA_STATUS_PORT);
        
        if (status == 0) {
            terminal_puts(&main_terminal, "  -> No response to IDENTIFY\r\n");
            return IDE_TYPE_NONE;
        }
        
        // Esperar DRQ o error
        int timeout = 100000;
        while (timeout-- > 0) {
            status = inb(io_base + ATA_STATUS_PORT);
            if (status & ATA_STATUS_DRQ) break;
            if (status & ATA_STATUS_ERR) {
                // Error probablemente indica ATAPI
                terminal_puts(&main_terminal, "  -> ATAPI (error on IDENTIFY)\r\n");
                return IDE_TYPE_PATAPI_CDROM;
            }
        }
        
        if (status & ATA_STATUS_DRQ) {
            terminal_puts(&main_terminal, "  -> ATA disk (DRQ on IDENTIFY)\r\n");
            return IDE_TYPE_PATA_DISK;
        }
        
        terminal_puts(&main_terminal, "  -> Unknown device type\r\n");
        return IDE_TYPE_UNKNOWN;
    }
}

bool ide_identify_device(ide_disk_t *disk) {
    uint16_t io_base = disk->io_base;
    
    terminal_printf(&main_terminal, "IDE: Identifying device at 0x%03x...\r\n", io_base);
    
    // 1. Seleccionar drive
    ide_select_drive(disk);
    ide_400ns_delay(disk);
    
    // 2. Enviar comando IDENTIFY
    outb(io_base + ATA_COMMAND_PORT, ATA_CMD_IDENTIFY);
    
    // 3. Esperar a que no esté busy
    int timeout = 1000000;
    uint8_t status;
    
    while (timeout-- > 0) {
        status = inb(io_base + ATA_STATUS_PORT);
        if (!(status & ATA_STATUS_BSY)) break;
    }
    
    if (status & ATA_STATUS_BSY) {
        terminal_puts(&main_terminal, "IDE: Timeout waiting for BSY clear\r\n");
        return false;
    }
    
    // 4. Verificar error
    if (status & ATA_STATUS_ERR) {
        terminal_puts(&main_terminal, "IDE: Error after IDENTIFY command\r\n");
        return false;
    }
    
    // 5. Esperar DRQ
    timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(io_base + ATA_STATUS_PORT);
        if (status & ATA_STATUS_DRQ) break;
        if (status & ATA_STATUS_ERR) {
            terminal_puts(&main_terminal, "IDE: Error waiting for DRQ\r\n");
            return false;
        }
    }
    
    if (!(status & ATA_STATUS_DRQ)) {
        terminal_puts(&main_terminal, "IDE: Timeout waiting for DRQ\r\n");
        return false;
    }
    
    // 6. Leer datos de identificación (256 words = 512 bytes)
    uint16_t identify_data[256];
    
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(io_base + ATA_DATA_PORT);
    }
    
    // 7. Verificar si es válido
    if (identify_data[0] == 0 || identify_data[0] == 0xFFFF) {
        terminal_puts(&main_terminal, "IDE: Invalid identify data\r\n");
        return false;
    }
    
    // 8. Extraer información del dispositivo
    disk->supports_lba48 = (identify_data[83] & (1 << 10)) ? true : false;
    
    // Contar sectores
    if (disk->supports_lba48) {
        disk->sector_count = *((uint64_t *)&identify_data[100]);
        terminal_printf(&main_terminal, "IDE: LBA48 supported, sectors: %llu\r\n", 
                       disk->sector_count);
    } else {
        disk->sector_count = *((uint32_t *)&identify_data[60]);
        terminal_printf(&main_terminal, "IDE: LBA28 only, sectors: %llu\r\n", 
                       disk->sector_count);
    }
    
    // Tamaño de sector (siempre 512 para IDE)
    disk->sector_size = 512;
    
    // 9. Extraer modelo (words 27-46)
    for (int i = 0; i < 20; i++) {
        uint16_t word = identify_data[27 + i];
        disk->model[i * 2] = (word >> 8) & 0xFF;
        disk->model[i * 2 + 1] = word & 0xFF;
    }
    disk->model[40] = '\0';
    
    // Trim trailing spaces
    for (int i = 39; i >= 0 && (disk->model[i] == ' ' || disk->model[i] == '\0'); i--) {
        disk->model[i] = '\0';
    }
    
    // 10. Extraer número de serie (words 10-19)
    for (int i = 0; i < 10; i++) {
        uint16_t word = identify_data[10 + i];
        disk->serial[i * 2] = (word >> 8) & 0xFF;
        disk->serial[i * 2 + 1] = word & 0xFF;
    }
    disk->serial[20] = '\0';
    
    // 11. Extraer firmware (words 23-26)
    for (int i = 0; i < 4; i++) {
        uint16_t word = identify_data[23 + i];
        disk->firmware[i * 2] = (word >> 8) & 0xFF;
        disk->firmware[i * 2 + 1] = word & 0xFF;
    }
    disk->firmware[8] = '\0';
    
    terminal_printf(&main_terminal, "IDE: Identified as: %s (FW: %s, S/N: %s)\r\n",
                   disk->model, disk->firmware, disk->serial);
    
    return true;
}

bool ide_detect_disk(uint8_t bus, uint8_t drive, ide_disk_t *disk) {
    if (!disk) {
        return false;
    }
    
    memset(disk, 0, sizeof(ide_disk_t));
    disk->bus = bus;
    disk->drive = drive;
    disk->io_base = (bus == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    disk->io_ctrl = (bus == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    
    terminal_printf(&main_terminal, "IDE: Detecting disk at bus %u, drive %u...\r\n", bus, drive);
    
    // 1. Verificar tipo de dispositivo
    ide_device_type_t type = ide_detect_device_type(bus, drive);
    
    if (type != IDE_TYPE_PATA_DISK) {
        if (type == IDE_TYPE_PATAPI_CDROM) {
            terminal_puts(&main_terminal, "IDE: Skipping ATAPI device (use ATAPI driver)\r\n");
        }
        return false;
    }
    
    terminal_puts(&main_terminal, "IDE: ATA disk detected, proceeding with identification...\r\n");
    
    // 2. Identificar dispositivo
    if (!ide_identify_device(disk)) {
        terminal_puts(&main_terminal, "IDE: Failed to identify device\r\n");
        
        // Intentar reset y reintentar
        terminal_puts(&main_terminal, "IDE: Trying device reset...\r\n");
        
        // Enviar DEVICE RESET
        outb(disk->io_ctrl + ATA_DEVCTL, 0x04); // Set SRST
        for (int i = 0; i < 4; i++) inb(disk->io_ctrl + ATA_ALT_STATUS);
        outb(disk->io_ctrl + ATA_DEVCTL, 0x00); // Clear SRST
        for (volatile int i = 0; i < 100000; i++); // Esperar
        
        // Reintentar identificación
        if (!ide_identify_device(disk)) {
            return false;
        }
    }
    
    disk->present = true;
    disk->initialized = true;
    
    uint64_t size_mb = (disk->sector_count * 512) / (1024 * 1024);
    terminal_printf(&main_terminal, "IDE: Disk detected successfully: %s, %llu MB (%llu sectors)\r\n",
                   disk->model, size_mb, disk->sector_count);
    
    return true;
}

// ========================================================================
// OPERACIONES DE LECTURA/ESCRITURA
// ========================================================================

static int ide_prepare_command(ide_disk_t *disk, uint64_t lba, uint32_t count, uint8_t cmd) {
    if (count == 0 || count > 255) {
        return -1;
    }
    
    // Seleccionar drive
    ide_select_drive(disk);
    ide_400ns_delay(disk);
    
    // Limpiar IRQ pendiente
    inb(disk->io_base + ATA_STATUS_PORT);
    
    // Configurar parámetros LBA
    if (disk->supports_lba48) {
        // LBA48
        outb(disk->io_base + ATA_DRIVE_SELECT, 0x40 | (disk->drive << 4));
        ide_400ns_delay(disk);
        
        // Parámetros LBA48
        outb(disk->io_base + ATA_SECTOR_COUNT, (count >> 8) & 0xFF);
        outb(disk->io_base + ATA_LBA_LOW, (lba >> 24) & 0xFF);
        outb(disk->io_base + ATA_LBA_MID, (lba >> 32) & 0xFF);
        outb(disk->io_base + ATA_LBA_HIGH, (lba >> 40) & 0xFF);
        outb(disk->io_base + ATA_SECTOR_COUNT, count & 0xFF);
        outb(disk->io_base + ATA_LBA_LOW, lba & 0xFF);
        outb(disk->io_base + ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(disk->io_base + ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    } else {
        // LBA28
        if (lba > 0xFFFFFFFULL || lba + (uint64_t)count > 0x10000000ULL) {
            return -1;
        }
        outb(disk->io_base + ATA_DRIVE_SELECT, 
             0xE0 | (disk->drive << 4) | ((lba >> 24) & 0x0F));
        ide_400ns_delay(disk);
        outb(disk->io_base + ATA_SECTOR_COUNT, count & 0xFF);
        outb(disk->io_base + ATA_LBA_LOW, lba & 0xFF);
        outb(disk->io_base + ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(disk->io_base + ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    }
    
    // Esperar a que esté listo
    if (ide_wait_ready(disk) != 0) {
        return -1;
    }
    
    // Enviar comando
    outb(disk->io_base + ATA_COMMAND_PORT, cmd);
    
    return 0;
}

int ide_read_sectors(ide_disk_t *disk, uint64_t lba, uint32_t count, void *buffer) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        return -1;
    }
    
    if (lba + count > disk->sector_count) {
        terminal_printf(&main_terminal, "IDE: LBA out of range (%llu + %u > %llu)\r\n",
                        lba, count, disk->sector_count);
        return -1;
    }
    
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t sectors_done = 0;
    int retries = IDE_RETRIES;
    char error_msg[64];
    
    while (retries-- > 0 && sectors_done < count) {
        uint32_t sectors_to_process = count - sectors_done;
        if (sectors_to_process > 255) {
            sectors_to_process = 255;
        }
        
        uint8_t cmd = disk->supports_lba48 ? ATA_CMD_READ_SECTORS_EXT : ATA_CMD_READ_SECTORS;
        
        if (ide_prepare_command(disk, lba + sectors_done, sectors_to_process, cmd) != 0) {
            continue;
        }
        
        for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
            if (ide_wait_drq(disk) != 0) {
                break;
            }
            
            if (ide_check_error(disk, error_msg, sizeof(error_msg)) != 0) {
                terminal_printf(&main_terminal, "IDE: Read error: %s\r\n", error_msg);
                break;
            }
            
            pio_read16(disk->io_base + ATA_DATA_PORT, buf, 256); // 512 bytes = 256 words
            buf += 512;
            sectors_done++;
        }
        
        if (sectors_done == count && ide_check_error(disk, NULL, 0) == 0) {
            disk->read_count++;
            return 0;
        }
    }
    
    disk->error_count++;
    return -1;
}

int ide_write_sectors(ide_disk_t *disk, uint64_t lba, uint32_t count, const void *buffer) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        return -1;
    }
    
    if (lba + count > disk->sector_count) {
        terminal_printf(&main_terminal, "IDE: LBA out of range (%llu + %u > %llu)\r\n",
                        lba, count, disk->sector_count);
        return -1;
    }
    
    const uint8_t *buf = (const uint8_t *)buffer;
    uint32_t sectors_done = 0;
    int retries = IDE_RETRIES;
    char error_msg[64];
    
    while (retries-- > 0 && sectors_done < count) {
        uint32_t sectors_to_process = count - sectors_done;
        if (sectors_to_process > 255) {
            sectors_to_process = 255;
        }
        
        uint8_t cmd = disk->supports_lba48 ? ATA_CMD_WRITE_SECTORS_EXT : ATA_CMD_WRITE_SECTORS;
        
        if (ide_prepare_command(disk, lba + sectors_done, sectors_to_process, cmd) != 0) {
            continue;
        }
        
        for (uint32_t sector = 0; sector < sectors_to_process; sector++) {
            if (ide_wait_drq(disk) != 0) {
                break;
            }
            
            pio_write16(disk->io_base + ATA_DATA_PORT, buf, 256); // 512 bytes = 256 words
            buf += 512;
            sectors_done++;
            
            // Esperar a que termine la escritura
            if (ide_wait_ready(disk) != 0) {
                break;
            }
        }
        
        if (sectors_done == count) {
            // Flush cache
            outb(disk->io_base + ATA_COMMAND_PORT, ATA_CMD_CACHE_FLUSH);
            if (ide_wait_ready(disk) != 0) {
                continue;
            }
        }
        
        if (sectors_done == count && ide_check_error(disk, error_msg, sizeof(error_msg)) == 0) {
            disk->write_count++;
            return 0;
        }
    }
    
    disk->error_count++;
    return -1;
}

int ide_flush_cache(ide_disk_t *disk) {
    if (!disk || !disk->initialized) {
        return -1;
    }
    
    ide_select_drive(disk);
    ide_400ns_delay(disk);
    
    outb(disk->io_base + ATA_COMMAND_PORT, ATA_CMD_CACHE_FLUSH);
    
    return ide_wait_ready(disk);
}

// ========================================================================
// FUNCIONES DEL SISTEMA DE DRIVERS - CORREGIDO
// ========================================================================

static int ide_driver_init(driver_instance_t *drv, void *config) {
    (void)config;
    
    if (!drv || !drv->priv_data) {
        return -1;
    }
    
    ide_driver_priv_t *priv = (ide_driver_priv_t *)drv->priv_data;
    ide_priv = priv;  // Guardar referencia global
    
    terminal_printf(&main_terminal, "Initializing IDE driver...\r\n");
    
    // Detectar dispositivos en ambos buses
    for (uint8_t bus = 0; bus < 2; bus++) {
        for (uint8_t drive = 0; drive < 2; drive++) {
            ide_disk_t *disk = &priv->disks[bus * 2 + drive];
            
            if (ide_detect_disk(bus, drive, disk)) {
                priv->disk_count++;
                terminal_printf(&main_terminal, "IDE: Found disk at bus %u, drive %u\r\n",
                                bus, drive);
            } else {
                terminal_printf(&main_terminal, "IDE: No disk at bus %u, drive %u\r\n",
                                bus, drive);
            }
        }
    }
    
    priv->initialized = true;
    priv->driver_instance = drv;
    
    terminal_printf(&main_terminal, "IDE driver initialized: %u disks found\r\n",
                    priv->disk_count);
    
    return 0;
}

static int ide_driver_start(driver_instance_t *drv) {
    (void)drv;
    terminal_puts(&main_terminal, "IDE driver started\r\n");
    return 0;
}

static int ide_driver_stop(driver_instance_t *drv) {
    (void)drv;
    terminal_puts(&main_terminal, "IDE driver stopped\r\n");
    return 0;
}

static int ide_driver_cleanup(driver_instance_t *drv) {
    if (!drv || !drv->priv_data) {
        return -1;
    }
    
    ide_driver_priv_t *priv = (ide_driver_priv_t *)drv->priv_data;
    
    terminal_puts(&main_terminal, "Cleaning up IDE driver...\r\n");
    
    // Limpiar estructura
    memset(priv, 0, sizeof(ide_driver_priv_t));
    ide_priv = NULL;  // Limpiar referencia global
    
    terminal_puts(&main_terminal, "IDE driver cleanup complete\r\n");
    return 0;
}

static int ide_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
    if (!drv || !drv->priv_data) {
        return -1;
    }
    
    ide_driver_priv_t *priv = (ide_driver_priv_t *)drv->priv_data;
    
    switch (cmd) {
        case 0x5001: // Listar dispositivos
            ide_list_devices();
            return 0;
            
        case 0x5002: // Obtener conteo de discos
            if (arg) {
                *(uint32_t *)arg = priv->disk_count;
            }
            return 0;
            
        case 0x5003: // Obtener información de disco
            if (arg) {
                uint32_t index = *(uint32_t *)arg;
                if (index < priv->disk_count) {
                    *(ide_disk_t **)arg = &priv->disks[index];
                    return 0;
                }
            }
            return -1;
            
        case 0x5004: // Leer sectores
            {
                struct ide_io_request {
                    uint32_t disk_index;
                    uint64_t lba;
                    uint32_t count;
                    void *buffer;
                } *req = (struct ide_io_request *)arg;
                
                if (!req || req->disk_index >= priv->disk_count) {
                    return -1;
                }
                
                return ide_read_sectors(&priv->disks[req->disk_index], 
                                       req->lba, req->count, req->buffer);
            }
            
        case 0x5005: // Escribir sectores
            {
                struct ide_io_request {
                    uint32_t disk_index;
                    uint64_t lba;
                    uint32_t count;
                    const void *buffer;
                } *req = (struct ide_io_request *)arg;
                
                if (!req || req->disk_index >= priv->disk_count) {
                    return -1;
                }
                
                return ide_write_sectors(&priv->disks[req->disk_index], 
                                        req->lba, req->count, req->buffer);
            }
            
        default:
            return -1;
    }
}

static driver_ops_t ide_driver_ops = {
    .init = ide_driver_init,
    .start = ide_driver_start,
    .stop = ide_driver_stop,
    .cleanup = ide_driver_cleanup,
    .ioctl = ide_driver_ioctl,
    .read = NULL,
    .write = NULL,
    .load_data = NULL
};

static driver_type_info_t ide_driver_type = {
    .type = DRIVER_TYPE_IDE,
    .type_name = "ide",
    .version = "1.0.0",
    .priv_data_size = sizeof(ide_driver_priv_t),
    .default_ops = &ide_driver_ops,
    .validate_data = NULL,
    .print_info = NULL
};

int ide_driver_register_type(void) {
    return driver_register_type(&ide_driver_type);
}

driver_instance_t *ide_driver_create(const char *name) {
    return driver_create(DRIVER_TYPE_IDE, name);
}

// ========================================================================
// FUNCIONES PÚBLICAS
// ========================================================================

void ide_list_devices(void) {
    if (!ide_priv || !ide_priv->initialized) {
        terminal_puts(&main_terminal, "IDE driver not initialized\r\n");
        return;
    }
    
    terminal_puts(&main_terminal, "\r\n=== IDE Devices ===\r\n");
    
    if (ide_priv->disk_count == 0) {
        terminal_puts(&main_terminal, "No IDE devices found\r\n");
        return;
    }
    
    for (uint8_t i = 0; i < ide_priv->disk_count; i++) {
        ide_disk_t *disk = &ide_priv->disks[i];
        
        const char *bus_name = (disk->bus == 0) ? "Primary" : "Secondary";
        const char *drive_name = (disk->drive == 0) ? "Master" : "Slave";
        
        terminal_printf(&main_terminal, "Disk %u: %s %s\r\n", i, bus_name, drive_name);
        terminal_printf(&main_terminal, "  Model: %s\r\n", disk->model);
        terminal_printf(&main_terminal, "  Serial: %s\r\n", disk->serial);
        terminal_printf(&main_terminal, "  Firmware: %s\r\n", disk->firmware);
        terminal_printf(&main_terminal, "  Sectors: %llu\r\n", disk->sector_count);
        terminal_printf(&main_terminal, "  LBA48: %s\r\n", disk->supports_lba48 ? "Yes" : "No");
        terminal_printf(&main_terminal, "  Reads: %llu, Writes: %llu, Errors: %llu\r\n",
                       disk->read_count, disk->write_count, disk->error_count);
        terminal_puts(&main_terminal, "\r\n");
    }
}

uint8_t ide_get_disk_count(void) {
    if (!ide_priv || !ide_priv->initialized) {
        return 0;
    }
    return ide_priv->disk_count;
}

ide_disk_t *ide_get_disk_info(uint8_t index) {
    if (!ide_priv || !ide_priv->initialized || index >= ide_priv->disk_count) {
        return NULL;
    }
    return &ide_priv->disks[index];
}