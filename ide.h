// ide.h - Driver de discos IDE/ATA
#ifndef IDE_H
#define IDE_H

#include <stdbool.h>
#include <stdint.h>
#include "driver_system.h"

// Comandos ATA
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_SET_FEATURES 0xEF
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_DEVICE_RESET 0x08

// Puertos ATA
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

// Registros ATA (OFFSETS, no puertos completos)
#define ATA_DATA_PORT 0x0
#define ATA_ERROR_PORT 0x1
#define ATA_FEATURES_PORT 0x1
#define ATA_SECTOR_COUNT 0x2
#define ATA_LBA_LOW 0x3
#define ATA_LBA_MID 0x4
#define ATA_LBA_HIGH 0x5
#define ATA_DRIVE_SELECT 0x6
#define ATA_COMMAND_PORT 0x7
#define ATA_STATUS_PORT 0x7
#define ATA_DEVCTL 0x206   // Device Control (write) - offset en puerto de control
#define ATA_ALT_STATUS 0x206 // Alternate Status (read) - offset en puerto de control

// Estados ATA
#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_RDY 0x40
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_ERR 0x01

// Errores ATA
#define ATA_ERR_ABRT 0x04 // Command aborted
#define ATA_ERR_IDNF 0x10 // ID not found
#define ATA_ERR_UNC 0x40  // Uncorrectable data error

// Firmware signatures
#define ATA_SIGNATURE_LBA_MID 0x00
#define ATA_SIGNATURE_LBA_HIGH 0x00
#define ATAPI_SIGNATURE_LBA_MID 0x14
#define ATAPI_SIGNATURE_LBA_HIGH 0xEB
#define SATA_SIGNATURE_LBA_MID 0x3C
#define SATA_SIGNATURE_LBA_HIGH 0xC3

// Timeouts
#define IDE_TIMEOUT_MS 5000
#define IDE_RETRIES 3

// Tipo de dispositivo IDE
typedef enum {
    IDE_TYPE_NONE = 0,
    IDE_TYPE_PATA_DISK,
    IDE_TYPE_PATAPI_CDROM,
    IDE_TYPE_UNKNOWN
} ide_device_type_t;

// Estructura del disco IDE
typedef struct {
    uint8_t bus;          // 0=primary, 1=secondary
    uint8_t drive;        // 0=master, 1=slave
    bool present;         // Dispositivo presente
    bool initialized;     // Inicializado
    bool supports_lba48;  // Soporta LBA48
    uint64_t sector_count;// Número de sectores
    uint32_t sector_size; // Tamaño de sector (512 bytes para IDE)
    
    // Información del dispositivo
    char model[41];       // Modelo
    char serial[21];      // Número de serie
    char firmware[9];     // Firmware
    
    // Puertos I/O
    uint16_t io_base;     // Puerto base de datos
    uint16_t io_ctrl;     // Puerto de control
    
    // Estadísticas
    uint64_t read_count;
    uint64_t write_count;
    uint64_t error_count;
} ide_disk_t;

// Estructura privada del driver IDE
typedef struct {
    ide_disk_t disks[4];  // Primary master/slave, Secondary master/slave
    uint8_t disk_count;
    bool initialized;
    driver_instance_t *driver_instance;
} ide_driver_priv_t;

// Funciones públicas
int ide_driver_register_type(void);
driver_instance_t *ide_driver_create(const char *name);

// Funciones de detección
ide_device_type_t ide_detect_device_type(uint8_t bus, uint8_t drive);
bool ide_detect_disk(uint8_t bus, uint8_t drive, ide_disk_t *disk);
bool ide_identify_device(ide_disk_t *disk);

// Funciones de operación
int ide_read_sectors(ide_disk_t *disk, uint64_t lba, uint32_t count, void *buffer);
int ide_write_sectors(ide_disk_t *disk, uint64_t lba, uint32_t count, const void *buffer);
int ide_flush_cache(ide_disk_t *disk);

// Funciones de bajo nivel
void ide_select_drive(ide_disk_t *disk);
void ide_400ns_delay(ide_disk_t *disk);
int ide_wait_ready(ide_disk_t *disk);
int ide_wait_drq(ide_disk_t *disk);
uint8_t ide_read_status(ide_disk_t *disk);
int ide_check_error(ide_disk_t *disk, char *error_msg, size_t msg_size);

// Funciones de información
void ide_list_devices(void);
uint8_t ide_get_disk_count(void);
ide_disk_t *ide_get_disk_info(uint8_t index);

#endif // IDE_H