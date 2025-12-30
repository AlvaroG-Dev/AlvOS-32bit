#ifndef FAT32_H
#define FAT32_H

#include "disk.h"
#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

// FAT32 constants
#define FAT32_SECTOR_SIZE 512
#define FAT32_MAX_FILENAME 255
#define FAT32_DIR_ENTRY_SIZE 32
#define FAT32_ENTRIES_PER_SECTOR (FAT32_SECTOR_SIZE / FAT32_DIR_ENTRY_SIZE)
#define FAT32_EOC 0x0FFFFFF8               // End of chain marker
#define FAT32_BAD_CLUSTER 0x0FFFFFF7       // Bad cluster marker
#define FAT32_FREE_CLUSTER 0x00000000      // Free cluster marker
#define FAT32_RESERVED_CLUSTER 0x0FFFFFF0  // Reserved cluster marker
#define FAT32_CLN_SHUT_BIT_MASK 0x08000000 // Bit 27: 1 = clean, 0 = dirty
#define FAT32_HRD_ERR_BIT_MASK 0x04000000  // Bit 26: 1 = no errors, 0 = errors

// Directory entry attributes
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LONG_NAME                                                   \
  (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM |              \
   FAT32_ATTR_VOLUME_ID)

// Formatting constants
#define FAT32_DEFAULT_SECTORS_PER_CLUSTER 8
#define FAT32_DEFAULT_NUM_FATS 2
#define FAT32_MAX_VOLUME_LABEL 11
#define FAT32_AUTO_SPC 0

// Boot sector structure (packed)
typedef struct __attribute__((packed)) {
  uint8_t jmp_boot[3];         // Jump instruction
  uint8_t oem_name[8];         // OEM name
  uint16_t bytes_per_sector;   // Bytes per sector
  uint8_t sectors_per_cluster; // Sectors per cluster
  uint16_t reserved_sectors;   // Reserved sectors
  uint8_t num_fats;            // Number of FATs
  uint16_t root_entries;       // Root directory entries (0 for FAT32)
  uint16_t total_sectors_16;   // Total sectors (0 for FAT32)
  uint8_t media_type;          // Media type
  uint16_t sectors_per_fat_16; // Sectors per FAT (0 for FAT32)
  uint16_t sectors_per_track;  // Sectors per track
  uint16_t num_heads;          // Number of heads
  uint32_t hidden_sectors;     // Hidden sectors
  uint32_t total_sectors_32;   // Total sectors

  // FAT32 specific fields
  uint32_t sectors_per_fat_32;    // Sectors per FAT
  uint16_t ext_flags;             // Extended flags
  uint16_t fs_version;            // Filesystem version
  uint32_t root_cluster;          // Root directory cluster
  uint16_t fs_info_sector;        // FSInfo sector
  uint16_t backup_boot_sector;    // Backup boot sector
  uint8_t reserved[12];           // Reserved
  uint8_t drive_number;           // Drive number
  uint8_t reserved1;              // Reserved
  uint8_t boot_signature;         // Boot signature
  uint32_t volume_id;             // Volume ID
  uint8_t volume_label[11];       // Volume label
  uint8_t fs_type[8];             // Filesystem type
  uint8_t boot_code[420];         // Boot code
  uint16_t boot_sector_signature; // Boot sector signature (0xAA55)
} fat32_boot_sector_t;

// FSInfo sector structure (packed)
typedef struct __attribute__((packed)) {
  uint32_t lead_signature;    // Lead signature (0x41615252)
  uint8_t reserved[480];      // Reserved
  uint32_t struct_signature;  // Structure signature (0x61417272)
  uint32_t free_clusters;     // Free cluster count
  uint32_t next_free_cluster; // Next free cluster
  uint8_t reserved2[12];      // Reserved
  uint32_t trail_signature;   // Trail signature (0xAA550000)
} fat32_fsinfo_t;

// Directory entry structure (packed)
typedef struct __attribute__((packed)) {
  uint8_t name[11];            // Short name (8.3 format)
  uint8_t attributes;          // File attributes
  uint8_t nt_reserved;         // Reserved for Windows NT
  uint8_t creation_time_tenth; // Creation time (tenths of second)
  uint16_t creation_time;      // Creation time
  uint16_t creation_date;      // Creation date
  uint16_t last_access_date;   // Last access date
  uint16_t first_cluster_high; // High word of first cluster
  uint16_t write_time;         // Write time
  uint16_t write_date;         // Write date
  uint16_t first_cluster_low;  // Low word of first cluster
  uint32_t file_size;          // File size in bytes
} fat32_dir_entry_t;

