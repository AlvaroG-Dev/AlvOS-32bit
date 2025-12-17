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

  // Leer sector 0
  disk_err_t err = disk_read_dispatch(disk, 0, 1, &pt->mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "PART: Failed to read sector 0 (error %d)\n", err);
    return PART_ERR_READ_FAILED;
  }

  // **Verificar bytes de firma individualmente**
  uint8_t *mbr_bytes = (uint8_t *)&pt->mbr;
  uint8_t sig_byte1 = mbr_bytes[510];
  uint8_t sig_byte2 = mbr_bytes[511];

  terminal_printf(&main_terminal, "\n=== Analyzing sector 0 ===\n");
  terminal_printf(&main_terminal, "Signature bytes 510-511: 0x%02X 0x%02X\n",
                  sig_byte1, sig_byte2);

  // **CORRECCIÓN CRÍTICA: Verificar firma PRIMERO**
  if (!(sig_byte1 == 0x55 && sig_byte2 == 0xAA)) {
    terminal_puts(&main_terminal, "✗ No valid boot sector signature\n");

    // Verificar si es FAT32 con firma corrupta
    terminal_puts(&main_terminal,
                  "Checking for FAT32 with corrupted signature...\n");

    uint8_t temp_sector[512];
    memcpy(temp_sector, mbr_bytes, 512);
    temp_sector[510] = 0x55;
    temp_sector[511] = 0xAA;

    bool is_fat32 = check_fat32_signature(temp_sector);

    if (is_fat32) {
      terminal_puts(&main_terminal,
                    "✓ FAT32 detected (boot signature was corrupted)\n");

      // Corregir la firma en el MBR
      pt->mbr.signature = 0xAA55;

      // Crear partición virtual como antes
      partition_info_t *info = &pt->partitions[0];
      info->index = 0;
      info->type = PART_TYPE_FAT32_LBA;
      info->bootable = true;
      info->lba_start = 0;
      info->sector_count = disk->sector_count;
      info->size_mb = (info->sector_count * 512ULL) / (1024 * 1024);
      info->is_extended = false;
      pt->partition_count = 1;

      mbr_partition_entry_t *entry = &pt->mbr.partitions[0];
      entry->status = PART_FLAG_BOOTABLE;
      entry->type = PART_TYPE_FAT32_LBA;
      entry->lba_start = 0;
      entry->sector_count = disk->sector_count;

      partition_lba_to_chs(0, entry->first_chs);
      partition_lba_to_chs(disk->sector_count - 1, entry->last_chs);

      terminal_printf(&main_terminal, "Created virtual partition: %llu MB\n",
                      info->size_mb);
      return PART_OK;
    }

    terminal_puts(&main_terminal, "✗ Not FAT32 either\n");
    return PART_ERR_INVALID_MBR;
  }

  terminal_puts(&main_terminal, "✓ Valid boot sector signature found\n");

  // **CORRECCIÓN: PRIMERO verificar tabla de particiones MBR**
  // Antes de verificar FAT32 sin tabla, verificar si hay tabla MBR válida

  pt->partition_count = 0;
  bool has_valid_partitions = false;

  // **CORRECCIÓN: Verificar si hay ALGUNA entrada no vacía en la tabla MBR**
  bool has_any_partition_entry = false;
  for (int i = 0; i < 4; i++) {
    mbr_partition_entry_t *entry = &pt->mbr.partitions[i];
    if (entry->type != PART_TYPE_EMPTY) {
      has_any_partition_entry = true;
      break;
    }
  }

  // **SI HAY ALGUNA ENTRADA en la tabla MBR, procesarla NORMALMENTE**
  // NO saltar directamente a FAT32 sin tabla
  if (has_any_partition_entry) {
    terminal_puts(&main_terminal,
                  "MBR has partition entries, processing them...\n");

    for (int i = 0; i < 4; i++) {
      mbr_partition_entry_t *entry = &pt->mbr.partitions[i];

      terminal_printf(&main_terminal,
                      "Partition entry %d: type=0x%02X, LBA=%u, size=%u\n", i,
                      entry->type, entry->lba_start, entry->sector_count);

      if (entry->type != PART_TYPE_EMPTY && entry->sector_count > 0) {
        has_valid_partitions = true;

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

        terminal_printf(&main_terminal, "✓ Found partition: %s, %llu MB\n",
                        partition_type_name(entry->type), info->size_mb);
      }
    }

    if (has_valid_partitions) {
      terminal_printf(&main_terminal, "✓ Found %u partition(s) in MBR table\n",
                      pt->partition_count);
      return PART_OK;
    } else {
      // Tiene entradas pero todas inválidas (tipo 0 o tamaño 0)
      terminal_puts(&main_terminal,
                    "✗ MBR has partition entries but all are invalid\n");

      // **CORRECCIÓN: Mostrar HEX dump de la tabla completa**
      terminal_puts(&main_terminal, "Full partition table (bytes 446-509):\n");
      for (int i = 0; i < 64; i += 16) {
        terminal_printf(&main_terminal, "  %03X: ", 446 + i);
        for (int j = 0; j < 16; j++) {
          terminal_printf(&main_terminal, "%02X ", mbr_bytes[446 + i + j]);
        }
        terminal_puts(&main_terminal, "\n");
      }

      return PART_ERR_NO_PARTITIONS;
    }
  }

  // **SOLO SI NO HAY NINGUNA ENTRADA en la tabla MBR, verificar FAT32 sin
  // tabla**
  terminal_puts(&main_terminal, "No MBR partition entries, checking if FAT32 "
                                "without partition table...\n");

  bool is_fat32 = check_fat32_signature(mbr_bytes);

  if (is_fat32) {
    // **CORRECCIÓN: Verificar que REALMENTE no hay tabla de particiones**
    // Doble verificación: bytes 446-509 deberían ser todos 0 (o casi)
    bool truly_no_partition_table = true;
    for (int i = 446; i < 510; i++) {
      if (mbr_bytes[i] != 0x00) {
        truly_no_partition_table = false;
        terminal_printf(&main_terminal, "  Byte %d is 0x%02X (not zero)\n", i,
                        mbr_bytes[i]);
        break;
      }
    }

    if (!truly_no_partition_table) {
      terminal_puts(
          &main_terminal,
          "✗ FAT32 signature found but partition table area is not empty\n");
      terminal_puts(
          &main_terminal,
          "  Treating as regular MBR with FAT32 filesystem in partition\n");

      // Procesar como MBR normal (aunque probablemente falle)
      for (int i = 0; i < 4; i++) {
        mbr_partition_entry_t *entry = &pt->mbr.partitions[i];
        if (entry->type != PART_TYPE_EMPTY && entry->sector_count > 0) {
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
                          "✓ Found MBR partition: %s, %llu MB\n",
                          partition_type_name(entry->type), info->size_mb);
        }
      }

      if (pt->partition_count > 0) {
        terminal_printf(&main_terminal, "✓ Found %u partition(s)\n",
                        pt->partition_count);
        return PART_OK;
      }

      return PART_ERR_NO_PARTITIONS;
    }

    // **ES FAT32 sin tabla de particiones (tabla completamente vacía)**
    terminal_puts(
        &main_terminal,
        "✓ Detected FAT32 directly on sector 0 (no partition table)\n");

    // Crear partición virtual
    partition_info_t *info = &pt->partitions[0];
    info->index = 0;
    info->type = PART_TYPE_FAT32_LBA;
    info->bootable = true;
    info->lba_start = 0;
    info->sector_count = disk->sector_count;
    info->size_mb = (info->sector_count * 512ULL) / (1024 * 1024);
    info->is_extended = false;
    pt->partition_count = 1;

    // Crear entrada MBR artificial (solo para consistencia interna)
    mbr_partition_entry_t *entry = &pt->mbr.partitions[0];
    entry->status = PART_FLAG_BOOTABLE;
    entry->type = PART_TYPE_FAT32_LBA;
    entry->lba_start = 0;
    entry->sector_count = disk->sector_count;

    partition_lba_to_chs(0, entry->first_chs);
    partition_lba_to_chs(disk->sector_count - 1, entry->last_chs);

    // La firma ya está correcta (0xAA55)
    terminal_printf(&main_terminal, "Created virtual partition: %llu MB\n",
                    info->size_mb);
    return PART_OK;
  }

  // **NO es FAT32 y no tiene entradas en la tabla MBR**
  terminal_puts(&main_terminal,
                "✗ Has boot signature but no valid partitions and not FAT32\n");

  // Dump para diagnóstico
  terminal_puts(&main_terminal, "First 32 bytes of partition table area:\n");
  for (int i = 0; i < 32; i++) {
    if (i % 16 == 0)
      terminal_printf(&main_terminal, "  %03X: ", 446 + i);
    terminal_printf(&main_terminal, "%02X ", mbr_bytes[446 + i]);
    if (i % 16 == 15)
      terminal_puts(&main_terminal, "\n");
  }

  if (32 % 16 != 0)
    terminal_puts(&main_terminal, "\n");

  return PART_ERR_NO_PARTITIONS;
}

