#include "partition.h"
#include "fat32.h"
#include "kernel.h"
#include "mbr.h"
#include "string.h"
#include "terminal.h"

extern Terminal main_terminal;

// Read partition table from disk
part_err_t partition_read_table(disk_t *disk, partition_table_t *pt) {
  if (!disk || !pt) {
    return PART_ERR_INVALID_DISK;
  }

  memset(pt, 0, sizeof(partition_table_t));
  pt->disk = disk;

  // Leer MBR (sector 0)
  disk_err_t err = disk_read_dispatch(disk, 0, 1, &pt->mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "PART: Failed to read MBR (error %d)\n",
                    err);
    // Intentar con más información de depuración
    uint8_t sector[512];
    err = disk_read_dispatch(disk, 0, 1, sector);
    if (err == DISK_ERR_NONE) {
      terminal_printf(&main_terminal, "PART: Raw sector read successful\n");
      terminal_printf(&main_terminal, "PART: Signature bytes: 0x%02X%02X\n",
                      sector[510], sector[511]);
    }
    return PART_ERR_READ_FAILED;
  }

  // Verificar MBR signature
  terminal_printf(&main_terminal,
                  "PART: MBR signature=0x%04X (expected 0xAA55)\n",
                  pt->mbr.signature);

  if (pt->mbr.signature != 0xAA55) {
    // Intentar ver si es FAT32 sin particiones (disco completo)
    uint8_t boot_sector[512];
    memcpy(boot_sector, &pt->mbr, 512);

    // Verificar si es FAT32 directamente (sin partición)
    if (check_fat32_signature(boot_sector)) {
      terminal_puts(&main_terminal, "PART: Detected partitionless FAT32\n");
      // Crear partición virtual que cubre todo el disco
      partition_info_t *info = &pt->partitions[0];
      info->index = 0;
      info->type = PART_TYPE_FAT32_LBA;
      info->bootable = true;
      info->lba_start = 0;
      info->sector_count = disk->sector_count;
      info->size_mb = (info->sector_count * 512ULL) / (1024 * 1024);
      info->is_extended = false;
      pt->partition_count = 1;

      // Crear entrada MBR correspondiente
      mbr_partition_entry_t *entry = &pt->mbr.partitions[0];
      entry->status = PART_FLAG_BOOTABLE;
      entry->type = PART_TYPE_FAT32_LBA;
      entry->lba_start = 0;
      entry->sector_count = disk->sector_count;
      partition_lba_to_chs(0, entry->first_chs);
      partition_lba_to_chs(disk->sector_count - 1, entry->last_chs);
      pt->mbr.signature = 0xAA55; // Forzar firma

      terminal_printf(&main_terminal,
                      "PART: Created virtual partition covering whole disk\n");
      return PART_OK;
    }

    terminal_printf(&main_terminal, "PART: Invalid MBR signature\n");
    mbr_print_hex((uint8_t *)&pt->mbr, 512);
    return PART_ERR_INVALID_MBR;
  }

  // Parse partition entries
  pt->partition_count = 0;
  for (int i = 0; i < 4; i++) {
    mbr_partition_entry_t *entry = &pt->mbr.partitions[i];

    terminal_printf(&main_terminal,
                    "PART: Entry %d: type=0x%02X, start=%u, size=%u\n", i,
                    entry->type, entry->lba_start, entry->sector_count);

    if (entry->type == PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal, "PART: Entry %d is empty\n", i);
      continue;
    }

    // Validar LBA start (debe ser >= 2048 para discos modernos)
    if (entry->lba_start < 2048 && entry->lba_start != 0) {
      terminal_printf(&main_terminal,
                      "PART: Warning: Partition %d has unusual start LBA %u\n",
                      i, entry->lba_start);
    }

    partition_info_t *info = &pt->partitions[pt->partition_count];
    info->index = i;
    info->type = entry->type;
    info->bootable = (entry->status == PART_FLAG_BOOTABLE);
    info->lba_start = entry->lba_start;
    info->sector_count = entry->sector_count;
    info->size_mb = (info->sector_count * 512ULL) / (1024 * 1024);
    info->is_extended = (entry->type == PART_TYPE_EXTENDED ||
                         entry->type == PART_TYPE_EXTENDED_LBA);

    pt->partition_count++;

    terminal_printf(&main_terminal,
                    "PART: Found partition %d: %s, %llu MB, LBA %u-%u, %s\n", i,
                    partition_type_name(entry->type), info->size_mb,
                    entry->lba_start,
                    entry->lba_start + entry->sector_count - 1,
                    info->bootable ? "bootable" : "non-bootable");
  }

  if (pt->partition_count == 0) {
    terminal_puts(&main_terminal, "PART: No partitions found in MBR\n");
    return PART_ERR_NO_PARTITIONS;
  }

  return PART_OK;
}

