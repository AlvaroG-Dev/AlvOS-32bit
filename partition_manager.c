#include "partition_manager.h"
#include "fat32.h"
#include "kernel.h"
#include "mbr.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"
#include "vfs.h"

extern Terminal main_terminal;

static disk_partitions_t managed_disks[MAX_DISKS];
static uint32_t disk_count = 0;

static void print_mount_callback(const char *mountpoint, const char *fs_name,
                                 void *arg) {
  terminal_printf(&main_terminal, "  %s -> %s\r\n", mountpoint, fs_name);
}

uint64_t partition_calculate_next_start_lba(partition_table_t *pt) {
  if (!pt)
    return 2048; // Default: empezar en 1MB para mejor alineación

  uint64_t next_lba =
      2048; // Empezar en sector 2048 (1MB) para mejor alineación

  for (int i = 0; i < 4; i++) {
    mbr_partition_entry_t *entry = &pt->mbr.partitions[i];

    if (entry->type != PART_TYPE_EMPTY && entry->sector_count > 0) {
      uint64_t partition_end = entry->lba_start + entry->sector_count;
      if (partition_end > next_lba) {
        next_lba = partition_end;
      }
    }
  }

  // Alinear a múltiplo de 2048 (1MB) para mejor rendimiento
  if (next_lba % 2048 != 0) {
    next_lba = ((next_lba / 2048) + 1) * 2048;
  }

  return next_lba;
}

part_mgr_err_t partition_manager_init(void) {
  memset(managed_disks, 0, sizeof(managed_disks));
  disk_count = 0;

  terminal_puts(&main_terminal, "Partition Manager: Initialized\r\n");
  return PART_MGR_OK;
}

part_mgr_err_t partition_manager_scan_disk(disk_t *disk, uint32_t disk_id) {
  if (!disk || !disk->initialized || disk_id >= MAX_DISKS) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  // Verificar si el disco ya está gestionado
  for (uint32_t i = 0; i < disk_count; i++) {
    if (managed_disks[i].disk == disk) {
      terminal_printf(&main_terminal,
                      "Partition Manager: Disk %u already managed\r\n",
                      disk_id);
      return PART_MGR_OK;
    }
  }

  if (disk_count >= MAX_DISKS) {
    terminal_puts(&main_terminal,
                  "Partition Manager: Maximum disk count reached\r\n");
    return PART_MGR_ERR_INVALID_DISK;
  }

  disk_partitions_t *disk_part = &managed_disks[disk_count];
  disk_part->disk = disk;
  disk_part->disk_id = disk_id;

  // Leer tabla de particiones
  part_err_t err = partition_read_table(disk, &disk_part->partition_table);
  if (err != PART_OK) {
    terminal_printf(&main_terminal,
                    "Partition Manager: Failed to read partition table for "
                    "disk %u (error %d)\r\n",
                    disk_id, err);
    return PART_MGR_ERR_READ_FAILED;
  }

  disk_part->initialized = true;
  disk_count++;

  terminal_printf(&main_terminal,
                  "Partition Manager: Disk %u scanned, %u partitions found\r\n",
                  disk_id, disk_part->partition_table.partition_count);

  return PART_MGR_OK;
}

part_mgr_err_t
partition_manager_create_partition(uint32_t disk_id, uint8_t part_num,
                                   uint8_t type, uint64_t start_lba,
                                   uint64_t sector_count, bool bootable) {
  if (part_num >= 4) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  // **CORRECCIÓN 1: Verificar que las particiones se creen en orden**
  for (int i = 0; i < part_num; i++) {
    if (disk_part->partition_table.mbr.partitions[i].type == PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal,
                      "Partition Manager: Cannot create partition %u - "
                      "partition %u is empty\r\n",
                      part_num, i);
      return PART_MGR_ERR_INVALID_PARTITION;
    }
  }

  // **CORRECCIÓN 2: Calcular start_lba automáticamente si es 0**
  if (start_lba == 0) {
    start_lba = partition_calculate_next_start_lba(&disk_part->partition_table);
    if (start_lba == 0) {
      terminal_puts(
          &main_terminal,
          "Partition Manager: No space available for new partition\r\n");
      return PART_MGR_ERR_NO_SPACE;
    }
    terminal_printf(&main_terminal,
                    "Partition Manager: Auto-calculated start LBA: %llu\r\n",
                    start_lba);
  }

  // **CORRECCIÓN 3: Verificar límites del disco más estricta**
  if (start_lba >= disk_part->disk->sector_count) {
    terminal_printf(
        &main_terminal,
        "Partition Manager: Start LBA %llu beyond disk size %llu\r\n",
        start_lba, disk_part->disk->sector_count);
    return PART_MGR_ERR_LBA_OUT_OF_RANGE;
  }

  if (start_lba + sector_count > disk_part->disk->sector_count) {
    terminal_printf(&main_terminal,
                    "Partition Manager: Partition extends beyond disk (LBA "
                    "%llu + %llu > %llu)\r\n",
                    start_lba, sector_count, disk_part->disk->sector_count);
    return PART_MGR_ERR_NO_SPACE;
  }

  if (disk_part->partition_table.mbr.signature != 0xAA55) {
    disk_part->partition_table.mbr.signature = 0xAA55;
  }

  // **CORRECCIÓN 4: Verificación de superposición mejorada**
  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    partition_info_t *existing = &disk_part->partition_table.partitions[i];
    if (existing->type != PART_TYPE_EMPTY) {
      uint64_t existing_start = existing->lba_start;
      uint64_t existing_end = existing_start + existing->sector_count;
      uint64_t new_end = start_lba + sector_count;

      // Verificar cualquier tipo de superposición
      if ((start_lba >= existing_start && start_lba < existing_end) ||
          (new_end > existing_start && new_end <= existing_end) ||
          (start_lba <= existing_start && new_end >= existing_end)) {
        terminal_printf(&main_terminal,
                        "Partition Manager: Overlap detected with partition %u "
                        "(%llu-%llu)\r\n",
                        i, existing_start, existing_end);
        return PART_MGR_ERR_OVERLAP;
      }

      // **CORRECCIÓN 5: Verificar que no estamos creando en medio de otra
      // partición**
      if (start_lba > existing_start && start_lba < existing_end) {
        terminal_printf(
            &main_terminal,
            "Partition Manager: Start LBA inside existing partition %u\r\n", i);
        return PART_MGR_ERR_OVERLAP;
      }
    }
  }

  // **CORRECCIÓN 6: Cálculo CHS mejorado**
  mbr_partition_entry_t *entry =
      &disk_part->partition_table.mbr.partitions[part_num];

  entry->status = bootable ? PART_FLAG_BOOTABLE : 0x00;
  entry->type = type;
  entry->lba_start = start_lba;
  entry->sector_count = sector_count;

  // Calcular CHS real (simplificado pero mejor)
  partition_lba_to_chs(start_lba, entry->first_chs);
  partition_lba_to_chs(start_lba + sector_count - 1, entry->last_chs);

  // **CORRECCIÓN 7: Actualizar información parseada correctamente**
  // Buscar si ya existe información para esta partición
  bool found_existing = false;
  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    if (disk_part->partition_table.partitions[i].index == part_num) {
      partition_info_t *info = &disk_part->partition_table.partitions[i];
      info->type = type;
      info->bootable = bootable;
      info->lba_start = start_lba;
      info->sector_count = sector_count;
      info->size_mb = (sector_count * 512ULL) / (1024 * 1024);
      info->is_extended =
          (type == PART_TYPE_EXTENDED || type == PART_TYPE_EXTENDED_LBA);
      found_existing = true;
      break;
    }
  }

  // Si no existe, agregar nueva entrada
  if (!found_existing) {
    if (disk_part->partition_table.partition_count < 4) {
      partition_info_t *info =
          &disk_part->partition_table
               .partitions[disk_part->partition_table.partition_count];
      info->index = part_num;
      info->type = type;
      info->bootable = bootable;
      info->lba_start = start_lba;
      info->sector_count = sector_count;
      info->size_mb = (sector_count * 512ULL) / (1024 * 1024);
      info->is_extended =
          (type == PART_TYPE_EXTENDED || type == PART_TYPE_EXTENDED_LBA);
      disk_part->partition_table.partition_count++;
    }
  }

  // **CORRECCIÓN 8: Ordenar particiones por índice**
  // Asegurar que las particiones estén ordenadas en el array
  for (int i = 0; i < disk_part->partition_table.partition_count - 1; i++) {
    for (int j = i + 1; j < disk_part->partition_table.partition_count; j++) {
      if (disk_part->partition_table.partitions[i].index >
          disk_part->partition_table.partitions[j].index) {
        partition_info_t temp = disk_part->partition_table.partitions[i];
        disk_part->partition_table.partitions[i] =
            disk_part->partition_table.partitions[j];
        disk_part->partition_table.partitions[j] = temp;
      }
    }
  }

  // Escribir tabla de particiones actualizada
  part_err_t err = partition_write_table(&disk_part->partition_table);
  if (err != PART_OK) {
    terminal_printf(
        &main_terminal,
        "Partition Manager: Failed to write partition table (error %d)\r\n",
        err);

    // **NUEVO: Intentar lectura y comparación**
    partition_table_t verify_pt;
    part_err_t read_err = partition_read_table(disk_part->disk, &verify_pt);
    if (read_err == PART_OK) {
      terminal_puts(&main_terminal, "Current disk state:\r\n");
      partition_print_info(&verify_pt);
    }

    return PART_MGR_ERR_WRITE_FAILED;
  }

  // **NUEVO: Verificación adicional**
  partition_table_t verify_pt;
  part_err_t verify_err = partition_read_table(disk_part->disk, &verify_pt);
  if (verify_err == PART_OK) {
    bool match = true;
    for (int i = 0; i < 4; i++) {
      if (disk_part->partition_table.mbr.partitions[i].type !=
          verify_pt.mbr.partitions[i].type) {
        match = false;
        terminal_printf(&main_terminal,
                        "  WARNING: Partition %d mismatch after write\r\n", i);
      }
    }

    if (!match) {
      terminal_puts(&main_terminal, "  ERROR: Written data doesn't match!\r\n");
      // Reintentar escritura
      err = partition_write_table(&disk_part->partition_table);
      if (err != PART_OK) {
        terminal_puts(&main_terminal,
                      "  FATAL: Second write attempt also failed\r\n");
      }
    }
  }

  // **NUEVO: Flushear forzadamente**
  terminal_puts(&main_terminal, "Flushing disk cache...\r\n");
  disk_flush_dispatch(disk_part->disk);

  // Esperar un poco para asegurar escritura
  for (volatile int i = 0; i < 1000000; i++)
    ;

  terminal_printf(&main_terminal,
                  "Partition Manager: Created partition %u on disk %u\r\n",
                  part_num, disk_id);

  return PART_MGR_OK;
}

