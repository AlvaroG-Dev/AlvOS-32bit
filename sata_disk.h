#ifndef SATA_DISK_H
#define SATA_DISK_H

#include "disk.h"
#include "dma.h"
#include <stdbool.h>
#include <stdint.h>


#define MAX_SATA_DISKS 8
#define SECTOR_SIZE 512
#define SATA_IO_BUFFER_SIZE (128 * 1024) // 64KB buffer for I/O

extern bool sata_initialized;

// SATA Error codes
typedef enum {
  SATA_ERR_NONE = 0,
  SATA_ERR_INVALID_PARAM,
  SATA_ERR_NOT_INITIALIZED,
  SATA_ERR_IO_ERROR,
  SATA_ERR_TIMEOUT,
  SATA_ERR_LBA_OUT_OF_RANGE,
  SATA_ERR_NO_MEMORY,
  SATA_ERR_DEVICE_NOT_READY
} sata_err_t;

// SATA Disk structure
typedef struct {
  uint8_t ahci_port; // AHCI port number
  bool present;      // Device present
  bool initialized;  // Device initialized

  // Device information
  char model[41];           // Device model string
  char serial[21];          // Device serial number
  uint64_t sector_count;    // Total sectors (LBA48 or LBA28)
  uint32_t sector_count_28; // LBA28 sector count

  // Capabilities
  bool supports_lba48; // LBA48 support
  bool supports_dma;   // DMA support
  bool supports_ncq;   // Native Command Queuing

  // I/O Buffer
  dma_buffer_t *io_buffer; // DMA buffer for I/O operations

  // Statistics
  uint64_t read_count;  // Number of read operations
  uint64_t write_count; // Number of write operations
  uint64_t error_count; // Number of errors
} sata_disk_t;

// Function prototypes

// Initialization and cleanup
bool sata_disk_init(void);
void sata_disk_cleanup(void);
bool sata_disk_setup(sata_disk_t *disk, uint8_t ahci_port);

// Disk operations
sata_err_t sata_disk_read(uint32_t disk_id, uint64_t lba, uint32_t count,
                          void *buffer);
sata_err_t sata_disk_write(uint32_t disk_id, uint64_t lba, uint32_t count,
                           const void *buffer);
sata_err_t sata_disk_flush(uint32_t disk_id);

// Information and utilities
uint32_t sata_disk_get_count(void);
sata_disk_t *sata_disk_get_info(uint32_t disk_id);
uint64_t sata_disk_get_sector_count(uint32_t disk_id);
bool sata_disk_is_present(uint32_t disk_id);
void sata_disk_list(void);

// Legacy disk interface compatibility
disk_err_t sata_to_legacy_disk_init(disk_t *disk, uint32_t sata_disk_id);
disk_err_t sata_to_legacy_disk_read(disk_t *disk, uint64_t lba, uint32_t count,
                                    void *buffer);
disk_err_t sata_to_legacy_disk_write(disk_t *disk, uint64_t lba, uint32_t count,
                                     const void *buffer);

// Testing
bool sata_disk_test(uint32_t disk_id);
void sata_disk_debug_port(uint8_t port_num);

#endif // SATA_DISK_H