#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stdint.h>

#define DISK_DRIVE_IDE_MASTER 0x00
#define DISK_DRIVE_IDE_SLAVE 0x01
#define DISK_DRIVE_IDE_SEC_MASTER 0x02
#define DISK_DRIVE_IDE_SEC_SLAVE 0x03
#define DISK_DRIVE_SATA_FIRST 0x80  // Primer disco SATA
#define DISK_DRIVE_ATAPI_FIRST 0xE0 // Primer dispositivo ATAPI

#define SECTOR_SIZE 512

typedef struct partition_info_t partition_info_t;
typedef struct disk_t disk_t;

typedef enum {
  DEVICE_TYPE_NONE = 0,
  DEVICE_TYPE_PATA_DISK,
  DEVICE_TYPE_PATAPI_CDROM,
  DEVICE_TYPE_SATA_DISK,
  DEVICE_TYPE_SATAPI_CDROM,
  DEVICE_TYPE_USB_DISK,
  DEVICE_TYPE_UNKNOWN
} device_type_t;

struct disk_t {
  uint8_t drive_number;
  uint8_t initialized; // 0 = not initialized, 1 = initialized
  uint8_t present;
  uint8_t supports_lba48;        // 0 = LBA28 only, 1 = supports LBA48
  uint64_t sector_count;         // Use uint64_t for LBA48 support
  device_type_t type;            // IDE or SATA
  uint64_t partition_lba_offset; // Offset LBA de la partición
  bool is_partition;             // true si es wrapper de partición
  disk_t *physical_disk; // Pointer to physical disk if this is a partition;
                         // NULL otherwise
};

// Disk operation states
typedef enum { DISK_OP_NONE, DISK_OP_READ, DISK_OP_WRITE } disk_op_t;

// Error codes
typedef enum {
  DISK_ERR_NONE = 0,
  DISK_ERR_INVALID_PARAM,
  DISK_ERR_NOT_INITIALIZED,
  DISK_ERR_TIMEOUT,
  DISK_ERR_DEVICE_NOT_PRESENT,
  DISK_ERR_ATA,
  DISK_ERR_ATAPI,
  DISK_ERR_LBA_OUT_OF_RANGE
} disk_err_t;

typedef struct {
  bool present;
  device_type_t type;
  uint8_t bus;
  uint8_t drive;
  char description[64];
} detected_device_t;

// Firmware signature detection
typedef enum {
  FIRMWARE_ATA = 0,
  FIRMWARE_ATAPI = 1,
  FIRMWARE_UNKNOWN = 2
} firmware_signature_t;

inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// Basic functions
disk_err_t disk_init(disk_t *disk, uint8_t drive_number);

disk_err_t disk_read(disk_t *disk, uint64_t lba, uint32_t count, void *buffer);
disk_err_t disk_write(disk_t *disk, uint64_t lba, uint32_t count,
                      const void *buffer);
uint64_t disk_get_sector_count(disk_t *disk);
int disk_is_initialized(disk_t *disk);
uint32_t disk_get_io_ticks(void);
uint64_t disk_get_io_cycles(void);
disk_err_t disk_flush(disk_t *disk);
disk_err_t disk_init_from_partition(disk_t *partition_disk,
                                    disk_t *physical_disk,
                                    partition_info_t *partition);

// Dispatch functions for unified disk interface
disk_err_t disk_read_dispatch(disk_t *disk, uint64_t lba, uint32_t count,
                              void *buffer);
disk_err_t disk_write_dispatch(disk_t *disk, uint64_t lba, uint32_t count,
                               const void *buffer);
disk_err_t disk_flush_dispatch(disk_t *disk);
void diagnose_disk_format(disk_t *disk);

disk_err_t disk_init_atapi(disk_t *disk, uint32_t atapi_device_id);
bool disk_is_atapi(disk_t *disk);
bool disk_atapi_media_present(disk_t *disk);
disk_err_t disk_atapi_eject(disk_t *disk);
disk_err_t disk_atapi_load(disk_t *disk);

void disk_list_detected_devices(void);
void disk_scan_all_buses(void);
void cmd_lsblk(void);

// Convenience macros for cleaner code
#define DISK_READ(disk, lba, count, buffer)                                    \
  disk_read_dispatch(disk, lba, count, buffer)
#define DISK_WRITE(disk, lba, count, buffer)                                   \
  disk_write_dispatch(disk, lba, count, buffer)
#define DISK_FLUSH(disk) disk_flush_dispatch(disk)

// Global state variables
extern volatile disk_op_t disk_current_op;
extern volatile void *disk_current_buffer;
extern volatile uint32_t disk_remaining_sectors;
extern volatile disk_err_t disk_error;
extern volatile uint32_t total_io_ticks;
extern volatile uint64_t total_io_cycles;

#endif