part_mgr_err_t partition_manager_delete_partition(uint32_t disk_id,
                                                  uint8_t part_num) {
  if (part_num >= 4) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  // **NUEVO: Leer MBR actual para diagnóstico**
  mbr_t current_mbr;
  disk_err_t d_err = disk_read_dispatch(disk_part->disk, 0, 1, &current_mbr);
  if (d_err == DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "Partition Manager: Current disk state before deletion:\r\n");
    terminal_printf(
        &main_terminal, "  Partition %u: Type=0x%02X, Bootable=%s\r\n",
        part_num, current_mbr.partitions[part_num].type,
        (current_mbr.partitions[part_num].status & 0x80) ? "Yes" : "No");
  }

  // Verificar que la partición existe
  partition_info_t *info = NULL;

  // Buscar en la información parseada
  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    if (disk_part->partition_table.partitions[i].index == part_num) {
      info = &disk_part->partition_table.partitions[i];
      break;
    }
  }

  // Si no se encontró en parsed info, verificar en MBR directamente
  if (!info || info->type == PART_TYPE_EMPTY) {
    if (current_mbr.partitions[part_num].type == PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal,
                      "Partition Manager: Partition %u is already empty\r\n",
                      part_num);
      return PART_MGR_OK;
    } else {
      // Hay discrepancia entre parsed info y MBR real
      terminal_puts(&main_terminal,
                    "Partition Manager: WARNING - Partition exists on disk but "
                    "not in parsed data\r\n");
    }
  }

  terminal_printf(
      &main_terminal,
      "Partition Manager: Deleting partition %u from disk %u...\r\n", part_num,
      disk_id);

  // **NUEVO: Mantener copia de seguridad del MBR original**
  mbr_t mbr_backup;
  memcpy(&mbr_backup, &disk_part->partition_table.mbr, sizeof(mbr_t));

  // Limpiar entrada en MBR
  mbr_partition_entry_t *entry =
      &disk_part->partition_table.mbr.partitions[part_num];

  terminal_printf(&main_terminal, "  Clearing entry %u: Type was 0x%02X\r\n",
                  part_num, entry->type);

  memset(entry, 0, sizeof(mbr_partition_entry_t));

  // Actualizar información parseada
  if (info) {
    info->type = PART_TYPE_EMPTY;
    info->bootable = false;
    info->lba_start = 0;
    info->sector_count = 0;
    info->size_mb = 0;
    info->is_extended = false;
  }

  // **NUEVO: Recalcular partition_count**
  uint32_t new_count = 0;
  for (int i = 0; i < 4; i++) {
    if (disk_part->partition_table.mbr.partitions[i].type != PART_TYPE_EMPTY) {
      new_count++;
    }
  }
  disk_part->partition_table.partition_count = new_count;

  // **NUEVO: Asegurar firma MBR válida**
  if (disk_part->partition_table.mbr.signature != 0xAA55) {
    terminal_puts(&main_terminal, "  Setting MBR signature to 0xAA55\r\n");
    disk_part->partition_table.mbr.signature = 0xAA55;
  }

  // **NUEVO: Mostrar lo que vamos a escribir**
  terminal_puts(&main_terminal, "  New MBR to write:\r\n");
  for (int i = 0; i < 4; i++) {
    entry = &disk_part->partition_table.mbr.partitions[i];
    if (entry->type != PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal,
                      "    Part %d: Type=0x%02X, LBA=%u, Sectors=%u\r\n", i,
                      entry->type, entry->lba_start, entry->sector_count);
    } else {
      terminal_printf(&main_terminal, "    Part %d: [EMPTY]\r\n", i);
    }
  }

  // **NUEVO: Escribir con verificación y reintentos**
  part_err_t err = PART_ERR_WRITE_FAILED;
  int attempts = 3;

  for (int attempt = 1; attempt <= attempts; attempt++) {
    terminal_printf(&main_terminal, "  Writing attempt %d/%d...\r\n", attempt,
                    attempts);

    err = partition_write_table(&disk_part->partition_table);
    if (err == PART_OK) {
      terminal_puts(&main_terminal, "  ✓ Write successful\r\n");
      break;
    } else {
      terminal_printf(&main_terminal, "  ✗ Write failed (error %d)\r\n", err);

      if (attempt < attempts) {
        terminal_puts(&main_terminal, "    Retrying...\r\n");
        // Pequeña pausa antes de reintentar
        for (volatile int i = 0; i < 500000; i++)
          ;
      }
    }
  }

  if (err != PART_OK) {
    terminal_printf(&main_terminal,
                    "Partition Manager: FATAL - Failed to write partition "
                    "table after %d attempts\r\n",
                    attempts);

    // **NUEVO: Restaurar copia de seguridad en memoria**
    memcpy(&disk_part->partition_table.mbr, &mbr_backup, sizeof(mbr_t));

    // Recalcular parsed info desde MBR restaurado
    disk_part->partition_table.partition_count = 0;
    for (int i = 0; i < 4; i++) {
      if (disk_part->partition_table.mbr.partitions[i].type !=
          PART_TYPE_EMPTY) {
        partition_info_t *p =
            &disk_part->partition_table
                 .partitions[disk_part->partition_table.partition_count];
        mbr_partition_entry_t *e =
            &disk_part->partition_table.mbr.partitions[i];

        p->index = i;
        p->type = e->type;
        p->bootable = (e->status == PART_FLAG_BOOTABLE);
        p->lba_start = e->lba_start;
        p->sector_count = e->sector_count;
        p->size_mb = (p->sector_count * 512ULL) / (1024 * 1024);
        p->is_extended = (e->type == PART_TYPE_EXTENDED ||
                          e->type == PART_TYPE_EXTENDED_LBA);

        disk_part->partition_table.partition_count++;
      }
    }

    return PART_MGR_ERR_WRITE_FAILED;
  }

  // **NUEVO: Verificación exhaustiva post-escritura**
  terminal_puts(&main_terminal, "  Verifying write...\r\n");

  mbr_t verify_mbr;
  d_err = disk_read_dispatch(disk_part->disk, 0, 1, &verify_mbr);

  if (d_err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "  ✗ Cannot verify (read error %d)\r\n",
                    d_err);
  } else {
    // Comparar byte a byte
    bool mismatch = false;
    for (int i = 0; i < 4; i++) {
      if (disk_part->partition_table.mbr.partitions[i].type !=
          verify_mbr.partitions[i].type) {
        terminal_printf(&main_terminal,
                        "  ✗ Part %d mismatch: expected 0x%02X, got 0x%02X\r\n",
                        i, disk_part->partition_table.mbr.partitions[i].type,
                        verify_mbr.partitions[i].type);
        mismatch = true;
      }
    }

    if (!mismatch) {
      terminal_puts(&main_terminal, "  ✓ Verification passed\r\n");
    } else {
      terminal_puts(&main_terminal, "  ✗ Verification failed\r\n");

      // **NUEVO: Intentar reparación automática**
      terminal_puts(&main_terminal, "  Attempting auto-repair...\r\n");
      err = partition_write_table(&disk_part->partition_table);
      if (err == PART_OK) {
        terminal_puts(&main_terminal, "  ✓ Auto-repair successful\r\n");
      } else {
        terminal_puts(&main_terminal, "  ✗ Auto-repair failed\r\n");
      }
    }
  }

  // **NUEVO: Flushear disco múltiples veces**
  terminal_puts(&main_terminal, "  Flushing disk cache...\r\n");
  for (int flush_attempt = 0; flush_attempt < 3; flush_attempt++) {
    disk_flush_dispatch(disk_part->disk);
    // Esperar entre flushes
    for (volatile int i = 0; i < 200000; i++)
      ;
  }

  // **NUEVO: Esperar para asegurar escritura física**
  terminal_puts(&main_terminal, "  Waiting for physical write...\r\n");
  for (volatile int i = 0; i < 1000000; i++)
    ;

  // **NUEVO: Lectura final de verificación**
  terminal_puts(&main_terminal, "  Final verification...\r\n");
  d_err = disk_read_dispatch(disk_part->disk, 0, 1, &verify_mbr);
  if (d_err == DISK_ERR_NONE &&
      verify_mbr.partitions[part_num].type == PART_TYPE_EMPTY) {
    terminal_puts(&main_terminal,
                  "  ✓ Partition successfully deleted from physical disk\r\n");
  } else if (d_err == DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "  ✗ WARNING: Partition still present on disk! Type: 0x%02X\r\n",
        verify_mbr.partitions[part_num].type);
  }

  terminal_printf(&main_terminal,
                  "Partition Manager: Deleted partition %u from disk %u\r\n",
                  part_num, disk_id);

  // **NUEVO: Mostrar estado final**
  terminal_printf(&main_terminal, "  Final partition count: %u\r\n",
                  disk_part->partition_table.partition_count);

  return PART_MGR_OK;
}

