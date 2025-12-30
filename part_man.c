#include "fat32.h"
#include "kernel.h"
#include "mbr.h"
#include "partition_manager.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"
#include "vfs.h"


extern Terminal main_terminal;

static disk_partitions_t managed_disks[MAX_DISKS];
static uint32_t disk_count = 0;
extern vfs_mount_info_t *mount_list;

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

  // **VERIFICAR ESTADO INICIAL**
  terminal_puts(&main_terminal, "Checking existing mounts...\r\n");
  vfs_mount_info_t *current = mount_list;
  int existing_mounts = 0;
  while (current) {
    terminal_printf(&main_terminal, "  %s -> %s\r\n", current->mountpoint,
                    current->fs_type);
    current = current->next;
    existing_mounts++;
  }
  terminal_printf(&main_terminal, "Existing mounts: %d\r\n\r\n",
                  existing_mounts);

  // **CORRECCIÓN: Crear directorios base necesarios**
  // Crear /mnt si no existe
  vfs_node_t *mnt_dir = NULL;
  if (vfs_mkdir("/mnt", &mnt_dir) != VFS_OK) {
    terminal_puts(&main_terminal, "  /mnt already exists\r\n");
  } else {
    if (mnt_dir) {
      mnt_dir->refcount--;
      if (mnt_dir->refcount == 0 && mnt_dir->ops->release) {
        mnt_dir->ops->release(mnt_dir);
      }
    }
    terminal_puts(&main_terminal, "  Created /mnt directory\r\n");
  }

  // **NO CREAR /home AQUÍ - será creado por el mount si es necesario**

  uint32_t mounted_count = 0;
  uint32_t fat32_count = 0;
  disk_t *home_disk_instance = NULL;
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
        } else {
          terminal_puts(&main_terminal, "      ✓ FAT32 signature verified\r\n");
        }

        // **CREAR PUNTO DE MONTAJE EN /mnt/**
        char mount_point[64];
        snprintf(mount_point, sizeof(mount_point), "/mnt/sd%c%d", disk_letter,
                 part->index + 1);

        // Crear directorio de montaje si no existe
        vfs_node_t *mount_dir = NULL;
        if (vfs_mkdir(mount_point, &mount_dir) != VFS_OK) {
          terminal_printf(&main_terminal,
                          "      Mount point %s already exists\r\n",
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
        terminal_printf(&main_terminal, "      Mounting at %s...\r\n",
                        mount_point);

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

        // **SELECCIONAR PARTICION PARA /home**
        // Estrategia: Usar la primera partición FAT32 encontrada en el disco 0
        if (disk_id == 0 && !home_partition) {
          home_partition = part;
          home_disk_instance = part_disk; // Guardar referencia
          terminal_printf(&main_terminal,
                          "      Selected for /home candidate\r\n");
        } else {
          // Liberar disco si no es candidato para /home
          kernel_free(part_disk);
        }

      } else {
        terminal_printf(&main_terminal,
                        "    Skipping non-FAT partition: %s (0x%02X)\r\n",
                        partition_type_name(part->type), part->type);
      }
    }
  }

  // **MONTAR /home SI SE ENCONTRÓ UNA PARTICION APROPIADA**
  if (home_disk_instance && home_partition) {
    terminal_printf(&main_terminal,
                    "\r\nSelected partition %u for /home (%llu MB)\r\n",
                    home_partition->index + 1, home_partition->size_mb);

    // **VERIFICAR SI /home YA ESTÁ MONTADO**
    current = mount_list;
    vfs_mount_info_t *home_mount = NULL;

    while (current) {
      if (strcmp(current->mountpoint, "/home") == 0) {
        home_mount = current;
        break;
      }
      current = current->next;
    }

    if (home_mount) {
      // **DESMONTAR /home EXISTENTE**
      terminal_printf(&main_terminal, "  Unmounting existing /home (%s)...\r\n",
                      home_mount->fs_type);

      close_fds_for_mount(home_mount->sb);
      int unmount_result = vfs_unmount("/home");

      if (unmount_result != VFS_OK) {
        terminal_puts(&main_terminal,
                      "  WARNING: Failed to unmount existing /home\r\n");
        terminal_puts(&main_terminal, "  Will mount anyway...\r\n");
      } else {
        terminal_puts(&main_terminal, "  ✓ Existing /home unmounted\r\n");
      }

      for (volatile int i = 0; i < 100000; i++)
        ; // Pequeña pausa
    }

    // **MONTAR EN /home**
    terminal_puts(&main_terminal, "  Mounting to /home...\r\n");

    int home_mount_err = vfs_mount("/home", "fat32", home_disk_instance);
    if (home_mount_err == VFS_OK) {
      terminal_puts(&main_terminal, "      ✓ /home mounted successfully\r\n");

      // **CREAR ENLACE EN /mnt/home PARA FACILIDAD DE ACCESO**
      vfs_node_t *home_link_dir = NULL;
      if (vfs_mkdir("/mnt/home", &home_link_dir) == VFS_OK) {
        if (home_link_dir) {
          home_link_dir->refcount--;
          if (home_link_dir->refcount == 0 && home_link_dir->ops->release) {
            home_link_dir->ops->release(home_link_dir);
          }
        }
        terminal_puts(&main_terminal,
                      "      Also accessible via /mnt/home\r\n");
      }
    } else {
      terminal_printf(&main_terminal,
                      "      ERROR: Failed to mount /home: %d\r\n",
                      home_mount_err);

      // **FALLBACK: Montar solo en /mnt y crear alias**
      char fallback_mount[64];
      snprintf(fallback_mount, sizeof(fallback_mount), "/mnt/sda%d",
               home_partition->index + 1);

      terminal_printf(&main_terminal, "      Falling back to %s...\r\n",
                      fallback_mount);

      home_mount_err = vfs_mount(fallback_mount, "fat32", home_disk_instance);
      if (home_mount_err == VFS_OK) {
        terminal_printf(&main_terminal, "      ✓ Mounted at %s\r\n",
                        fallback_mount);
        terminal_printf(&main_terminal, "      Use: cd %s\r\n", fallback_mount);
      } else {
        terminal_printf(&main_terminal,
                        "      ERROR: Fallback also failed: %d\r\n",
                        home_mount_err);
      }
    }

    // **IMPORTANTE: NO liberar home_disk_instance - vfs_mount lo maneja**
  } else {
    terminal_puts(&main_terminal,
                  "\r\nNo suitable FAT32 partition found for /home\r\n");

    // **CREAR /home CON TMPFS COMO FALLBACK**
    vfs_node_t *home_dir = NULL;
    if (vfs_mkdir("/home", &home_dir) == VFS_OK) {
      if (home_dir) {
        home_dir->refcount--;
        if (home_dir->refcount == 0 && home_dir->ops->release) {
          home_dir->ops->release(home_dir);
        }
      }
      terminal_puts(&main_terminal,
                    "  Created empty /home directory (tmpfs)\r\n");
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