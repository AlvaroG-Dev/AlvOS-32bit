#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"

// Partition types
#define PART_TYPE_EMPTY         0x00
#define PART_TYPE_FAT12         0x01
#define PART_TYPE_FAT16_SMALL   0x04
#define PART_TYPE_EXTENDED      0x05
#define PART_TYPE_FAT16         0x06
#define PART_TYPE_NTFS          0x07
#define PART_TYPE_FAT32         0x0B
#define PART_TYPE_FAT32_LBA     0x0C
#define PART_TYPE_FAT16_LBA     0x0E
#define PART_TYPE_EXTENDED_LBA  0x0F
#define PART_TYPE_LINUX         0x83
#define PART_TYPE_GPT           0xEE

// Partition flags
#define PART_FLAG_BOOTABLE      0x80

// MBR Partition Entry (16 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  status;           // 0x80 = bootable, 0x00 = non-bootable
    uint8_t  first_chs[3];     // CHS address of first sector
    uint8_t  type;             // Partition type
    uint8_t  last_chs[3];      // CHS address of last sector
    uint32_t lba_start;        // LBA of first sector
    uint32_t sector_count;     // Number of sectors
} mbr_partition_entry_t;

// Master Boot Record (512 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  boot_code[446];   // Bootstrap code
    mbr_partition_entry_t partitions[4];  // Partition table
    uint16_t signature;        // 0xAA55
} mbr_t;

// Parsed partition information
typedef struct partition_info_t {
    uint8_t  index;            // Partition index (0-3 for primary)
    uint8_t  type;             // Partition type
    bool     bootable;         // Bootable flag
    uint64_t lba_start;        // Starting LBA
    uint64_t sector_count;     // Size in sectors
    uint64_t size_mb;          // Size in MB
    bool     is_extended;      // Extended partition
} partition_info_t;


// Partition table context
typedef struct {
    disk_t *disk;
    mbr_t mbr;
    partition_info_t partitions[4];
    uint32_t partition_count;
} partition_table_t;

// Error codes
typedef enum {
    PART_OK = 0,
    PART_ERR_INVALID_DISK,
    PART_ERR_READ_FAILED,
    PART_ERR_INVALID_MBR,
    PART_ERR_NO_PARTITIONS,
    PART_ERR_WRITE_FAILED,
    PART_ERR_INVALID_INDEX
} part_err_t;

// Functions
part_err_t partition_read_table(disk_t *disk, partition_table_t *pt);
part_err_t partition_write_table(partition_table_t *pt);
const char* partition_type_name(uint8_t type);
bool partition_is_fat(uint8_t type);
void partition_print_info(partition_table_t *pt);
partition_info_t* partition_find_bootable(partition_table_t *pt);
partition_info_t* partition_find_by_type(partition_table_t *pt, uint8_t type);
part_err_t partition_set_bootable(partition_table_t *pt, uint8_t index);
// Nueva función para crear una entrada de partición
void partition_create_entry(mbr_partition_entry_t *entry, uint8_t type, 
                           uint64_t start_lba, uint64_t sector_count, bool bootable);

// Función para verificar espacio libre
uint64_t partition_find_free_space(partition_table_t *pt, uint64_t size_sectors);

// Función para calcular CHS desde LBA (simplificada)
void partition_lba_to_chs(uint64_t lba, uint8_t *chs);

// Función para limpiar tabla de particiones
void partition_clear_table(partition_table_t *pt);

#endif // PARTITION_H