part_mgr_err_t partition_manager_format_partition(uint32_t disk_id,
                                                  uint8_t part_num,
                                                  const char *fs_type) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  partition_info_t *part_info =
      partition_manager_get_partition(disk_id, part_num);
  if (!part_info || part_info->type == PART_TYPE_EMPTY) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Crear disco virtual para la partición
  disk_t part_disk;
  disk_err_t d_err =
      disk_init_from_partition(&part_disk, disk_part->disk, part_info);
  if (d_err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "Partition Manager: Failed to create partition disk "
                    "wrapper (error %d)\r\n",
                    d_err);
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Formatear según el tipo de sistema de archivos
  if (strcmp(fs_type, "FAT32") == 0) {
    terminal_printf(
        &main_terminal,
        "Partition Manager: Formatting partition %u as FAT32...\r\n", part_num);

    // Generar nombre de volumen automático
    char volume_label[12];
    snprintf(volume_label, sizeof(volume_label), "DISK%u_PART%u", disk_id,
             part_num);

    // Formatear la partición como FAT32
    int format_result = fat32_format(&part_disk, volume_label);
    if (format_result != VFS_OK) {
      terminal_printf(&main_terminal,
                      "Partition Manager: FAT32 format failed (error %d)\r\n",
                      format_result);
      return PART_MGR_ERR_INVALID_PARTITION;
    }

    terminal_printf(&main_terminal,
                    "Partition Manager: Successfully formatted as FAT32 with "
                    "label '%s'\r\n",
                    volume_label);

  } else if (strcmp(fs_type, "FAT16") == 0) {
    terminal_printf(
        &main_terminal,
        "Partition Manager: FAT16 formatting not yet implemented\r\n");
    return PART_MGR_ERR_INVALID_PARTITION;
  } else {
    terminal_printf(&main_terminal,
                    "Partition Manager: Unsupported filesystem: %s\r\n",
                    fs_type);
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  disk_flush_dispatch(disk_part->disk);
  return PART_MGR_OK;
}

part_mgr_err_t partition_manager_set_bootable(uint32_t disk_id,
                                              uint8_t part_num, bool bootable) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  // Primero limpiar todas las flags bootable
  for (int i = 0; i < 4; i++) {
    disk_part->partition_table.mbr.partitions[i].status = 0x00;
  }

  // Establecer la partición especificada como bootable si se solicita
  if (bootable) {
    disk_part->partition_table.mbr.partitions[part_num].status =
        PART_FLAG_BOOTABLE;
  }

  // Actualizar información parseada
  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    disk_part->partition_table.partitions[i].bootable =
        (i == part_num) && bootable;
  }

  // Escribir tabla de particiones actualizada
  part_err_t err = partition_write_table(&disk_part->partition_table);
  if (err != PART_OK) {
    return PART_MGR_ERR_WRITE_FAILED;
  }

  terminal_printf(&main_terminal,
                  "Partition Manager: Partition %u %s bootable\r\n", part_num,
                  bootable ? "set as" : "unset as");

  disk_flush_dispatch(disk_part->disk);
  return PART_MGR_OK;
}

// Funciones de información
disk_partitions_t *partition_manager_get_disk(uint32_t disk_id) {
  for (uint32_t i = 0; i < disk_count; i++) {
    if (managed_disks[i].disk_id == disk_id) {
      return &managed_disks[i];
    }
  }
  return NULL;
}

partition_info_t *partition_manager_get_partition(uint32_t disk_id,
                                                  uint8_t part_num) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || part_num >= 4) {
    return NULL;
  }

  return &disk_part->partition_table.partitions[part_num];
}

uint32_t partition_manager_get_disk_count(void) { return disk_count; }

void partition_manager_list_disks(void) {
  terminal_puts(&main_terminal, "\r\n=== Managed Disks ===\r\n");

  if (disk_count == 0) {
    terminal_puts(&main_terminal, "No disks managed\r\n");
    return;
  }

  for (uint32_t i = 0; i < disk_count; i++) {
    disk_partitions_t *disk_part = &managed_disks[i];
    terminal_printf(
        &main_terminal, "Disk %u: %s, %llu sectors, %u partitions\r\n",
        disk_part->disk_id,
        disk_part->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA" : "IDE",
        disk_part->disk->sector_count,
        disk_part->partition_table.partition_count);
  }
}

void partition_manager_list_partitions(uint32_t disk_id) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part) {
    terminal_printf(&main_terminal, "Disk %u not found\r\n", disk_id);
    return;
  }

  terminal_printf(&main_terminal, "\r\n=== Partitions on Disk %u ===\r\n",
                  disk_id);

  if (disk_part->partition_table.partition_count == 0) {
    terminal_puts(&main_terminal, "No partitions found\r\n");
    return;
  }

  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    partition_info_t *part = &disk_part->partition_table.partitions[i];
    if (part->type != PART_TYPE_EMPTY) {
      terminal_printf(&main_terminal, "Partition %u:\r\n", i);
      terminal_printf(&main_terminal, "  Type: %s (0x%02X)\r\n",
                      partition_type_name(part->type), part->type);
      terminal_printf(&main_terminal, "  Start LBA: %llu\r\n", part->lba_start);
      terminal_printf(&main_terminal, "  Sectors: %llu\r\n",
                      part->sector_count);
      terminal_printf(&main_terminal, "  Size: %llu MB\r\n", part->size_mb);
      terminal_printf(&main_terminal, "  Bootable: %s\r\n",
                      part->bootable ? "Yes" : "No");
      terminal_printf(&main_terminal, "  Extended: %s\r\n",
                      part->is_extended ? "Yes" : "No");
      terminal_puts(&main_terminal, "\r\n");
    }
  }
}

