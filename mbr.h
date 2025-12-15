#ifndef MBR_H
#define MBR_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"
#include "partition.h"

// Boot signature
#define BOOT_SIGNATURE 0xAA55

// MBR boot code size
#define MBR_BOOT_CODE_SIZE 446

// VBR (Volume Boot Record) for FAT32
typedef struct __attribute__((packed)) {
    uint8_t  jump[3];              // Jump instruction
    uint8_t  oem_name[8];          // OEM name
    uint16_t bytes_per_sector;     // Bytes per sector
    uint8_t  sectors_per_cluster;  // Sectors per cluster
    uint16_t reserved_sectors;     // Reserved sectors
    uint8_t  num_fats;             // Number of FATs
    uint16_t root_entries;         // Root entries (0 for FAT32)
    uint16_t total_sectors_16;     // Total sectors (0 for FAT32)
    uint8_t  media_type;           // Media type
    uint16_t sectors_per_fat_16;   // Sectors per FAT (0 for FAT32)
    uint16_t sectors_per_track;    // Sectors per track
    uint16_t num_heads;            // Number of heads
    uint32_t hidden_sectors;       // Hidden sectors
    uint32_t total_sectors_32;     // Total sectors
    uint32_t sectors_per_fat_32;   // Sectors per FAT
    uint16_t ext_flags;            // Extended flags
    uint16_t fs_version;           // Filesystem version
    uint32_t root_cluster;         // Root directory cluster
    uint16_t fs_info_sector;       // FSInfo sector
    uint16_t backup_boot_sector;   // Backup boot sector
    uint8_t  reserved[12];         // Reserved
    uint8_t  drive_number;         // Drive number
    uint8_t  reserved1;            // Reserved
    uint8_t  boot_signature;       // Boot signature (0x29)
    uint32_t volume_id;            // Volume ID
    uint8_t  volume_label[11];     // Volume label
    uint8_t  fs_type[8];           // Filesystem type
    uint8_t  boot_code[420];       // Boot code
    uint16_t signature;            // Boot signature (0xAA55)
} vbr_fat32_t;

// Error codes
typedef enum {
    MBR_OK = 0,
    MBR_ERR_INVALID_DISK,
    MBR_ERR_READ_FAILED,
    MBR_ERR_WRITE_FAILED,
    MBR_ERR_INVALID_SIGNATURE,
    MBR_ERR_BUFFER_TOO_SMALL,
    MBR_ERR_VERIFY_FAILED
} mbr_err_t;

// Functions for MBR
mbr_err_t mbr_read(disk_t *disk, mbr_t *mbr);
mbr_err_t mbr_write(disk_t *disk, const mbr_t *mbr);
mbr_err_t mbr_install_bootcode(disk_t *disk, const uint8_t *boot_code, uint32_t size);
mbr_err_t mbr_backup(disk_t *disk, uint8_t *backup_buffer, uint32_t buffer_size);
mbr_err_t mbr_restore(disk_t *disk, const uint8_t *backup_buffer, uint32_t buffer_size);
bool mbr_verify_signature(const mbr_t *mbr);

// Functions for VBR (Volume Boot Record)
mbr_err_t vbr_read(disk_t *disk, uint64_t partition_lba, vbr_fat32_t *vbr);
mbr_err_t vbr_write(disk_t *disk, uint64_t partition_lba, const vbr_fat32_t *vbr);
mbr_err_t vbr_install_bootcode(disk_t *disk, uint64_t partition_lba, 
                                const uint8_t *boot_code, uint32_t size);
bool vbr_verify_fat32(const vbr_fat32_t *vbr);

// Utility functions
void mbr_print_hex(const uint8_t *data, uint32_t size);

#endif // MBR_H