// Write partition table to disk
part_err_t partition_write_table(partition_table_t *pt) {
  if (!pt || !pt->disk) {
    return PART_ERR_INVALID_DISK;
  }

  // Verify signature before writing
  if (pt->mbr.signature != 0xAA55) {
    terminal_printf(&main_terminal, "PART: Refusing to write invalid MBR\n");
    return PART_ERR_INVALID_MBR;
  }

  terminal_printf(&main_terminal, "PART: Writing partition table...\n");

  disk_err_t err = disk_write_dispatch(pt->disk, 0, 1, &pt->mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "PART: Failed to write MBR (error %d)\n",
                    err);
    return PART_ERR_WRITE_FAILED;
  }

  // Verify write
  mbr_t verify_mbr;
  err = disk_read_dispatch(pt->disk, 0, 1, &verify_mbr);
  if (err != DISK_ERR_NONE ||
      memcmp(&pt->mbr, &verify_mbr, sizeof(mbr_t)) != 0) {
    terminal_printf(&main_terminal, "PART: MBR verification failed!\n");
    return PART_ERR_WRITE_FAILED;
  }

  terminal_printf(&main_terminal,
                  "PART: Partition table written and verified\n");
  return PART_OK;
}

// Get partition type name
const char *partition_type_name(uint8_t type) {
  switch (type) {
  case PART_TYPE_EMPTY:
    return "Empty";
  case PART_TYPE_FAT12:
    return "FAT12";
  case PART_TYPE_FAT16_SMALL:
    return "FAT16 (small)";
  case PART_TYPE_EXTENDED:
    return "Extended";
  case PART_TYPE_FAT16:
    return "FAT16";
  case PART_TYPE_NTFS:
    return "NTFS/exFAT";
  case PART_TYPE_FAT32:
    return "FAT32";
  case PART_TYPE_FAT32_LBA:
    return "FAT32 LBA";
  case PART_TYPE_FAT16_LBA:
    return "FAT16 LBA";
  case PART_TYPE_EXTENDED_LBA:
    return "Extended LBA";
  case PART_TYPE_LINUX:
    return "Linux";
  case PART_TYPE_GPT:
    return "GPT Protective";
  default:
    return "Unknown";
  }
}

// Check if partition is FAT filesystem
bool partition_is_fat(uint8_t type) {
  return (type == PART_TYPE_FAT12 || type == PART_TYPE_FAT16_SMALL ||
          type == PART_TYPE_FAT16 || type == PART_TYPE_FAT32 ||
          type == PART_TYPE_FAT32_LBA || type == PART_TYPE_FAT16_LBA);
}