part_mgr_err_t partition_manager_auto_mount_all(void) {
  terminal_puts(&main_terminal, "\r\n=== Partition Auto-mount ===\r\n");

  // **CORRECCIÓN: Crear directorios base necesarios**
  // Crear /mnt si no existe
  vfs_node_t *mnt_dir = NULL;
  if (vfs_mkdir("/mnt", &mnt_dir) != VFS_OK) {
    terminal_puts(&main_terminal,
                  "WARNING: /mnt already exists or cannot be created\r\n");
  } else {
    if (mnt_dir) {
      mnt_dir->refcount--;
      if (mnt_dir->refcount == 0 && mnt_dir->ops->release) {
        mnt_dir->ops->release(mnt_dir);
      }
    }
  }

  // Crear /home si no existe
  vfs_node_t *home_dir = NULL;
  if (vfs_mkdir("/home", &home_dir) != VFS_OK) {
    terminal_puts(&main_terminal, "WARNING: /home already exists\r\n");
  } else {
    if (home_dir) {
      home_dir->refcount--;
      if (home_dir->refcount == 0 && home_dir->ops->release) {
        home_dir->ops->release(home_dir);
      }
    }
  }

  uint32_t mounted_count = 0;
  uint32_t fat32_count = 0;
  disk_t *home_disk_instance =
      NULL; // Para guardar referencia al disco de /home
  partition_info_t *home_partition = NULL;

  // **PROCESAR CADA DISCO**
  for (uint32_t disk_id = 0; disk_id < partition_manager_get_disk_count();
       disk_id++) {
    disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
    if (!disk_part || !disk_part->initialized) {
      terminal_printf(&main_terminal,
                      "  Disk %u: Not initialized, skipping\r\n", disk_id);
      continue;
    }

    char disk_letter = 'a' + disk_id;
    terminal_printf(&main_terminal, "\r\nProcessing disk sd%c:\r\n",
                    disk_letter);

    // **CREAR DISPOSITIVO PARA EL DISCO COMPLETO EN /dev/**
    char disk_device[32];
    snprintf(disk_device, sizeof(disk_device), "/dev/sd%c", disk_letter);

    uint32_t minor_base = disk_id * 16;

    if (vfs_mknod(disk_device, VFS_DEV_BLOCK, 8, minor_base) == VFS_OK) {
      terminal_printf(&main_terminal, "  Created device: %s\r\n", disk_device);
    }

    // **PROCESAR CADA PARTICION**
    for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
      partition_info_t *part = &disk_part->partition_table.partitions[i];

      if (part->type == PART_TYPE_EMPTY) {
        continue;
      }

      terminal_printf(&main_terminal,
                      "  Partition %d: Type=0x%02X (%s), Size=%llu MB\r\n",
                      part->index + 1, part->type,
                      partition_type_name(part->type), part->size_mb);

      // **VERIFICAR SI ES FAT32**
      bool is_fat = partition_is_fat(part->type);

      if (is_fat) {
        fat32_count++;
        terminal_printf(&main_terminal, "    ✓ Detected as FAT filesystem\r\n");

        // **CREAR DISCO VIRTUAL PARA LA PARTICION**
        disk_t *part_disk = (disk_t *)kernel_malloc(sizeof(disk_t));
        if (!part_disk) {
          terminal_puts(&main_terminal, "      ERROR: Out of memory\r\n");
          continue;
        }

        disk_err_t d_err =
            disk_init_from_partition(part_disk, disk_part->disk, part);
        if (d_err != DISK_ERR_NONE) {
          terminal_printf(&main_terminal,
                          "      ERROR: Cannot create partition disk: %d\r\n",
                          d_err);
          kernel_free(part_disk);
          continue;
        }

        // **CREAR DISPOSITIVO EN /dev/**
        char part_device[32];
        snprintf(part_device, sizeof(part_device), "/dev/sd%c%d", disk_letter,
                 part->index + 1);

        uint32_t minor = minor_base + part->index + 1;

        if (vfs_mknod(part_device, VFS_DEV_BLOCK, 8, minor) != VFS_OK) {
          terminal_printf(&main_terminal,
                          "      WARNING: Cannot create device node %s\r\n",
                          part_device);
        } else {
          terminal_printf(&main_terminal, "      Created device: %s\r\n",
                          part_device);
        }

        // **VERIFICAR FIRMA FAT32**
        uint8_t boot_sector[512];
        d_err = disk_read_dispatch(part_disk, 0, 1, boot_sector);
        if (d_err != DISK_ERR_NONE) {
          terminal_printf(&main_terminal,
                          "      ERROR: Cannot read boot sector: %d\r\n",
                          d_err);
          kernel_free(part_disk);
          continue;
        }

        bool has_fat32_sig = check_fat32_signature(boot_sector);
        if (!has_fat32_sig) {
          terminal_printf(&main_terminal,
                          "      WARNING: No FAT32 signature found\r\n");
          // Aún así intentar montar si el tipo indica FAT32
        } else {
          terminal_puts(&main_terminal, "      ✓ FAT32 signature verified\r\n");
        }

        // **CORRECCIÓN: CREAR PUNTO DE MONTAJE EN /mnt/**
        char mount_point[64];
        snprintf(mount_point, sizeof(mount_point), "/mnt/sd%c%d", disk_letter,
                 part->index + 1);

        // Crear directorio de montaje
        vfs_node_t *mount_dir = NULL;
        if (vfs_mkdir(mount_point, &mount_dir) != VFS_OK) {
          terminal_printf(&main_terminal,
                          "      WARNING: Cannot create mount point %s\r\n",
                          mount_point);
        } else {
          if (mount_dir) {
            mount_dir->refcount--;
            if (mount_dir->refcount == 0 && mount_dir->ops->release) {
              mount_dir->ops->release(mount_dir);
            }
          }
        }

        // **INTENTAR MONTAR EN /mnt/**
        terminal_printf(&main_terminal,
                        "      Attempting to mount at %s...\r\n", mount_point);

        int mount_err = vfs_mount(mount_point, "fat32", part_disk);
        if (mount_err != VFS_OK) {
          // Intentar nombres alternativos
          const char *alt_names[] = {"FAT32", "fat", "FAT", NULL};
          for (int alt = 0; alt_names[alt] != NULL; alt++) {
            mount_err = vfs_mount(mount_point, alt_names[alt], part_disk);
            if (mount_err == VFS_OK) {
              terminal_printf(&main_terminal,
                              "      ✓ Mounted with name '%s'\r\n",
                              alt_names[alt]);
              mounted_count++;
              break;
            }
          }

          if (mount_err != VFS_OK) {
            terminal_puts(&main_terminal,
                          "      ✗ All mount attempts failed\r\n");
            kernel_free(part_disk);
            continue;
          }
        } else {
          terminal_printf(&main_terminal,
                          "      ✓ Successfully mounted at %s\r\n",
                          mount_point);
          mounted_count++;
        }

        // **CORRECCIÓN: DECIDIR QUÉ PARTICION VA EN /home**
        // Estrategia: Usar la partición más grande para /home
        if (!home_partition || part->size_mb > home_partition->size_mb) {
          home_partition = part;
          home_disk_instance = part_disk; // Guardar referencia
        }

      } else {
        terminal_printf(&main_terminal,
                        "    Skipping non-FAT partition: %s (0x%02X)\r\n",
                        partition_type_name(part->type), part->type);
      }
    }
  }

  // **CORRECCIÓN: MONTAR LA PARTICION ELEGIDA EN /home**
  if (home_disk_instance && home_partition) {
    terminal_printf(&main_terminal,
                    "\r\nSelected partition %u for /home (%llu MB)\r\n",
                    home_partition->index + 1, home_partition->size_mb);

    // **IMPORTANTE: Usar bind mount o montar la misma instancia**
    // Por ahora, creamos otro disco para /home (NO RECOMENDADO PERO FUNCIONAL)
    disk_t *home_disk_copy = (disk_t *)kernel_malloc(sizeof(disk_t));
    if (home_disk_copy) {
      // Encontrar el disk_part original
      disk_partitions_t *disk_part =
          partition_manager_get_disk(0); // Asumimos disco 0
      if (disk_part) {
        disk_err_t home_err = disk_init_from_partition(
            home_disk_copy, disk_part->disk, home_partition);
        if (home_err == DISK_ERR_NONE) {
          int home_mount_err = vfs_mount("/home", "fat32", home_disk_copy);
          if (home_mount_err == VFS_OK) {
            terminal_puts(&main_terminal, "      ✓ Mounted as /home\r\n");

            // **CREAR ENLACE SIMBÓLICO PARA CONVENIENCIA**
            char home_link[64];
            snprintf(home_link, sizeof(home_link), "/mnt/home");
            // Nota: Necesitarías implementar vfs_symlink
            terminal_printf(&main_terminal,
                            "      Access via: /mnt/sda%u and /home\r\n",
                            home_partition->index + 1);
          } else {
            terminal_printf(&main_terminal,
                            "      ERROR: Failed to mount /home: %d\r\n",
                            home_mount_err);
            kernel_free(home_disk_copy);
          }
        } else {
          kernel_free(home_disk_copy);
        }
      }
    }
  }

  // **RESUMEN**
  terminal_puts(&main_terminal,
                "\r\n========================================\r\n");
  terminal_puts(&main_terminal,
                "           AUTO-MOUNT COMPLETE           \r\n");
  terminal_puts(&main_terminal, "========================================\r\n");

  terminal_printf(&main_terminal, "Disks processed: %u\r\n",
                  partition_manager_get_disk_count());
  terminal_printf(&main_terminal, "FAT32 partitions detected: %u\r\n",
                  fat32_count);
  terminal_printf(&main_terminal, "FAT32 partitions mounted: %u\r\n",
                  mounted_count);

  if (mounted_count == 0 && fat32_count > 0) {
    terminal_puts(&main_terminal,
                  "\r\nERROR: FAT32 partitions found but none mounted!\r\n");
    terminal_puts(&main_terminal, "Possible issues:\r\n");
    terminal_puts(&main_terminal,
                  "  1. FAT32 driver not properly registered\r\n");
    terminal_puts(&main_terminal,
                  "  2. Partitions not formatted (no FAT32 signature)\r\n");
    terminal_puts(&main_terminal, "  3. VFS mount function failing\r\n");
  }

  // **MOSTRAR ESTRUCTURA CREADA**
  terminal_puts(&main_terminal, "\r\nCreated structure:\r\n");
  terminal_puts(&main_terminal, "  /dev/sd*      - Disk devices\r\n");
  terminal_puts(&main_terminal, "  /mnt/sd*      - Mount points\r\n");
  terminal_puts(&main_terminal, "  /home         - Home directory\r\n");

  // **MOSTRAR MONTAJES ACTIVOS**
  terminal_puts(&main_terminal, "\r\nActive mount points:\r\n");
  int total_mounts = vfs_list_mounts(print_mount_callback, NULL);
  if (total_mounts == 0) {
    terminal_puts(&main_terminal, "  (no active mounts)\r\n");
  }

  return PART_MGR_OK;
}

part_mgr_err_t partition_manager_mount_partition(uint32_t disk_id,
                                                 uint8_t part_num,
                                                 const char *mount_point,
                                                 const char *fs_type) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  partition_info_t *part_info =
      partition_manager_get_partition(disk_id, part_num);
  if (!part_info || part_info->type == PART_TYPE_EMPTY) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Crear disco virtual para la partición
  disk_t *part_disk = (disk_t *)kernel_malloc(sizeof(disk_t));
  if (!part_disk) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  disk_err_t d_err =
      disk_init_from_partition(part_disk, disk_part->disk, part_info);
  if (d_err != DISK_ERR_NONE) {
    kernel_free(part_disk);
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Montar en VFS
  int mount_err = vfs_mount(mount_point, fs_type, part_disk);
  if (mount_err != VFS_OK) {
    kernel_free(part_disk);
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  terminal_printf(&main_terminal,
                  "Partition Manager: Mounted partition %u at %s\r\n", part_num,
                  mount_point);

  return PART_MGR_OK;
}

// Verificación de tabla de particiones
bool partition_manager_verify_partition_table(uint32_t disk_id) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    terminal_printf(&main_terminal,
                    "Partition Manager: Disk %u does not have partitions or is "
                    "uninitialized\r\n",
                    disk_id);
    return false;
  }

  // Verificar firma MBR
  if (disk_part->partition_table.mbr.signature != 0xAA55) {
    return false;
  }

  // Verificar que no hay particiones superpuestas
  for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
    partition_info_t *part1 = &disk_part->partition_table.partitions[i];
    if (part1->type == PART_TYPE_EMPTY)
      continue;

    for (uint32_t j = i + 1; j < disk_part->partition_table.partition_count;
         j++) {
      partition_info_t *part2 = &disk_part->partition_table.partitions[j];
      if (part2->type == PART_TYPE_EMPTY)
        continue;

      uint64_t part1_end = part1->lba_start + part1->sector_count;
      uint64_t part2_end = part2->lba_start + part2->sector_count;

      if ((part1->lba_start >= part2->lba_start &&
           part1->lba_start < part2_end) ||
          (part1_end > part2->lba_start && part1_end <= part2_end)) {
        return false;
      }
    }
  }

  return true;
}