// Write partition table to disk
part_err_t partition_write_table(partition_table_t *pt) {
  if (!pt || !pt->disk) {
    return PART_ERR_INVALID_DISK;
  }

  // Verificar firma antes de escribir
  if (pt->mbr.signature != 0xAA55) {
    terminal_printf(&main_terminal, "PART: Warning: Setting MBR signature\n");
    pt->mbr.signature = 0xAA55;
  }

  terminal_printf(&main_terminal, "PART: Writing partition table to disk...\n");
  terminal_printf(&main_terminal, "  Signature: 0x%04X\n", pt->mbr.signature);

  // Asegurar que la tabla de particiones está limpia
  for (int i = pt->partition_count; i < 4; i++) {
    memset(&pt->mbr.partitions[i], 0, sizeof(mbr_partition_entry_t));
  }

  // Mostrar lo que vamos a escribir
  for (int i = 0; i < 4; i++) {
    mbr_partition_entry_t *entry = &pt->mbr.partitions[i];
    if (entry->type != PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal,
                      "  Part %d: Type=0x%02X, LBA=%u, Sectors=%u\n", i,
                      entry->type, entry->lba_start, entry->sector_count);
    }
  }

  // Escribir MBR (sector 0)
  disk_err_t err = disk_write_dispatch(pt->disk, 0, 1, &pt->mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "PART: Failed to write MBR (error %d)\n",
                    err);
    return PART_ERR_WRITE_FAILED;
  }

  // **NUEVO: Flushear inmediatamente**
  disk_flush_dispatch(pt->disk);

  // Verificar escritura (con reintentos)
  mbr_t verify_mbr;
  int retries = 3;

  while (retries > 0) {
    err = disk_read_dispatch(pt->disk, 0, 1, &verify_mbr);
    if (err != DISK_ERR_NONE) {
      terminal_printf(
          &main_terminal,
          "PART: Cannot verify write (read error %d), retrying...\n", err);
      retries--;
      if (retries > 0) {
        // Reintentar escritura
        err = disk_write_dispatch(pt->disk, 0, 1, &pt->mbr);
        if (err != DISK_ERR_NONE) {
          terminal_printf(&main_terminal,
                          "PART: Retry write failed (error %d)\n", err);
          break;
        }
        disk_flush_dispatch(pt->disk);
      }
      continue;
    }

    // Verificar contenido
    if (memcmp(&pt->mbr, &verify_mbr, sizeof(mbr_t)) == 0) {
      terminal_puts(
          &main_terminal,
          "PART: Partition table written and verified successfully\n");

      // **NUEVO: Flushear nuevamente para asegurar**
      disk_flush_dispatch(pt->disk);
      return PART_OK;
    }

    terminal_printf(&main_terminal,
                    "PART: MBR verification failed (attempt %d/3)\n",
                    4 - retries);
    retries--;

    if (retries > 0) {
      // Reintentar escritura
      err = disk_write_dispatch(pt->disk, 0, 1, &pt->mbr);
      if (err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "PART: Retry write failed (error %d)\n",
                        err);
        break;
      }
      disk_flush_dispatch(pt->disk);
    }
  }

  // Mostrar diferencias en caso de error
  terminal_puts(&main_terminal,
                "PART: MBR verification failed - write corrupted!\n");

  // Mostrar diferencias
  for (int i = 0; i < 4; i++) {
    if (pt->mbr.partitions[i].type != verify_mbr.partitions[i].type) {
      terminal_printf(
          &main_terminal, "  Part %d mismatch: expected 0x%02X, got 0x%02X\n",
          i, pt->mbr.partitions[i].type, verify_mbr.partitions[i].type);
    }
  }

  // Intentar forzar un flush final
  disk_flush_dispatch(pt->disk);
  return PART_ERR_WRITE_FAILED;
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