// Print partition table information
void partition_print_info(partition_table_t *pt) {
  if (!pt)
    return;

  terminal_printf(&main_terminal, "\n=== Partition Table ===\n");
  terminal_printf(&main_terminal, "Disk: %s\n",
                  pt->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA" : "IDE");
  terminal_printf(&main_terminal, "Total partitions: %u\n\n",
                  pt->partition_count);

  for (uint32_t i = 0; i < pt->partition_count; i++) {
    partition_info_t *p = &pt->partitions[i];
    terminal_printf(&main_terminal, "Partition %u:\n", p->index);
    terminal_printf(&main_terminal, "  Type: %s (0x%02X)\n",
                    partition_type_name(p->type), p->type);
    terminal_printf(&main_terminal, "  Start LBA: %llu\n", p->lba_start);
    terminal_printf(&main_terminal, "  Sectors: %llu\n", p->sector_count);
    terminal_printf(&main_terminal, "  Size: %llu MB\n", p->size_mb);
    terminal_printf(&main_terminal, "  Bootable: %s\n",
                    p->bootable ? "Yes" : "No");
    terminal_printf(&main_terminal, "\n");
  }
}

// Find bootable partition
partition_info_t *partition_find_bootable(partition_table_t *pt) {
  if (!pt)
    return NULL;

  for (uint32_t i = 0; i < pt->partition_count; i++) {
    if (pt->partitions[i].bootable) {
      return &pt->partitions[i];
    }
  }

  return NULL;
}

// Find partition by type
partition_info_t *partition_find_by_type(partition_table_t *pt, uint8_t type) {
  if (!pt)
    return NULL;

  for (uint32_t i = 0; i < pt->partition_count; i++) {
    if (pt->partitions[i].type == type) {
      return &pt->partitions[i];
    }
  }

  return NULL;
}

// Set partition as bootable
part_err_t partition_set_bootable(partition_table_t *pt, uint8_t index) {
  if (!pt || index >= 4) {
    return PART_ERR_INVALID_INDEX;
  }

  // Clear all bootable flags
  for (int i = 0; i < 4; i++) {
    pt->mbr.partitions[i].status = 0x00;
  }

  // Set bootable flag on specified partition
  pt->mbr.partitions[index].status = PART_FLAG_BOOTABLE;

  // Update parsed info
  for (uint32_t i = 0; i < pt->partition_count; i++) {
    pt->partitions[i].bootable = (pt->partitions[i].index == index);
  }

  terminal_printf(&main_terminal, "PART: Set partition %u as bootable\n",
                  index);
  return PART_OK;
}

void partition_create_entry(mbr_partition_entry_t *entry, uint8_t type,
                            uint64_t start_lba, uint64_t sector_count,
                            bool bootable) {
  memset(entry, 0, sizeof(mbr_partition_entry_t));

  entry->status = bootable ? PART_FLAG_BOOTABLE : 0x00;
  entry->type = type;
  entry->lba_start = start_lba;
  entry->sector_count = sector_count;

  // Calcular CHS simplificado
  partition_lba_to_chs(start_lba, entry->first_chs);
  partition_lba_to_chs(start_lba + sector_count - 1, entry->last_chs);
}

uint64_t partition_find_free_space(partition_table_t *pt,
                                   uint64_t size_sectors) {
  if (!pt)
    return 0;

  // **CORRECCIÓN: Comportamiento diferente para size_sectors = 0 (max)**
  if (size_sectors == 0) {
    // Modo "max": calcular el espacio total disponible
    uint64_t last_used_sector = 2048; // Empezar después del MBR + alineación

    for (uint32_t i = 0; i < pt->partition_count; i++) {
      partition_info_t *part = &pt->partitions[i];
      if (part->type != PART_TYPE_EMPTY) {
        uint64_t partition_end = part->lba_start + part->sector_count;
        if (partition_end > last_used_sector) {
          last_used_sector = partition_end;
        }
      }
    }

    // Calcular espacio disponible desde last_used_sector hasta el final del
    // disco
    if (pt->disk && last_used_sector < pt->disk->sector_count) {
      uint64_t available_space = pt->disk->sector_count - last_used_sector;
      terminal_printf(&main_terminal,
                      "DEBUG: Available space calculation:\r\n");
      terminal_printf(&main_terminal, "  Last used sector: %llu\r\n",
                      last_used_sector);
      terminal_printf(&main_terminal, "  Disk sectors: %llu\r\n",
                      pt->disk->sector_count);
      terminal_printf(&main_terminal, "  Available: %llu sectors (%llu MB)\r\n",
                      available_space,
                      (available_space * 512ULL) / (1024 * 1024));
      return available_space;
    }
    return 0;
  } else {
    // Comportamiento normal: encontrar espacio para tamaño específico
    uint64_t current_pos = 2048; // Empezar después del MBR con alineación

    for (uint32_t i = 0; i < pt->partition_count; i++) {
      partition_info_t *part = &pt->partitions[i];
      if (part->type != PART_TYPE_EMPTY) {
        uint64_t gap_start = current_pos;
        uint64_t gap_size = part->lba_start - current_pos;

        if (gap_size >= size_sectors) {
          return gap_start;
        }

        current_pos = part->lba_start + part->sector_count;
      }
    }

    // Verificar espacio al final del disco
    if (pt->disk && current_pos + size_sectors <= pt->disk->sector_count) {
      return current_pos;
    }

    return 0; // No hay espacio suficiente
  }
}

void partition_lba_to_chs(uint64_t lba, uint8_t *chs) {
  // Conversión simplificada - asume geometría estándar
  uint32_t cylinders = lba / (16 * 63); // 16 heads, 63 sectors/track
  uint32_t temp = lba % (16 * 63);
  uint32_t heads = temp / 63;
  uint32_t sectors = temp % 63 + 1;

  if (cylinders > 1023)
    cylinders = 1023;

  chs[0] = heads & 0xFF;
  chs[1] = ((sectors & 0x3F) | ((cylinders >> 8) & 0xC0)) & 0xFF;
  chs[2] = cylinders & 0xFF;
}

void partition_clear_table(partition_table_t *pt) {
  if (!pt)
    return;

  memset(&pt->mbr.partitions, 0, sizeof(pt->mbr.partitions));
  memset(pt->partitions, 0, sizeof(pt->partitions));
  pt->partition_count = 0;
  pt->mbr.signature = 0xAA55;
}