// Nueva función para formateo con parámetros avanzados
part_mgr_err_t partition_manager_format_partition_advanced(
    uint32_t disk_id, uint8_t part_num, const char *fs_type,
    uint16_t sectors_per_cluster, uint8_t num_fats, const char *volume_label) {
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    return PART_MGR_ERR_INVALID_DISK;
  }

  partition_info_t *part_info =
      partition_manager_get_partition(disk_id, part_num);
  if (!part_info || part_info->type == PART_TYPE_EMPTY) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Crear disco virtual para la partición
  disk_t part_disk;
  disk_err_t d_err =
      disk_init_from_partition(&part_disk, disk_part->disk, part_info);
  if (d_err != DISK_ERR_NONE) {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  // Formatear con parámetros específicos
  if (strcmp(fs_type, "FAT32") == 0) {
    terminal_printf(&main_terminal,
                    "Partition Manager: Advanced FAT32 format...\r\n");
    terminal_printf(&main_terminal, "  Sectors per cluster: %u\r\n",
                    sectors_per_cluster);
    terminal_printf(&main_terminal, "  Number of FATs: %u\r\n", num_fats);
    terminal_printf(&main_terminal, "  Volume label: %s\r\n",
                    volume_label ? volume_label : "(default)");

    int format_result = fat32_format_with_params(
        &part_disk, sectors_per_cluster, num_fats, volume_label);
    if (format_result != VFS_OK) {
      terminal_printf(
          &main_terminal,
          "Partition Manager: Advanced FAT32 format failed (error %d)\r\n",
          format_result);
      return PART_MGR_ERR_INVALID_PARTITION;
    }
  } else {
    return PART_MGR_ERR_INVALID_PARTITION;
  }

  return PART_MGR_OK;
}

/// COMANDOS TERMINAL PARA GESTIÓN DE PARTICIONES ///
// Función auxiliar para parsear argumentos
static int parse_uint32(const char *str, uint32_t *value) {
  if (!str || !*str)
    return 0;

  char *endptr;
  uint32_t result = strtoul(str, &endptr, 10);
  if (*endptr != '\0')
    return 0;

  *value = result;
  return 1;
}

static int parse_uint64(const char *str, uint64_t *value) {
  if (!str || !*str)
    return 0;

  char *endptr;
  uint64_t result = strtoull(str, &endptr, 10);
  if (*endptr != '\0')
    return 0;

  *value = result;
  return 1;
}

void part_list_command(Terminal *term, const char *args) {
  if (args[0] != '\0') {
    // Listar particiones de un disco específico
    uint32_t disk_id;
    if (!parse_uint32(args, &disk_id)) {
      terminal_printf(term, "part list: Invalid disk ID '%s'\r\n", args);
      return;
    }

    partition_manager_list_partitions(disk_id);
  } else {
    // Listar todos los discos
    partition_manager_list_disks();
  }
}

void part_space_command(Terminal *term, const char *args) {
  char disk_str[16];
  uint32_t disk_id = 0;

  // Parsear argumento opcional (disk ID)
  if (args[0] != '\0') {
    if (!parse_uint32(args, &disk_id)) {
      terminal_printf(term, "part space: Invalid disk ID '%s'\r\n", args);
      terminal_puts(term, "Usage: part space [disk_id]\r\n");
      return;
    }
  }

  // Si no se especificó disk_id, mostrar información de todos los discos
  if (args[0] == '\0') {
    uint32_t disk_count = partition_manager_get_disk_count();

    if (disk_count == 0) {
      terminal_puts(term, "No disks managed. Use 'part scan' first.\r\n");
      return;
    }

    terminal_puts(term, "=== Disk Space Information ===\r\n");

    for (uint32_t i = 0; i < disk_count; i++) {
      disk_partitions_t *disk_part = partition_manager_get_disk(i);
      if (!disk_part || !disk_part->initialized)
        continue;

      uint64_t next_start =
          partition_calculate_next_start_lba(&disk_part->partition_table);
      uint64_t available_space = 0;

      if (next_start < disk_part->disk->sector_count) {
        available_space = disk_part->disk->sector_count - next_start;
      }

      uint64_t used_space =
          next_start - 2048; // Restar espacio del MBR + alineación

      terminal_printf(term, "Disk %u:\r\n", i);
      terminal_printf(term, "  Type: %s\r\n",
                      disk_part->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA"
                                                                     : "IDE");
      terminal_printf(term, "  Total size: %llu MB\r\n",
                      (disk_part->disk->sector_count * 512ULL) / (1024 * 1024));
      terminal_printf(term, "  Used space: %llu MB\r\n",
                      (used_space * 512ULL) / (1024 * 1024));
      terminal_printf(term, "  Free space: %llu MB\r\n",
                      (available_space * 512ULL) / (1024 * 1024));
      terminal_printf(term, "  Next available LBA: %llu\r\n", next_start);
      terminal_printf(term, "  Partitions: %u\r\n",
                      disk_part->partition_table.partition_count);

      // Mostrar uso por partición
      if (disk_part->partition_table.partition_count > 0) {
        terminal_puts(term, "  Partition layout:\r\n");
        for (uint32_t j = 0; j < disk_part->partition_table.partition_count;
             j++) {
          partition_info_t *part = &disk_part->partition_table.partitions[j];
          if (part->type != PART_TYPE_EMPTY) {
            terminal_printf(
                term, "    Part %u: %s, %llu MB, LBA %llu-%llu%s\r\n",
                part->index, partition_type_name(part->type), part->size_mb,
                part->lba_start, part->lba_start + part->sector_count - 1,
                part->bootable ? " [BOOT]" : "");
          }
        }
      }
      terminal_puts(term, "\r\n");
    }

  } else {
    // Mostrar información específica de un disco
    disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
    if (!disk_part) {
      terminal_printf(
          term, "part space: Disk %u not found. Use 'part scan' first.\r\n",
          disk_id);
      return;
    }

    uint64_t next_start =
        partition_calculate_next_start_lba(&disk_part->partition_table);
    uint64_t available_space = 0;

    if (next_start < disk_part->disk->sector_count) {
      available_space = disk_part->disk->sector_count - next_start;
    }

    uint64_t used_space = next_start - 2048;
    uint32_t used_percent = (used_space * 100) / disk_part->disk->sector_count;
    uint32_t free_percent =
        (available_space * 100) / disk_part->disk->sector_count;

    terminal_printf(term, "=== Detailed Space Information - Disk %u ===\r\n",
                    disk_id);
    terminal_printf(term, "Disk Type: %s\r\n",
                    disk_part->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA"
                                                                   : "IDE");
    terminal_printf(term, "Total Sectors: %llu\r\n",
                    disk_part->disk->sector_count);
    terminal_printf(term, "Total Size: %llu MB\r\n",
                    (disk_part->disk->sector_count * 512ULL) / (1024 * 1024));

    terminal_puts(term, "\r");
    terminal_printf(term, "Used Space:  %llu MB (%u%%)\r\n",
                    (used_space * 512ULL) / (1024 * 1024), used_percent);
    terminal_printf(term, "Free Space:  %llu MB (%u%%)\r\n",
                    (available_space * 512ULL) / (1024 * 1024), free_percent);

    terminal_puts(term, "\r");
    terminal_printf(term, "Next Available LBA: %llu\r\n", next_start);
    terminal_printf(term, "Managed Partitions: %u\r\n",
                    disk_part->partition_table.partition_count);

    // Diagrama visual simple
    terminal_puts(term, "\rDisk Layout:\r\n");
    terminal_puts(term, "[MBR]");

    uint64_t current_pos = 2048;
    for (uint32_t i = 0; i < disk_part->partition_table.partition_count; i++) {
      partition_info_t *part = &disk_part->partition_table.partitions[i];
      if (part->type != PART_TYPE_EMPTY) {
        // Espacio entre particiones
        if (part->lba_start > current_pos) {
          uint64_t gap = part->lba_start - current_pos;
          uint32_t gap_mb = (gap * 512ULL) / (1024 * 1024);
          if (gap_mb > 0) {
            terminal_printf(term, "[%uMB FREE]", gap_mb);
          }
        }

        // Partición
        terminal_printf(term, "[Part%u:%lluMB%s]", part->index, part->size_mb,
                        part->bootable ? "*" : "");

        current_pos = part->lba_start + part->sector_count;
      }
    }

    // Espacio libre al final
    if (current_pos < disk_part->disk->sector_count) {
      uint64_t free_end = disk_part->disk->sector_count - current_pos;
      uint32_t free_end_mb = (free_end * 512ULL) / (1024 * 1024);
      if (free_end_mb > 0) {
        terminal_printf(term, "[%uMB FREE]", free_end_mb);
      }
    }

    terminal_puts(term, "\r\n");

    // Información detallada de particiones
    if (disk_part->partition_table.partition_count > 0) {
      terminal_puts(term, "\rPartition Details:\r\n");
      for (uint32_t i = 0; i < disk_part->partition_table.partition_count;
           i++) {
        partition_info_t *part = &disk_part->partition_table.partitions[i];
        if (part->type != PART_TYPE_EMPTY) {
          terminal_printf(term, "  Partition %u:\r\n", part->index);
          terminal_printf(term, "    Type: %s (0x%02X)\r\n",
                          partition_type_name(part->type), part->type);
          terminal_printf(term, "    Size: %llu MB\r\n", part->size_mb);
          terminal_printf(term, "    LBA Range: %llu - %llu\r\n",
                          part->lba_start,
                          part->lba_start + part->sector_count - 1);
          terminal_printf(term, "    Sectors: %llu\r\n", part->sector_count);
          terminal_printf(term, "    Bootable: %s\r\n",
                          part->bootable ? "Yes" : "No");
          terminal_printf(term, "    Extended: %s\r\n",
                          part->is_extended ? "Yes" : "No");
        }
      }
    }
  }
}