// Long filename entry structure (packed)
typedef struct __attribute__((packed)) {
  uint8_t order;          // Order of this entry
  uint16_t name1[5];      // First 5 characters (Unicode)
  uint8_t attributes;     // Attributes (always FAT32_ATTR_LONG_NAME)
  uint8_t type;           // Type (always 0)
  uint8_t checksum;       // Checksum of short name
  uint16_t name2[6];      // Next 6 characters (Unicode)
  uint16_t first_cluster; // First cluster (always 0)
  uint16_t name3[2];      // Last 2 characters (Unicode)
} fat32_lfn_entry_t;

// FAT32 filesystem private data
typedef struct {
  disk_t *disk;                    // Backing disk device
  fat32_boot_sector_t boot_sector; // Boot sector
  fat32_fsinfo_t fsinfo;           // FSInfo sector

  // Calculated values
  uint32_t fat_start_sector;  // First FAT sector
  uint32_t data_start_sector; // First data sector
  uint32_t root_dir_cluster;  // Root directory cluster
  uint32_t cluster_size;      // Cluster size in bytes
  uint32_t total_clusters;    // Total number of clusters

  // Cache for FAT table
  uint32_t *fat_cache;       // Cached FAT entries
  uint32_t fat_cache_sector; // Currently cached FAT sector
  uint8_t fat_cache_dirty;   // FAT cache dirty flag

  // Directory entry cache
  uint8_t *dir_cache;        // Directory sector cache
  uint32_t dir_cache_sector; // Currently cached directory sector
  uint8_t dir_cache_dirty;   // Directory cache dirty flag

  // Nueva: Flag para errores
  uint8_t has_errors; // 1 si hubo errores, 0 si no
} fat32_fs_t;

// FAT32 vnode private data
typedef struct {
  uint32_t first_cluster;   // First cluster of file/directory
  uint32_t size;            // File size (0 for directories)
  uint32_t current_cluster; // Current cluster for sequential access
  uint32_t cluster_offset;  // Offset within current cluster
  uint8_t attributes;       // File attributes
  uint8_t is_directory;     // 1 if directory, 0 if file
  uint32_t parent_cluster;  // Cluster of the parent directory (0 for root)
  uint8_t short_name[11];   // Short 8.3 name for locating dir entry
} fat32_node_t;

// Function prototypes
extern vfs_fs_type_t fat32_fs_type;

// Internal functions
int fat32_mount(void *device, vfs_superblock_t **out_sb);
int fat32_read_boot_sector(fat32_fs_t *fs);
int fat32_read_fsinfo(fat32_fs_t *fs);
uint32_t fat32_get_fat_entry(fat32_fs_t *fs, uint32_t cluster);
int fat32_set_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value);
uint32_t fat32_allocate_cluster(fat32_fs_t *fs);
int fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t cluster);
uint32_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster);
int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buffer);
int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buffer);
int fat32_update_fsinfo(fat32_fs_t *fs); // Cambiado de void a int
int fat32_update_dir_entry(fat32_fs_t *fs, fat32_node_t *node_data);
static int
fat32_is_directory_empty(fat32_fs_t *fs,
                         uint32_t dir_cluster); // Nueva: declaración static
int fat32_validate_cluster_chain(fat32_fs_t *fs, uint32_t first_cluster,
                                 uint32_t *out_chain_length);

// VFS operations
int fat32_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out);
int fat32_create(vfs_node_t *parent, const char *name, vfs_node_t **out);
int fat32_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset);
int fat32_write(vfs_node_t *node, const uint8_t *buf, uint32_t size,
                uint32_t offset);
int fat32_readdir(vfs_node_t *node, vfs_dirent_t *buf, uint32_t *count,
                  uint32_t offset);
void fat32_release(vfs_node_t *node);
int fat32_mkdir(vfs_node_t *parent, const char *name, vfs_node_t **out);
int fat32_unlink(vfs_node_t *parent, const char *name);

// Utility functions
int fat32_parse_short_name(const char *name, uint8_t *fat_name);
int fat32_format_short_name(const uint8_t *fat_name, char *name);
uint8_t fat32_calculate_checksum(const uint8_t *short_name);
bool check_fat32_signature(uint8_t *boot_sector);
int fat32_find_free_dir_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                              uint32_t *sector, uint32_t *offset);
int fat32_create_dir_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                           const char *name, uint32_t first_cluster,
                           uint32_t size, uint8_t attributes);
int fat32_calculate_free_clusters(fat32_fs_t *fs, uint32_t *free_clusters,
                                  uint32_t *next_free_cluster);

// CACHE HANDLING
int fat32_flush_fat_cache(fat32_fs_t *fs);
int fat32_flush_dir_cache(fat32_fs_t *fs);

// Test
int create_large_file(const char *path);

// Formatting functions
int fat32_format(disk_t *disk, const char *volume_label);
int fat32_format_with_params(disk_t *disk, uint16_t sectors_per_cluster,
                             uint8_t num_fats, const char *volume_label);

#endif // FAT32_H
