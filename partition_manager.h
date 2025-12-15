#ifndef PARTITION_MANAGER_H
#define PARTITION_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"
#include "partition.h"
#include "terminal.h"

#define MAX_DISKS 8
#define MAX_PARTITIONS_PER_DISK 16

typedef enum {
    PART_MGR_OK = 0,
    PART_MGR_ERR_INVALID_DISK,
    PART_MGR_ERR_NO_PARTITIONS,
    PART_MGR_ERR_INVALID_PARTITION,
    PART_MGR_ERR_READ_FAILED,
    PART_MGR_ERR_WRITE_FAILED,
    PART_MGR_ERR_OVERLAP,
    PART_MGR_ERR_NO_SPACE,
    PART_MGR_ERR_LBA_OUT_OF_RANGE
} part_mgr_err_t;

typedef struct {
    disk_t *disk;
    partition_table_t partition_table;
    bool initialized;
    uint32_t disk_id;
} disk_partitions_t;

uint64_t partition_calculate_next_start_lba(partition_table_t *pt);

// Gestión de particiones
part_mgr_err_t partition_manager_init(void);
part_mgr_err_t partition_manager_scan_disk(disk_t *disk, uint32_t disk_id);
part_mgr_err_t partition_manager_create_partition(uint32_t disk_id, uint8_t part_num, 
                                                 uint8_t type, uint64_t start_lba, 
                                                 uint64_t sector_count, bool bootable);
part_mgr_err_t partition_manager_delete_partition(uint32_t disk_id, uint8_t part_num);
part_mgr_err_t partition_manager_format_partition(uint32_t disk_id, uint8_t part_num, 
                                                 const char *fs_type);
part_mgr_err_t partition_manager_set_bootable(uint32_t disk_id, uint8_t part_num, bool bootable);

// Información y listado
disk_partitions_t* partition_manager_get_disk(uint32_t disk_id);
partition_info_t* partition_manager_get_partition(uint32_t disk_id, uint8_t part_num);
uint32_t partition_manager_get_disk_count(void);
void partition_manager_list_disks(void);
void partition_manager_list_partitions(uint32_t disk_id);

// Utilidades para VFS
part_mgr_err_t partition_manager_mount_partition(uint32_t disk_id, uint8_t part_num, 
                                                const char *mount_point, const char *fs_type);
part_mgr_err_t partition_manager_auto_mount_all(void);

// Verificación y reparación
bool partition_manager_verify_partition_table(uint32_t disk_id);
part_mgr_err_t partition_manager_repair_partition_table(uint32_t disk_id);

// Comandos de particiones
void part_list_command(Terminal *term, const char *args);
void part_space_command(Terminal *term, const char *args);
void part_create_command(Terminal *term, const char *args);
void part_delete_command(Terminal *term, const char *args);
void part_fix_order_command(Terminal *term, const char *args);
void part_format_command(Terminal *term, const char *args);
void part_mount_command(Terminal *term, const char *args);
void part_info_command(Terminal *term, const char *args);
void part_scan_command(Terminal *term, const char *args);
void part_help_command(Terminal *term, const char *args);

// Comando de formateo avanzado
void part_format_advanced_command(Terminal *term, const char *args);

#endif