void part_create_command(Terminal *term, const char *args) {
  char disk_str[16], part_str[16], type_str[16], size_str[32];
  char bootable_str[16] = "0";

  // Parsear: disk part type size [bootable]
  int parsed = sscanf(args, "%15s %15s %15s %31s %15s", disk_str, part_str,
                      type_str, size_str, bootable_str);

  if (parsed < 4) {
    terminal_puts(term, "part create: Usage: part create <disk> <partition> "
                        "<type> <size> [bootable]\r\n");
    terminal_puts(term, "  disk: Disk ID (0, 1, ...)\r\n");
    terminal_puts(term, "  partition: Partition number (0-3)\r\n");
    terminal_puts(term, "  type: FAT32, FAT16, LINUX, etc.\r\n");
    terminal_puts(term, "  size: Size in MB or 'max' for remaining space\r\n");
    terminal_puts(term,
                  "  bootable: 1 for bootable, 0 for not (default: 0)\r\n");
    terminal_puts(term, "Examples:\r\n");
    terminal_puts(term,
                  "  part create 0 0 FAT32 100 1    # 100MB booteable\r\n");
    terminal_puts(
        term, "  part create 0 1 FAT32 max 0    # Usar espacio restante\r\n");
    return;
  }

  uint32_t disk_id, part_num;
  if (!parse_uint32(disk_str, &disk_id) || !parse_uint32(part_str, &part_num) ||
      part_num > 3) {
    terminal_printf(term, "part create: Invalid disk or partition number\r\n");
    return;
  }

  // Convertir tipo de partición
  uint8_t part_type;
  if (strcmp(type_str, "FAT32") == 0 || strcmp(type_str, "FAT32_LBA") == 0) {
    part_type = PART_TYPE_FAT32_LBA;
  } else if (strcmp(type_str, "FAT16") == 0 ||
             strcmp(type_str, "FAT16_LBA") == 0) {
    part_type = PART_TYPE_FAT16_LBA;
  } else if (strcmp(type_str, "LINUX") == 0) {
    part_type = PART_TYPE_LINUX;
  } else if (strcmp(type_str, "NTFS") == 0) {
    part_type = PART_TYPE_NTFS;
  } else if (strcmp(type_str, "EXTENDED") == 0) {
    part_type = PART_TYPE_EXTENDED_LBA;
  } else {
    terminal_printf(term, "part create: Unsupported partition type: %s\r\n",
                    type_str);
    terminal_puts(term,
                  "Supported types: FAT32, FAT16, LINUX, NTFS, EXTENDED\r\n");
    return;
  }

  // Obtener información del disco
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part) {
    terminal_printf(
        term, "part create: Disk %u not found. Use 'part scan' first.\r\n",
        disk_id);
    return;
  }

  // **CORRECCIÓN COMPLETA: Manejo separado para "max" vs tamaño específico**
  uint64_t size_sectors;
  uint64_t start_lba;

  if (strcmp(size_str, "max") == 0) {
    // **MODO "max": Calcular espacio disponible y posición automáticamente**

    // 1. Calcular dónde debe empezar la nueva partición
    start_lba = partition_calculate_next_start_lba(&disk_part->partition_table);
    if (start_lba == 0) {
      terminal_puts(term, "part create: Cannot calculate start position\r\n");
      return;
    }

    // 2. Calcular cuánto espacio hay disponible desde esa posición
    if (start_lba >= disk_part->disk->sector_count) {
      terminal_puts(term, "part create: No space available - disk is full\r\n");
      return;
    }

    size_sectors = disk_part->disk->sector_count - start_lba;

    // 3. Verificar que el espacio disponible es razonable (mínimo 1MB)
    if (size_sectors < 2048) {
      terminal_puts(term, "part create: Available space is less than 1MB\r\n");
      return;
    }

    terminal_printf(term, "Using maximum available space:\r\n");
    terminal_printf(term, "  Start LBA: %llu\r\n", start_lba);
    terminal_printf(term, "  Size: %llu sectors (%llu MB)\r\n", size_sectors,
                    (size_sectors * 512ULL) / (1024 * 1024));

  } else {
    // **MODO tamaño específico: Buscar espacio para el tamaño solicitado**

    // Parsear tamaño en MB
    uint32_t size_mb;
    if (!parse_uint32(size_str, &size_mb) || size_mb == 0) {
      terminal_printf(term, "part create: Invalid size '%s'\r\n", size_str);
      return;
    }

    size_sectors = (uint64_t)size_mb * 1024 * 1024 / 512;

    // Verificar que el tamaño es razonable
    if (size_sectors < 2048) { // Mínimo 1MB
      terminal_puts(term, "part create: Minimum partition size is 1MB\r\n");
      return;
    }

    if (size_sectors > disk_part->disk->sector_count) {
      terminal_printf(
          term, "part create: Size %uMB exceeds disk size %lluMB\r\n", size_mb,
          (disk_part->disk->sector_count * 512ULL) / (1024 * 1024));
      return;
    }

    // Buscar posición para este tamaño específico
    start_lba =
        partition_find_free_space(&disk_part->partition_table, size_sectors);
    if (start_lba == 0) {
      terminal_puts(term, "part create: Not enough contiguous free space\r\n");
      return;
    }

    terminal_printf(term, "Using specific size:\r\n");
    terminal_printf(term, "  Start LBA: %llu\r\n", start_lba);
    terminal_printf(term, "  Size: %llu sectors (%llu MB)\r\n", size_sectors,
                    (size_sectors * 512ULL) / (1024 * 1024));
  }

  bool bootable = (strcmp(bootable_str, "1") == 0);

  // Mostrar información final antes de crear
  terminal_printf(term, "Creating partition:\r\n");
  terminal_printf(term, "  Disk: %u\r\n", disk_id);
  terminal_printf(term, "  Partition: %u\r\n", part_num);
  terminal_printf(term, "  Type: %s (0x%02X)\r\n",
                  partition_type_name(part_type), part_type);
  terminal_printf(term, "  Start LBA: %llu\r\n", start_lba);
  terminal_printf(term, "  Sectors: %llu\r\n", size_sectors);
  terminal_printf(term, "  Size: %llu MB\r\n",
                  (size_sectors * 512ULL) / (1024 * 1024));
  terminal_printf(term, "  Bootable: %s\r\n", bootable ? "Yes" : "No");

  // Crear la partición
  part_mgr_err_t err = partition_manager_create_partition(
      disk_id, part_num, part_type, start_lba, size_sectors, bootable);
  if (err != PART_MGR_OK) {
    terminal_printf(
        term, "part create: Failed to create partition (error %d)\r\n", err);
    switch (err) {
    case PART_MGR_ERR_OVERLAP:
      terminal_puts(term, "  Reason: Overlaps with existing partition\r\n");
      break;
    case PART_MGR_ERR_NO_SPACE:
      terminal_puts(term, "  Reason: Not enough space\r\n");
      break;
    case PART_MGR_ERR_INVALID_PARTITION:
      terminal_puts(term, "  Reason: Invalid partition number or order\r\n");
      break;
    case PART_MGR_ERR_LBA_OUT_OF_RANGE:
      terminal_puts(term, "  Reason: LBA out of valid range\r\n");
      break;
    default:
      terminal_printf(term, "  Reason: Unknown error %d\r\n", err);
      break;
    }
    return;
  }

  terminal_printf(
      term, "part create: Successfully created partition %u on disk %u\r\n",
      part_num, disk_id);
}

void part_delete_command(Terminal *term, const char *args) {
  char disk_str[16], part_str[16];

  if (sscanf(args, "%15s %15s", disk_str, part_str) != 2) {
    terminal_puts(term,
                  "part delete: Usage: part delete <disk> <partition>\r\n");
    return;
  }

  uint32_t disk_id, part_num;
  if (!parse_uint32(disk_str, &disk_id) || !parse_uint32(part_str, &part_num) ||
      part_num > 3) {
    terminal_printf(term, "part delete: Invalid disk or partition number\r\n");
    return;
  }

  part_mgr_err_t err = partition_manager_delete_partition(disk_id, part_num);
  if (err != PART_MGR_OK) {
    terminal_printf(
        term, "part delete: Failed to delete partition (error %d)\r\n", err);
    return;
  }

  terminal_printf(term, "part delete: Deleted partition %u from disk %u\r\n",
                  part_num, disk_id);
}

void part_fix_order_command(Terminal *term, const char *args) {
  char disk_str[16];

  if (sscanf(args, "%15s", disk_str) != 1) {
    terminal_puts(term, "part fix-order: Usage: part fix-order <disk>\r\n");
    return;
  }

  uint32_t disk_id;
  if (!parse_uint32(disk_str, &disk_id)) {
    terminal_printf(term, "part fix-order: Invalid disk ID '%s'\r\n", disk_str);
    return;
  }

  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part) {
    terminal_printf(term, "part fix-order: Disk %u not found\r\n", disk_id);
    return;
  }

  terminal_printf(term, "Fixing partition order on disk %u...\r\n", disk_id);

  // Reordenar particiones
  mbr_partition_entry_t temp_partitions[4];
  memcpy(temp_partitions, disk_part->partition_table.mbr.partitions,
         sizeof(temp_partitions));

  // Limpiar tabla original
  memset(disk_part->partition_table.mbr.partitions, 0,
         sizeof(disk_part->partition_table.mbr.partitions));

  // Copiar particiones en orden
  int dest_index = 0;
  for (int src_index = 0; src_index < 4; src_index++) {
    if (temp_partitions[src_index].type != PART_TYPE_EMPTY) {
      memcpy(&disk_part->partition_table.mbr.partitions[dest_index],
             &temp_partitions[src_index], sizeof(mbr_partition_entry_t));
      dest_index++;
    }
  }

  // Recalcular información parseada
  disk_part->partition_table.partition_count = dest_index;
  for (int i = 0; i < dest_index; i++) {
    partition_info_t *info = &disk_part->partition_table.partitions[i];
    mbr_partition_entry_t *entry =
        &disk_part->partition_table.mbr.partitions[i];

    info->index = i;
    info->type = entry->type;
    info->bootable = (entry->status == PART_FLAG_BOOTABLE);
    info->lba_start = entry->lba_start;
    info->sector_count = entry->sector_count;
    info->size_mb = (info->sector_count * 512ULL) / (1024 * 1024);
    info->is_extended = (entry->type == PART_TYPE_EXTENDED ||
                         entry->type == PART_TYPE_EXTENDED_LBA);
  }

  // Escribir tabla corregida
  part_err_t err = partition_write_table(&disk_part->partition_table);
  if (err != PART_OK) {
    terminal_printf(term,
                    "part fix-order: Failed to write corrected partition table "
                    "(error %d)\r\n",
                    err);
    return;
  }

  terminal_puts(term, "Partition order fixed successfully\r\n");
  partition_manager_list_partitions(disk_id);
}

// part_format_command:
void part_format_command(Terminal *term, const char *args) {
  char disk_str[16], part_str[16], fs_type[16];
  char label[32] = "";

  // Parsear: disk part fs_type [label]
  int parsed =
      sscanf(args, "%15s %15s %15s %31s", disk_str, part_str, fs_type, label);

  if (parsed < 3) {
    terminal_puts(term, "part format: Usage: part format <disk> <partition> "
                        "<fs_type> [label]\r\n");
    terminal_puts(term, "  fs_type: FAT32, FAT16\r\n");
    terminal_puts(term, "  label: Volume label (optional, max 11 chars)\r\n");
    terminal_puts(term, "\r\nExamples:\r\n");
    terminal_puts(term, "  part format 0 0 FAT32 SYSTEM\r\n");
    terminal_puts(term, "  part format 0 1 FAT32 DATA\r\n");
    return;
  }

  uint32_t disk_id, part_num;
  if (!parse_uint32(disk_str, &disk_id) || !parse_uint32(part_str, &part_num) ||
      part_num > 3) {
    terminal_printf(term, "part format: Invalid disk or partition number\r\n");
    return;
  }

  // Verificar que el tipo de sistema de archivos es compatible
  if (strcmp(fs_type, "FAT32") != 0 && strcmp(fs_type, "FAT16") != 0) {
    terminal_printf(term, "part format: Unsupported filesystem: %s\r\n",
                    fs_type);
    return;
  }

  // Usar label si se proporcionó, sino string vacío
  const char *volume_label = (parsed >= 4 && label[0] != '\0') ? label : NULL;

  terminal_printf(term, "Formatting partition %u on disk %u as %s...\r\n",
                  part_num, disk_id, fs_type);

  // **LEER INFO DE LA PARTICION ANTES DE FORMATEAR**
  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part || !disk_part->initialized) {
    terminal_printf(term, "ERROR: Disk %u not found\r\n", disk_id);
    return;
  }

  partition_info_t *part_info =
      partition_manager_get_partition(disk_id, part_num);
  if (!part_info || part_info->type == PART_TYPE_EMPTY) {
    terminal_printf(term, "ERROR: Partition %u not found\r\n", part_num);
    return;
  }

  terminal_printf(
      term, "  Partition info: Start LBA=%llu, Sectors=%llu, Size=%llu MB\r\n",
      part_info->lba_start, part_info->sector_count, part_info->size_mb);

  // **CREAR DISCO VIRTUAL PARA LA PARTICION**
  disk_t part_disk;
  disk_err_t d_err =
      disk_init_from_partition(&part_disk, disk_part->disk, part_info);
  if (d_err != DISK_ERR_NONE) {
    terminal_printf(term, "ERROR: Cannot create partition disk wrapper: %d\r\n",
                    d_err);
    return;
  }

  // **VERIFICAR ESTADO ACTUAL (DEBUG)**
  uint8_t boot_sector[512];
  d_err = disk_read_dispatch(&part_disk, 0, 1, boot_sector);
  if (d_err == DISK_ERR_NONE) {
    terminal_puts(term, "  Current boot sector signature: ");
    terminal_printf(term, "0x%02X 0x%02X\r\n", boot_sector[510],
                    boot_sector[511]);

    // Mostrar tipo de filesystem si existe
    if (boot_sector[510] == 0x55 && boot_sector[511] == 0xAA) {
      terminal_puts(term, "  Filesystem type bytes: ");
      for (int i = 0x36; i < 0x3E; i++) {
        char c = boot_sector[i];
        terminal_putchar(term, (c >= 32 && c <= 126) ? c : '.');
      }
      terminal_puts(term, "\r\n");
    }
  }

  // **FORMATEAR**
  int format_result = -1;

  if (strcmp(fs_type, "FAT32") == 0) {
    terminal_puts(term, "  Formatting as FAT32...\r\n");

    // Generar nombre de volumen si no se proporcionó
    char default_label[12];
    if (!volume_label) {
      snprintf(default_label, sizeof(default_label), "DISK%u_P%u", disk_id,
               part_num);
      volume_label = default_label;
    }

    terminal_printf(term, "  Volume label: %s\r\n", volume_label);

    // Calcular sectores por cluster óptimo
    uint64_t total_sectors = part_info->sector_count;
    uint8_t sectors_per_cluster = 1;

    if (total_sectors > 1024 * 1024 * 1024 / 512) { // > 1GB
      sectors_per_cluster = 8;
    } else if (total_sectors > 512 * 1024 * 1024 / 512) { // > 512MB
      sectors_per_cluster = 4;
    } else if (total_sectors > 256 * 1024 * 1024 / 512) { // > 256MB
      sectors_per_cluster = 2;
    }

    terminal_printf(term, "  Sectors per cluster: %u\r\n", sectors_per_cluster);

    // Llamar a la función de formateo
    // Asegúrate de que fat32_format_with_params exista
    format_result = fat32_format_with_params(&part_disk, sectors_per_cluster, 2,
                                             volume_label);

    if (format_result == VFS_OK) {
      terminal_puts(term, "  ✓ FAT32 format successful\r\n");

      // **VERIFICAR QUE SE ESCRIBIÓ CORRECTAMENTE**
      d_err = disk_read_dispatch(&part_disk, 0, 1, boot_sector);
      if (d_err == DISK_ERR_NONE) {
        terminal_printf(
            term, "  New signature: 0x%02X 0x%02X %s\r\n", boot_sector[510],
            boot_sector[511],
            (boot_sector[510] == 0x55 && boot_sector[511] == 0xAA) ? "✓" : "✗");
      }
    }
  }

  if (format_result != VFS_OK) {
    terminal_printf(term, "ERROR: Format failed with code %d\r\n",
                    format_result);

    // **INTENTAR CON FORMATO SIMPLE SI EL AVANZADO FALLA**
    terminal_puts(term, "  Trying simple format...\r\n");
    format_result =
        fat32_format(&part_disk, volume_label ? volume_label : "NO_NAME");
  }

  if (format_result != VFS_OK) {
    terminal_printf(term, "ERROR: All format attempts failed: %d\r\n",
                    format_result);
    return;
  }

  terminal_printf(term,
                  "✓ Successfully formatted partition %u on disk %u as %s\r\n",
                  part_num, disk_id, fs_type);

  // **FORZAR SINCROIZACIÓN**
  disk_flush_dispatch(disk_part->disk);

  terminal_puts(term, "  Disk cache flushed\r\n");
}

void part_format_advanced_command(Terminal *term, const char *args) {
  char disk_str[16], part_str[16], fs_type[16], spc_str[16], fats_str[16];
  char label[32] = "";

  // Parsear: disk part fs_type spc fats [label]
  int parsed = sscanf(args, "%15s %15s %15s %15s %15s %31s", disk_str, part_str,
                      fs_type, spc_str, fats_str, label);

  if (parsed < 5) {
    terminal_puts(term, "part format-adv: Usage: part format-adv <disk> "
                        "<partition> <fs_type> <spc> <fats> [label]\r\n");
    terminal_puts(term, "  fs_type: FAT32\r\n");
    terminal_puts(term,
                  "  spc: Sectors per cluster (1,2,4,8,16,32,64,128)\r\n");
    terminal_puts(term, "  fats: Number of FATs (1 or 2)\r\n");
    terminal_puts(term, "  label: Volume label (optional)\r\n");
    return;
  }

  uint32_t disk_id, part_num, sectors_per_cluster, num_fats;
  if (!parse_uint32(disk_str, &disk_id) || !parse_uint32(part_str, &part_num) ||
      part_num > 3) {
    terminal_printf(term,
                    "part format-adv: Invalid disk or partition number\r\n");
    return;
  }

  if (!parse_uint32(spc_str, &sectors_per_cluster) ||
      !parse_uint32(fats_str, &num_fats)) {
    terminal_printf(
        term, "part format-adv: Invalid sectors per cluster or FAT count\r\n");
    return;
  }

  // Validar parámetros
  if (strcmp(fs_type, "FAT32") != 0) {
    terminal_printf(
        term, "part format-adv: Only FAT32 supported for advanced format\r\n");
    return;
  }

  if ((sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
      sectors_per_cluster > 128) {
    terminal_printf(
        term,
        "part format-adv: Sectors per cluster must be power of 2 and ≤128\r\n");
    return;
  }

  if (num_fats == 0 || num_fats > 2) {
    terminal_printf(term, "part format-adv: Number of FATs must be 1 or 2\r\n");
    return;
  }

  const char *volume_label = (parsed >= 6 && label[0] != '\0') ? label : NULL;

  part_mgr_err_t err = partition_manager_format_partition_advanced(
      disk_id, part_num, fs_type, sectors_per_cluster, num_fats, volume_label);
  if (err != PART_MGR_OK) {
    terminal_printf(
        term, "part format-adv: Failed to format partition (error %d)\r\n",
        err);
    return;
  }

  terminal_printf(
      term,
      "part format-adv: Formatted partition %u with custom parameters\r\n",
      part_num);
}

void part_mount_command(Terminal *term, const char *args) {
  char disk_str[16], part_str[16], mount_point[VFS_PATH_MAX];
  char fs_type[16] = "FAT32"; // Default

  // Parsear: disk part [mount_point] [fs_type]
  int parsed = sscanf(args, "%15s %15s %255s %15s", disk_str, part_str,
                      mount_point, fs_type);

  if (parsed < 2) {
    terminal_puts(term, "part mount: Usage: part mount <disk> <partition> "
                        "[mount_point] [fs_type]\r\n");
    terminal_puts(term,
                  "  mount_point: Mount path (default: /diskX/partY)\r\n");
    terminal_puts(term, "  fs_type: Filesystem type (default: FAT32)\r\n");
    return;
  }

  uint32_t disk_id, part_num;
  if (!parse_uint32(disk_str, &disk_id) || !parse_uint32(part_str, &part_num) ||
      part_num > 3) {
    terminal_printf(term, "part mount: Invalid disk or partition number\r\n");
    return;
  }

  // Generar punto de montaje por defecto si no se proporciona
  if (parsed < 3) {
    snprintf(mount_point, sizeof(mount_point), "/disk%u/part%u", disk_id,
             part_num);
  }

  part_mgr_err_t err = partition_manager_mount_partition(disk_id, part_num,
                                                         mount_point, fs_type);
  if (err != PART_MGR_OK) {
    terminal_printf(
        term, "part mount: Failed to mount partition (error %d)\r\n", err);
    return;
  }

  terminal_printf(term, "part mount: Mounted partition %u on disk %u at %s\r\n",
                  part_num, disk_id, mount_point);
}

void part_info_command(Terminal *term, const char *args) {
  if (args[0] == '\0') {
    // Mostrar información general
    uint32_t disk_count = partition_manager_get_disk_count();
    terminal_printf(term, "Partition Manager: %u disk(s) managed\r\n",
                    disk_count);
    return;
  }

  char disk_str[16], part_str[16];

  if (sscanf(args, "%15s %15s", disk_str, part_str) == 2) {
    // Información de partición específica
    uint32_t disk_id, part_num;
    if (!parse_uint32(disk_str, &disk_id) ||
        !parse_uint32(part_str, &part_num) || part_num > 3) {
      terminal_printf(term, "part info: Invalid disk or partition number\r\n");
      return;
    }

    partition_info_t *part = partition_manager_get_partition(disk_id, part_num);
    if (!part || part->type == PART_TYPE_EMPTY) {
      terminal_printf(
          term, "part info: Partition %u on disk %u not found or empty\r\n",
          part_num, disk_id);
      return;
    }

    terminal_printf(term, "Partition %u on Disk %u:\r\n", part_num, disk_id);
    terminal_printf(term, "  Type: %s (0x%02X)\r\n",
                    partition_type_name(part->type), part->type);
    terminal_printf(term, "  Start LBA: %llu\r\n", part->lba_start);
    terminal_printf(term, "  Sectors: %llu\r\n", part->sector_count);
    terminal_printf(term, "  Size: %llu MB\r\n", part->size_mb);
    terminal_printf(term, "  Bootable: %s\r\n", part->bootable ? "Yes" : "No");
    terminal_printf(term, "  Extended: %s\r\n",
                    part->is_extended ? "Yes" : "No");

  } else {
    // Información de disco específico
    uint32_t disk_id;
    if (!parse_uint32(args, &disk_id)) {
      terminal_printf(term, "part info: Invalid disk ID '%s'\r\n", args);
      return;
    }

    disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
    if (!disk_part) {
      terminal_printf(term, "part info: Disk %u not found\r\n", disk_id);
      return;
    }

    terminal_printf(term, "Disk %u Information:\r\n", disk_id);
    terminal_printf(term, "  Type: %s\r\n",
                    disk_part->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA"
                                                                   : "IDE");
    terminal_printf(term, "  Total Sectors: %llu\r\n",
                    disk_part->disk->sector_count);
    terminal_printf(term, "  Total Size: %llu MB\r\n",
                    (disk_part->disk->sector_count * 512ULL) / (1024 * 1024));
    terminal_printf(term, "  Partitions: %u\r\n",
                    disk_part->partition_table.partition_count);
    terminal_printf(term, "  Table Valid: %s\r\n",
                    partition_manager_verify_partition_table(disk_id) ? "Yes"
                                                                      : "No");
  }
}

void part_scan_command(Terminal *term, const char *args) {
  if (args[0] != '\0') {
    terminal_puts(term, "part scan: Usage: part scan\r\n");
    return;
  }

  terminal_puts(term, "Scanning for disks and partitions...\r\n");

  // Aquí deberías escanear todos los discos disponibles
  // Por ahora, asumimos que main_disk está disponible
  part_mgr_err_t err = partition_manager_scan_disk(&main_disk, 0);
  if (err != PART_MGR_OK) {
    terminal_printf(term, "part scan: Failed to scan disk 0 (error %d)\r\n",
                    err);
    return;
  }

  terminal_puts(term, "Scan completed. Use 'part list' to see results.\r\n");
}

void part_help_command(Terminal *term, const char *args) {
  terminal_puts(term, "Partition Management Commands:\r\n");
  terminal_puts(
      term,
      "  part scan                    - Scan for disks and partitions\r\n");
  terminal_puts(
      term, "  part list [disk]             - List disks or partitions\r\n");
  terminal_puts(
      term, "  part info [disk] [partition] - Show partition information\r\n");
  terminal_puts(term,
                "  part create <args>           - Create new partition\r\n");
  terminal_puts(term, "  part delete <disk> <part>    - Delete partition\r\n");
  terminal_puts(term, "  part format <args>           - Format partition\r\n");
  terminal_puts(
      term,
      "  part format-adv <args>       - Advanced format with parameters\r\n");
  terminal_puts(term, "  part mount <args>            - Mount partition\r\n");
  terminal_puts(
      term, "  part auto-mount              - Auto-mount all partitions\r\n");
  terminal_puts(term,
                "  part show-all                - Show all partitions\r\n");
  terminal_puts(term, "  part help                    - Show this help\r\n");
}

// Comando para sincronizar particiones con disco
void part_sync_command(Terminal *term, const char *args) {
  char disk_str[16];
  uint32_t disk_id = 0;

  if (args[0] != '\0') {
    if (!parse_uint32(args, &disk_id)) {
      terminal_printf(term, "part sync: Invalid disk ID '%s'\n", args);
      return;
    }
  }

  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part) {
    terminal_printf(term, "part sync: Disk %u not found\n", disk_id);
    return;
  }

  terminal_printf(term, "Syncing partition table for disk %u...\n", disk_id);

  // Escribir tabla de particiones
  part_err_t err = partition_write_table(&disk_part->partition_table);
  if (err != PART_OK) {
    terminal_printf(term, "ERROR: Failed to sync partition table (error %d)\n",
                    err);
    return;
  }

  terminal_puts(term, "✓ Partition table synchronized to disk\n");
}

// Comando para forzar sincronización de todos los discos
void part_sync_all_command(Terminal *term, const char *args) {
  terminal_puts(term, "Syncing all disks...\n");

  uint32_t disk_count = partition_manager_get_disk_count();
  uint32_t synced_count = 0;

  for (uint32_t disk_id = 0; disk_id < disk_count; disk_id++) {
    disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
    if (!disk_part)
      continue;

    terminal_printf(term, "Disk %u: ", disk_id);

    part_err_t err = partition_write_table(&disk_part->partition_table);
    if (err == PART_OK) {
      terminal_puts(term, "✓ Synced\n");
      synced_count++;
    } else {
      terminal_printf(term, "✗ Failed (error %d)\n", err);
    }
  }

  terminal_printf(term, "\nSynced %u of %u disks\n", synced_count, disk_count);
}

// Comando para refrescar particiones desde disco
void part_refresh_command(Terminal *term, const char *args) {
  char disk_str[16];
  uint32_t disk_id = 0;

  if (args[0] != '\0') {
    if (!parse_uint32(args, &disk_id)) {
      terminal_printf(term, "part refresh: Invalid disk ID '%s'\n", args);
      return;
    }
  }

  disk_partitions_t *disk_part = partition_manager_get_disk(disk_id);
  if (!disk_part) {
    terminal_printf(term, "part refresh: Disk %u not found\n", disk_id);
    return;
  }

  terminal_printf(term, "Refreshing partition table for disk %u...\n", disk_id);

  // Re-leer desde disco
  part_err_t err =
      partition_read_table(disk_part->disk, &disk_part->partition_table);
  if (err != PART_OK) {
    terminal_printf(
        term, "ERROR: Failed to refresh partition table (error %d)\n", err);
    return;
  }

  terminal_puts(term, "✓ Partition table refreshed from disk\n");
  partition_manager_list_partitions(disk_id);
}