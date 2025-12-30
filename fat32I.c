// fat32.c
#include "disk.h"
#include "fat32.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"
#include "vfs.h"


extern Terminal main_terminal;

// Update the fat32_vnode_ops structure
static vnode_ops_t fat32_vnode_ops = {
    .lookup = fat32_lookup,
    .create = fat32_create,
    .mkdir = fat32_mkdir, // Updated to point to implemented function
    .read = fat32_read,
    .write = fat32_write,
    .readdir = fat32_readdir,
    .release = fat32_release,
    .unlink = fat32_unlink};

static int fat32_unmount(vfs_superblock_t *sb);

// FS type structure
vfs_fs_type_t fat32_fs_type = {
    .name = "fat32",
    .mount = fat32_mount,
    .unmount = fat32_unmount // Nueva
};

static inline uint16_t cpu_to_le16(uint16_t val) { return val; }
static inline uint32_t cpu_to_le32(uint32_t val) { return val; }
static inline uint16_t le16_to_cpu(uint16_t val) { return val; }
static inline uint32_t le32_to_cpu(uint32_t val) { return val; }

// Tabla para SPC óptimo basado en tamaño del disco (en MB)
static uint16_t get_optimal_spc(uint64_t total_sectors) {
  uint64_t total_mb = (total_sectors * 512) / (1024 * 1024);
  if (total_mb < 512)
    return 1;
  else if (total_mb < 1024)
    return 2;
  else if (total_mb < 2048)
    return 4;
  else if (total_mb < 4096)
    return 8;
  else if (total_mb < 8192)
    return 16;
  else if (total_mb < 16384)
    return 32;
  else if (total_mb < 32768)
    return 64;
  else
    return 128; // Para discos >32GB
}

// Nueva función helper para contar clusters en una cadena
static uint32_t fat32_count_clusters_in_chain(fat32_fs_t *fs,
                                              uint32_t first_cluster) {
  if (first_cluster < 2 || first_cluster >= fs->total_clusters + 2) {
    return 0;
  }

  uint32_t count = 0;
  uint32_t current = first_cluster;
  const uint32_t MAX_CLUSTERS = 65536; // Límite de seguridad

  while (current >= 2 && current < FAT32_EOC && count < MAX_CLUSTERS) {
    count++;
    current = fat32_get_fat_entry(fs, current);
    if (current == FAT32_BAD_CLUSTER || current == FAT32_FREE_CLUSTER) {
      terminal_printf(&main_terminal, "FAT32: Invalid cluster %u in chain\n",
                      current);
      break;
    }
  }

  return count;
}

// Nueva función helper para extender cadena de clusters
static int fat32_extend_cluster_chain(fat32_fs_t *fs, uint32_t first_cluster,
                                      uint32_t additional_clusters) {
  if (!fs || first_cluster < 2 || additional_clusters == 0) {
    return VFS_ERR;
  }

  // Find the last cluster in the chain
  uint32_t last_cluster = first_cluster;
  uint32_t next;

  while ((next = fat32_get_fat_entry(fs, last_cluster)) < FAT32_EOC) {
    if (next < 2 || next >= fs->total_clusters + 2) {
      terminal_printf(&main_terminal, "FAT32: Invalid cluster %u in chain\n",
                      next);
      return VFS_ERR;
    }
    last_cluster = next;
  }

  // Allocate and link additional clusters
  uint32_t prev_cluster = last_cluster;
  for (uint32_t i = 0; i < additional_clusters; i++) {
    uint32_t new_cluster = fat32_allocate_cluster(fs);
    if (new_cluster == FAT32_BAD_CLUSTER) {
      terminal_printf(&main_terminal,
                      "FAT32: Cannot allocate cluster %u of %u\n", i + 1,
                      additional_clusters);
      return VFS_ERR;
    }

    // Initialize the new cluster
    uint8_t *zero_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
    if (!zero_buffer) {
      fat32_free_cluster_chain(fs, new_cluster);
      return VFS_ERR;
    }
    memset(zero_buffer, 0, fs->cluster_size);

    if (fat32_write_cluster(fs, new_cluster, zero_buffer) != VFS_OK) {
      kernel_free(zero_buffer);
      fat32_free_cluster_chain(fs, new_cluster);
      return VFS_ERR;
    }
    kernel_free(zero_buffer);

    // Link previous cluster to this one
    if (fat32_set_fat_entry(fs, prev_cluster, new_cluster) != VFS_OK) {
      terminal_printf(&main_terminal, "FAT32: Cannot link cluster %u to %u\n",
                      prev_cluster, new_cluster);
      fat32_free_cluster_chain(fs, new_cluster);
      return VFS_ERR;
    }

    // Mark new cluster as EOC
    if (fat32_set_fat_entry(fs, new_cluster, FAT32_EOC) != VFS_OK) {
      terminal_printf(&main_terminal, "FAT32: Cannot mark cluster %u as EOC\n",
                      new_cluster);
      return VFS_ERR;
    }

    prev_cluster = new_cluster;

    // Flush periodically for large allocations
    if ((i + 1) % 8 == 0) {
      if (fat32_flush_fat_cache(fs) != VFS_OK) {
        terminal_printf(&main_terminal,
                        "FAT32: Failed to flush FAT cache during extension\n");
        return VFS_ERR;
      }
    }
  }

  // Final flush
  if (fat32_flush_fat_cache(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to flush FAT cache after extension\n");
    return VFS_ERR;
  }

  serial_printf(COM1_BASE,
                "FAT32: Successfully extended chain with %u clusters\n",
                additional_clusters);
  return VFS_OK;
}

void fat32_debug_fat(fat32_fs_t *fs, uint32_t start_cluster, uint32_t count) {
  serial_printf(COM1_BASE, "FAT32: Dumping FAT entries from cluster %u\n",
                start_cluster);
  for (uint32_t i = start_cluster;
       i < start_cluster + count && i < fs->total_clusters + 2; i++) {
    uint32_t entry = fat32_get_fat_entry(fs, i);
    serial_printf(COM1_BASE, "FAT32: Cluster %u -> 0x%08X\n", i, entry);
  }
}

static int fat32_validate_filesystem(fat32_fs_t *fs) {
  if (!fs || !fs->disk) {
    terminal_puts(&main_terminal,
                  "FAT32: Invalid parameters in validate_filesystem\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  terminal_printf(&main_terminal,
                  "FAT32: Starting filesystem validation (total_clusters=%u, "
                  "cluster_size=%u bytes)\n",
                  fs->total_clusters, fs->cluster_size);

  // 1. Validar todas las entradas de la FAT
  uint32_t invalid_clusters = 0;
  for (uint32_t cluster = 2; cluster < fs->total_clusters + 2; cluster++) {
    uint32_t entry = fat32_get_fat_entry(fs, cluster);
    // Fixed condition: Allow EOC as valid
    if (entry != FAT32_FREE_CLUSTER && entry != FAT32_EOC &&
        entry != FAT32_BAD_CLUSTER &&
        (entry < 2 || entry >= fs->total_clusters + 2)) {
      terminal_printf(
          &main_terminal,
          "FAT32: Invalid FAT entry for cluster %u: 0x%08X, marking as free\n",
          cluster, entry);
      if (fat32_set_fat_entry(fs, cluster, FAT32_FREE_CLUSTER) != VFS_OK) {
        terminal_printf(&main_terminal,
                        "FAT32: Failed to mark cluster %u as free\n", cluster);
        fs->has_errors = 1;
        return VFS_ERR;
      }
      invalid_clusters++;
    }
  }
  if (invalid_clusters > 0) {
    terminal_printf(&main_terminal, "FAT32: Corrected %u invalid FAT entries\n",
                    invalid_clusters);
    if (fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_puts(
          &main_terminal,
          "FAT32: Failed to flush FAT cache after correcting entries\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  // 2. Validar cadenas de clústeres para todos los archivos y directorios
  uint32_t current_cluster = fs->root_dir_cluster;
  uint32_t sector_offset = 0;
  uint32_t invalid_entries = 0;

  while (current_cluster < FAT32_EOC && current_cluster >= 2) {
    uint32_t sector =
        fat32_cluster_to_sector(fs, current_cluster) + sector_offset;
    uint8_t *buffer = (uint8_t *)kernel_malloc(FAT32_SECTOR_SIZE);
    if (!buffer) {
      terminal_puts(&main_terminal,
                    "FAT32: Failed to allocate buffer for directory scan\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }

    if (disk_read_dispatch(fs->disk, sector, 1, buffer) != DISK_ERR_NONE) {
      terminal_printf(&main_terminal,
                      "FAT32: Failed to read sector %u for directory scan\n",
                      sector);
      kernel_free(buffer);
      fs->has_errors = 1;
      return VFS_ERR;
    }

    fat32_dir_entry_t *entry = (fat32_dir_entry_t *)buffer;
    for (uint32_t i = 0; i < FAT32_ENTRIES_PER_SECTOR; i++) {
      if (entry->name[0] == 0x00)
        break; // Fin del directorio
      if (entry->name[0] == 0xE5) {
        entry++;
        continue;
      } // Entrada borrada
      if ((entry->attributes & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME) {
        entry++;
        continue;
      }

      char name[13];
      fat32_format_short_name(entry->name, name);
      uint32_t first_cluster = ((uint32_t)entry->first_cluster_high << 16) |
                               entry->first_cluster_low;

      if (first_cluster >= 2 && first_cluster < fs->total_clusters + 2) {
        uint32_t fat_entry = fat32_get_fat_entry(fs, first_cluster);
        if (fat_entry == FAT32_FREE_CLUSTER) {
          serial_printf(
              COM1_BASE,
              "FAT32: Cluster %u for %s is marked free, attempting recovery\n",
              first_cluster, name);
          // Intentar leer el clúster para verificar si los datos son válidos
          uint8_t *test_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
          bool readable = false;
          if (test_buffer &&
              fat32_read_cluster(fs, first_cluster, test_buffer) == VFS_OK) {
            readable = true;
            serial_printf(
                COM1_BASE,
                "FAT32: Cluster %u for %s is readable, attempting to recover\n",
                first_cluster, name);
          }
          kernel_free(test_buffer);

          if (readable) {
            // Reasignar un nuevo clúster
            uint32_t new_cluster = fat32_allocate_cluster(fs);
            if (new_cluster == FAT32_BAD_CLUSTER) {
              terminal_printf(&main_terminal,
                              "FAT32: Failed to allocate new cluster for %s, "
                              "truncating to zero\n",
                              name);
              entry->file_size = 0;
              entry->first_cluster_low = 0;
              entry->first_cluster_high = 0;
              if (disk_write_dispatch(fs->disk, sector, 1, buffer) !=
                  DISK_ERR_NONE) {
                terminal_printf(
                    &main_terminal,
                    "FAT32: Failed to write sector %u after truncating\n",
                    sector);
                kernel_free(buffer);
                fs->has_errors = 1;
                return VFS_ERR;
              }
              invalid_entries++;
            } else {
              entry->first_cluster_low = new_cluster & 0xFFFF;
              entry->first_cluster_high = (new_cluster >> 16) & 0xFFFF;
              if (fat32_set_fat_entry(fs, new_cluster, FAT32_EOC) != VFS_OK) {
                terminal_printf(
                    &main_terminal,
                    "FAT32: Failed to set FAT entry for new cluster %u\n",
                    new_cluster);
                fat32_free_cluster_chain(fs, new_cluster);
                kernel_free(buffer);
                fs->has_errors = 1;
                return VFS_ERR;
              }
              // Copiar datos del clúster original al nuevo
              uint8_t *data_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
              if (data_buffer && fat32_read_cluster(fs, first_cluster,
                                                    data_buffer) == VFS_OK) {
                if (fat32_write_cluster(fs, new_cluster, data_buffer) !=
                    VFS_OK) {
                  terminal_printf(
                      &main_terminal,
                      "FAT32: Failed to copy data to new cluster %u\n",
                      new_cluster);
                }
              }
              kernel_free(data_buffer);
              if (disk_write_dispatch(fs->disk, sector, 1, buffer) !=
                  DISK_ERR_NONE) {
                terminal_printf(
                    &main_terminal,
                    "FAT32: Failed to write sector %u after recovery\n",
                    sector);
                kernel_free(buffer);
                fs->has_errors = 1;
                return VFS_ERR;
              }
              invalid_entries++;
            }
          } else {
            // No recuperable, truncar a cero
            terminal_printf(
                &main_terminal,
                "FAT32: Cluster %u for %s not readable, truncating to zero\n",
                first_cluster, name);
            entry->file_size = 0;
            entry->first_cluster_low = 0;
            entry->first_cluster_high = 0;
            if (disk_write_dispatch(fs->disk, sector, 1, buffer) !=
                DISK_ERR_NONE) {
              terminal_printf(
                  &main_terminal,
                  "FAT32: Failed to write sector %u after truncating\n",
                  sector);
              kernel_free(buffer);
              fs->has_errors = 1;
              return VFS_ERR;
            }
            invalid_entries++;
          }
        } else {
          uint32_t chain_length = 0;
          int chain_ok =
              fat32_validate_cluster_chain(fs, first_cluster, &chain_length);
          if (chain_ok != VFS_OK) {
            serial_printf(COM1_BASE,
                          "FAT32: Invalid cluster chain for %s (length=%u), "
                          "attempting to repair\n",
                          name, chain_length);
            // Truncar la cadena al último clúster válido
            uint32_t temp_cluster = first_cluster;
            uint32_t last_valid_cluster = first_cluster;
            uint32_t valid_length = 1; // Incluye el primer cluster
            while (temp_cluster < FAT32_EOC && temp_cluster >= 2) {
              uint32_t next = fat32_get_fat_entry(fs, temp_cluster);
              // Fixed condition: Allow EOC as valid end
              if (next == FAT32_FREE_CLUSTER || next == FAT32_BAD_CLUSTER ||
                  (next < FAT32_EOC &&
                   (next < 2 || next >= fs->total_clusters + 2))) {
                serial_printf(COM1_BASE,
                              "FAT32: Repairing chain at cluster %u (invalid "
                              "next=0x%08X)\n",
                              temp_cluster, next);
                if (fat32_set_fat_entry(fs, last_valid_cluster, FAT32_EOC) !=
                    VFS_OK) {
                  terminal_printf(&main_terminal,
                                  "FAT32: Failed to set EOC for cluster %u\n",
                                  last_valid_cluster);
                  kernel_free(buffer);
                  fs->has_errors = 1;
                  return VFS_ERR;
                }
                // Liberar clústeres inválidos posteriores
                if (next < FAT32_EOC && next >= 2 &&
                    next < fs->total_clusters + 2) {
                  if (fat32_free_cluster_chain(fs, next) != VFS_OK) {
                    terminal_printf(&main_terminal,
                                    "FAT32: Failed to free invalid cluster "
                                    "chain starting at %u\n",
                                    next);
                    kernel_free(buffer);
                    fs->has_errors = 1;
                    return VFS_ERR;
                  }
                }
                if (fat32_flush_fat_cache(fs) != VFS_OK) {
                  terminal_printf(
                      &main_terminal,
                      "FAT32: Failed to flush FAT cache after truncation\n");
                  kernel_free(buffer);
                  fs->has_errors = 1;
                  return VFS_ERR;
                }
                break;
              }
              valid_length++;
              last_valid_cluster = temp_cluster;
              temp_cluster = next;
            }
            chain_length =
                valid_length; // Actualizar chain_length con la longitud válida

            // ✅ FIX: Si el archivo necesita más clusters que los disponibles,
            // EXTENDER
            uint32_t max_size = chain_length * fs->cluster_size;
            uint32_t file_size = le32_to_cpu(entry->file_size);
            if (!(entry->attributes & FAT32_ATTR_DIRECTORY) &&
                file_size > max_size) {
              terminal_printf(&main_terminal,
                              "FAT32: File %s size (%u) exceeds repaired chain "
                              "size (%u), attempting to EXTEND\n",
                              name, file_size, max_size);

              // Calcular clusters adicionales necesarios
              uint32_t required_clusters =
                  (file_size + fs->cluster_size - 1) / fs->cluster_size;
              uint32_t additional_needed = required_clusters - chain_length;

              if (additional_needed > 0) {
                // Intentar extender la cadena reparada
                if (fat32_extend_cluster_chain(fs, last_valid_cluster,
                                               additional_needed) == VFS_OK) {
                  terminal_printf(&main_terminal,
                                  "FAT32: Successfully extended repaired chain "
                                  "for %s by %u clusters "
                                  "(now %u clusters total)\n",
                                  name, additional_needed, required_clusters);

                  // ✅ CRÍTICO: Asegurar que el último cluster tenga EOF
                  // correctamente marcado
                  uint32_t last_cluster = last_valid_cluster;
                  for (uint32_t c = 0; c < additional_needed; c++) {
                    last_cluster = fat32_get_fat_entry(fs, last_cluster);
                  }

                  if (fat32_set_fat_entry(fs, last_cluster, FAT32_EOC) !=
                      VFS_OK) {
                    terminal_printf(
                        &main_terminal,
                        "FAT32: Failed to set EOF on last cluster %u for %s\n",
                        last_cluster, name);
                  }
                } else {
                  terminal_printf(
                      &main_terminal,
                      "FAT32: Could not extend repaired chain for %s, "
                      "adjusting file size to %u\n",
                      name, max_size);
                  // Solo como último recurso, ajustar el tamaño del archivo
                  entry->file_size = cpu_to_le32(max_size);
                }
              }

              if (disk_write_dispatch(fs->disk, sector, 1, buffer) !=
                  DISK_ERR_NONE) {
                terminal_printf(
                    &main_terminal,
                    "FAT32: Failed to write sector %u after chain repair\n",
                    sector);
                kernel_free(buffer);
                fs->has_errors = 1;
                return VFS_ERR;
              }
              invalid_entries++;
            }
          } else {
            // ✅ FIX: Cadena válida - VERIFICAR si necesita extensión
            uint32_t max_size = chain_length * fs->cluster_size;
            uint32_t file_size = le32_to_cpu(entry->file_size);

            if (!(entry->attributes & FAT32_ATTR_DIRECTORY) &&
                file_size > max_size) {
              terminal_printf(
                  &main_terminal,
                  "FAT32: File %s size (%u) exceeds valid chain length "
                  "(%u clusters, max %u bytes), EXTENDING chain...\n",
                  name, file_size, chain_length, max_size);

              // Calcular clusters adicionales necesarios
              uint32_t required_clusters =
                  (file_size + fs->cluster_size - 1) / fs->cluster_size;
              uint32_t additional_clusters = required_clusters - chain_length;

              if (additional_clusters > 0) {
                // EXTENDER la cadena de clusters existente
                if (fat32_extend_cluster_chain(fs, first_cluster,
                                               additional_clusters) == VFS_OK) {
                  terminal_printf(
                      &main_terminal,
                      "FAT32: Successfully extended %s by %u clusters "
                      "(now %u clusters total for %u bytes)\n",
                      name, additional_clusters, required_clusters, file_size);

                  // ✅ CRÍTICO: Verificar que todos los clusters estén bien
                  // enlazados Contar clusters después de la extensión para
                  // validación
                  uint32_t final_count =
                      fat32_count_clusters_in_chain(fs, first_cluster);
                  if (final_count != required_clusters) {
                    terminal_printf(
                        &main_terminal,
                        "FAT32: WARNING: Chain count mismatch after extension: "
                        "%u vs expected %u\n",
                        final_count, required_clusters);
                  }

                  // ✅ CRÍTICO: Asegurar que el último cluster tenga EOF
                  uint32_t current = first_cluster;
                  uint32_t last_cluster = current;
                  uint32_t clusters_traversed = 0;

                  while (current < FAT32_EOC && current >= 2 &&
                         clusters_traversed < required_clusters) {
                    last_cluster = current;
                    current = fat32_get_fat_entry(fs, current);
                    clusters_traversed++;
                  }

                  if (clusters_traversed == required_clusters) {
                    uint32_t last_entry = fat32_get_fat_entry(fs, last_cluster);
                    if (last_entry != FAT32_EOC) {
                      terminal_printf(
                          &main_terminal,
                          "FAT32: Setting EOF on last cluster %u for %s\n",
                          last_cluster, name);
                      fat32_set_fat_entry(fs, last_cluster, FAT32_EOC);
                    }
                  }

                  invalid_entries++;
                } else {
                  terminal_printf(
                      &main_terminal,
                      "FAT32: Failed to extend cluster chain for %s, "
                      "need %u additional clusters (insufficient free "
                      "space?)\n",
                      name, additional_clusters);
                  // Si no podemos extender, solo advertir pero no truncar
                  terminal_printf(&main_terminal,
                                  "FAT32: WARNING: File %s may be incomplete "
                                  "(%u bytes, only %u clusters available)\n",
                                  name, file_size, chain_length);
                }
              }
            }
          }
        }
      }
      entry++;
    }
    kernel_free(buffer);

    sector_offset++;
    if (sector_offset >= fs->boot_sector.sectors_per_cluster) {
      sector_offset = 0;
      current_cluster = fat32_get_fat_entry(fs, current_cluster);
    }
  }

  if (invalid_entries > 0) {
    terminal_printf(&main_terminal,
                    "FAT32: Corrected %u invalid directory entries\n",
                    invalid_entries);
    if (fat32_flush_dir_cache(fs) != VFS_OK) {
      terminal_puts(
          &main_terminal,
          "FAT32: Failed to flush directory cache after correcting entries\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  // 3. Recalcular FSInfo si es necesario
  if (fs->fsinfo.free_clusters == 0xFFFFFFFF ||
      fs->fsinfo.next_free_cluster == 0xFFFFFFFF || invalid_clusters > 0 ||
      invalid_entries > 0) {
    uint32_t free_clusters, next_free_cluster;
    if (fat32_calculate_free_clusters(fs, &free_clusters, &next_free_cluster) !=
        VFS_OK) {
      terminal_puts(&main_terminal,
                    "FAT32: Failed to recalculate free clusters\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
    fs->fsinfo.free_clusters = free_clusters;
    fs->fsinfo.next_free_cluster = next_free_cluster;
    if (fat32_update_fsinfo(fs) != VFS_OK) {
      terminal_puts(&main_terminal, "FAT32: Failed to update FSInfo\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  // 4. Marcar el sistema de archivos como limpio si no hay errores
  if (!fs->has_errors) {
    uint32_t fat1 = fat32_get_fat_entry(fs, 1);
    fat1 |= FAT32_CLN_SHUT_BIT_MASK; // Set bit27=1 (clean)
    fat1 |= FAT32_HRD_ERR_BIT_MASK;  // Set bit26=1 (no errors)
    fat1 &= 0x0FFFFFFF;              // Limpiar bits 28-31
    if (fat32_set_fat_entry(fs, 1, fat1) != VFS_OK) {
      terminal_puts(&main_terminal,
                    "FAT32: Failed to set clean shutdown bit\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
    if (fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_puts(
          &main_terminal,
          "FAT32: Failed to flush FAT cache after setting clean bit\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  terminal_puts(&main_terminal, "FAT32: Filesystem validation completed\n");
  return VFS_OK;
}

// ========================================================================
// CACHE HANDLING
// ========================================================================

int fat32_flush_fat_cache(fat32_fs_t *fs) {
  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, fat_cache=%p\n", fs,
                    fs ? fs->disk : NULL, fs ? fs->fat_cache : NULL);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }
  if (!fs->fat_cache_dirty || fs->fat_cache_sector == 0xFFFFFFFF) {
    return VFS_OK;
  }

  disk_err_t err =
      disk_write_dispatch(fs->disk, fs->fat_cache_sector, 1, fs->fat_cache);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to write primary FAT sector %u (error %d)\n",
                    fs->fat_cache_sector, err);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  for (uint8_t fat_num = 1; fat_num < fs->boot_sector.num_fats; fat_num++) {
    uint32_t backup_sector =
        fs->fat_cache_sector + (fat_num * fs->boot_sector.sectors_per_fat_32);
    err = disk_write_dispatch(fs->disk, backup_sector, 1, fs->fat_cache);
    if (err != DISK_ERR_NONE) {
      terminal_printf(
          &main_terminal,
          "FAT32: Failed to write backup FAT %u sector %u (error %d)\n",
          fat_num, backup_sector, err);
      fs->has_errors = 1;
    }
  }

  fs->fat_cache_dirty = 0;
  return VFS_OK;
}

int fat32_flush_dir_cache(fat32_fs_t *fs) {
  if (!fs->dir_cache_dirty || fs->dir_cache_sector == 0xFFFFFFFF) {
    return VFS_OK;
  }

  disk_err_t err =
      disk_write_dispatch(fs->disk, fs->dir_cache_sector, 1, fs->dir_cache);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to write dir cache to sector %u\r\n",
                    fs->dir_cache_sector);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  fs->dir_cache_dirty = 0;
  return VFS_OK;
}

int fat32_calculate_free_clusters(fat32_fs_t *fs, uint32_t *free_clusters,
                                  uint32_t *next_free_cluster) {
  if (!fs || !fs->disk || !free_clusters || !next_free_cluster) {
    terminal_puts(&main_terminal,
                  "FAT32: Invalid parameters in calculate_free_clusters\r\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  *free_clusters = 0;
  *next_free_cluster = 2;

  for (uint32_t cluster = 2; cluster < fs->total_clusters + 2; cluster++) {
    uint32_t entry = fat32_get_fat_entry(fs, cluster);
    if (entry == FAT32_FREE_CLUSTER) {
      (*free_clusters)++;
      if (*next_free_cluster == 2) {
        *next_free_cluster = cluster;
      }
    } else if (entry >= fs->total_clusters && entry != FAT32_EOC &&
               entry != FAT32_BAD_CLUSTER) {
      // Marcar clústeres inválidos como FREE, no EOC
      terminal_printf(
          &main_terminal,
          "FAT32: Invalid cluster %u (value %u), setting to FREE\r\n", cluster,
          entry);
      fat32_set_fat_entry(fs, cluster, FAT32_FREE_CLUSTER);
      (*free_clusters)++;
      if (*next_free_cluster == 2) {
        *next_free_cluster = cluster;
      }
    }
  }

  serial_printf(COM1_BASE,
                "FAT32: Calculated %u free clusters, next free: %u\r\n",
                *free_clusters, *next_free_cluster);
  return VFS_OK;
}

// ========================================================================
// VALIDATION
// ========================================================================

int fat32_validate_cluster_chain(fat32_fs_t *fs, uint32_t first_cluster,
                                 uint32_t *out_chain_length) {
  if (first_cluster < 2 || first_cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid first cluster %u in chain\n",
                    first_cluster);
    if (out_chain_length)
      *out_chain_length = 0;
    return VFS_ERR;
  }

  uint32_t current = first_cluster;
  uint32_t visited_count = 0; // Esto será la longitud
  const uint32_t MAX_CHAIN_LENGTH = 65536;

  while (current < FAT32_EOC && current >= 2 &&
         visited_count < MAX_CHAIN_LENGTH) {
    uint32_t next = fat32_get_fat_entry(fs, current);
    if (next == FAT32_FREE_CLUSTER) {
      terminal_printf(&main_terminal,
                      "FAT32: Cluster %u in chain is marked free\n", current);
      if (out_chain_length)
        *out_chain_length = visited_count;
      return VFS_ERR;
    }

    // Detección de ciclos (mismo código, pero suma visited_count antes)
    visited_count++; // Cuenta este cluster
    if (visited_count > 1 &&
        (visited_count % 1024) ==
            0) { // visited_count >1 para evitar false positive en primer
      uint32_t test_current = first_cluster;
      for (uint32_t i = 0; i < 1024 && test_current != current; i++) {
        test_current = fat32_get_fat_entry(fs, test_current);
        if (test_current == current) {
          terminal_printf(
              &main_terminal,
              "FAT32: Cycle detected in cluster chain at length %u\n",
              visited_count);
          if (out_chain_length)
            *out_chain_length = visited_count;
          return VFS_ERR;
        }
      }
    }

    if (next == FAT32_BAD_CLUSTER ||
        (next < FAT32_EOC && (next < 2 || next >= fs->total_clusters + 2))) {
      terminal_printf(&main_terminal,
                      "FAT32: Invalid cluster %u in chain (next=0x%08X), "
                      "length so far %u\n",
                      current, next, visited_count);
      if (out_chain_length)
        *out_chain_length = visited_count;
      return VFS_ERR;
    }
    current = next;
  }

  if (visited_count >= MAX_CHAIN_LENGTH) {
    terminal_printf(&main_terminal,
                    "FAT32: Cluster chain too long (%u), possible corruption\n",
                    visited_count);
    if (out_chain_length)
      *out_chain_length = visited_count;
    return VFS_ERR;
  }

  if (out_chain_length)
    *out_chain_length = visited_count;
  return VFS_OK;
}

int fat32_mount(void *device, vfs_superblock_t **out_sb) {
  if (!device || !out_sb) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Invalid device=%p or out_sb=%p\n", device,
                    out_sb);
    return VFS_ERR;
  }

  disk_t *disk = (disk_t *)device;
  if (!disk_is_initialized(disk)) {
    terminal_printf(&main_terminal, "fat32_mount: Disk not initialized\n");
    return VFS_ERR;
  }

  fat32_fs_t *fs = (fat32_fs_t *)kernel_malloc(sizeof(fat32_fs_t));
  if (!fs) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to allocate filesystem structure\n");
    return VFS_ERR;
  }
  memset(fs, 0, sizeof(fat32_fs_t));
  fs->disk = disk;
  fs->has_errors = 0;

  // Read boot sector
  if (fat32_read_boot_sector(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to read boot sector\n");
    kernel_free(fs);
    return VFS_ERR;
  }

  // Validate boot sector
  if (fs->boot_sector.bytes_per_sector != FAT32_SECTOR_SIZE ||
      fs->boot_sector.sectors_per_cluster == 0 ||
      fs->boot_sector.num_fats == 0 || fs->boot_sector.total_sectors_32 == 0 ||
      fs->boot_sector.sectors_per_fat_32 == 0) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Invalid boot sector: bytes_per_sector=%u, "
                    "sectors_per_cluster=%u, num_fats=%u, total_sectors=%u, "
                    "sectors_per_fat=%u\n",
                    fs->boot_sector.bytes_per_sector,
                    fs->boot_sector.sectors_per_cluster,
                    fs->boot_sector.num_fats, fs->boot_sector.total_sectors_32,
                    fs->boot_sector.sectors_per_fat_32);
    kernel_free(fs);
    return VFS_ERR;
  }

  // Initialize caches
  fs->fat_cache = (uint32_t *)kernel_malloc(FAT32_SECTOR_SIZE);
  fs->dir_cache = (uint8_t *)kernel_malloc(FAT32_SECTOR_SIZE);
  if (!fs->fat_cache || !fs->dir_cache) {
    terminal_printf(&main_terminal, "fat32_mount: Failed to allocate caches\n");
    if (fs->fat_cache)
      kernel_free(fs->fat_cache);
    if (fs->dir_cache)
      kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }
  memset(fs->fat_cache, 0, FAT32_SECTOR_SIZE);
  memset(fs->dir_cache, 0, FAT32_SECTOR_SIZE);
  fs->fat_cache_sector = 0xFFFFFFFF;
  fs->dir_cache_sector = 0xFFFFFFFF;
  fs->fat_cache_dirty = 0;
  fs->dir_cache_dirty = 0;

  // Calculate filesystem parameters
  fs->fat_start_sector = fs->boot_sector.reserved_sectors;
  fs->data_start_sector =
      fs->fat_start_sector +
      (fs->boot_sector.num_fats * fs->boot_sector.sectors_per_fat_32);
  fs->root_dir_cluster = fs->boot_sector.root_cluster;
  fs->cluster_size = fs->boot_sector.sectors_per_cluster * FAT32_SECTOR_SIZE;
  uint32_t data_sectors =
      fs->boot_sector.total_sectors_32 - fs->data_start_sector;
  fs->total_clusters = data_sectors / fs->boot_sector.sectors_per_cluster;

  // Validate calculated parameters
  if (fs->fat_start_sector >= fs->boot_sector.total_sectors_32 ||
      fs->data_start_sector >= fs->boot_sector.total_sectors_32 ||
      fs->total_clusters < 65526) {
    terminal_printf(
        &main_terminal,
        "fat32_mount: Invalid parameters: fat_start_sector=%u, "
        "data_start_sector=%u, total_sectors=%u, total_clusters=%u\n",
        fs->fat_start_sector, fs->data_start_sector,
        fs->boot_sector.total_sectors_32, fs->total_clusters);
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }

  // Read FSInfo
  if (fat32_read_fsinfo(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to read FSInfo sector\n");
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }

  // Validate and repair FAT[1]
  uint32_t fat1 = fat32_get_fat_entry(fs, 1);
  serial_printf(COM1_BASE, "fat32_mount: FAT[1]=0x%08X\n", fat1);
  if (fat1 == FAT32_BAD_CLUSTER || (fat1 & 0xF0000000) != 0x0FFFFFFF) {
    serial_printf(COM1_BASE,
                  "fat32_mount: Invalid FAT[1]=0x%08X, setting to 0x0FFFFFFF\n",
                  fat1);
    fat1 = 0x0FFFFFFF;
    fs->has_errors = 1;
    if (fat32_set_fat_entry(fs, 1, fat1) != VFS_OK ||
        fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_printf(&main_terminal, "fat32_mount: Failed to repair FAT[1]\n");
      kernel_free(fs->fat_cache);
      kernel_free(fs->dir_cache);
      kernel_free(fs);
      return VFS_ERR;
    }
  }

  // Set dirty bit
  uint32_t new_fat1 =
      (fat1 & ~FAT32_CLN_SHUT_BIT_MASK) | FAT32_HRD_ERR_BIT_MASK;
  new_fat1 &= 0x0FFFFFFF;
  if (fat32_set_fat_entry(fs, 1, new_fat1) != VFS_OK ||
      fat32_flush_fat_cache(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to set FAT[1]=0x%08X\n", new_fat1);
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }

  // Recalculate FSInfo if invalid
  if (fs->fsinfo.free_clusters == 0xFFFFFFFF ||
      fs->fsinfo.next_free_cluster == 0xFFFFFFFF) {
    serial_printf(COM1_BASE, "fat32_mount: Recalculating FSInfo\n");
    uint32_t free_clusters, next_free;
    if (fat32_calculate_free_clusters(fs, &free_clusters, &next_free) !=
        VFS_OK) {
      terminal_printf(&main_terminal,
                      "fat32_mount: Failed to calculate free clusters\n");
      kernel_free(fs->fat_cache);
      kernel_free(fs->dir_cache);
      kernel_free(fs);
      return VFS_ERR;
    }
    fs->fsinfo.free_clusters = free_clusters;
    fs->fsinfo.next_free_cluster = next_free;
    if (fat32_update_fsinfo(fs) != VFS_OK) {
      terminal_printf(&main_terminal, "fat32_mount: Failed to update FSInfo\n");
      kernel_free(fs->fat_cache);
      kernel_free(fs->dir_cache);
      kernel_free(fs);
      return VFS_ERR;
    }
  }

  // Initialize superblock
  vfs_superblock_t *sb =
      (vfs_superblock_t *)kernel_malloc(sizeof(vfs_superblock_t));
  if (!sb) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to allocate superblock\n");
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }
  memset(sb, 0, sizeof(vfs_superblock_t));
  strcpy(sb->fs_name, "fat32");
  sb->private = fs;
  sb->backing_device = device;

  // Initialize root node
  vfs_node_t *root = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
  if (!root) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to allocate root vnode\n");
    kernel_free(sb);
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }
  memset(root, 0, sizeof(vfs_node_t));
  strcpy(root->name, "/");
  root->type = VFS_NODE_DIR;
  root->ops = &fat32_vnode_ops;
  root->sb = sb;
  root->refcount = 1;

  fat32_node_t *root_data = (fat32_node_t *)kernel_malloc(sizeof(fat32_node_t));
  if (!root_data) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Failed to allocate root node data\n");
    kernel_free(root);
    kernel_free(sb);
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }
  memset(root_data, 0, sizeof(fat32_node_t));
  root_data->first_cluster = fs->root_dir_cluster;
  root_data->current_cluster = fs->root_dir_cluster;
  root_data->is_directory = 1;
  root_data->parent_cluster = 0;
  root->fs_private = root_data;

  sb->root = root;
  *out_sb = sb;

  if (fat32_validate_filesystem(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_mount: Filesystem validation failed\n");
    kernel_free(root_data);
    kernel_free(root);
    kernel_free(sb);
    kernel_free(fs->fat_cache);
    kernel_free(fs->dir_cache);
    kernel_free(fs);
    return VFS_ERR;
  }

  fat32_debug_fat(fs, 2, 10);
  serial_printf(COM1_BASE,
                "fat32_mount: Success, root cluster=%u, total clusters=%u\n",
                fs->root_dir_cluster, fs->total_clusters);
  return VFS_OK;
}

static int fat32_unmount(vfs_superblock_t *sb) {
  if (!sb) {
    terminal_puts(&main_terminal,
                  "FAT32: unmount failed: invalid superblock\r\n");
    return VFS_ERR;
  }

  fat32_fs_t *fs = (fat32_fs_t *)sb->private;
  if (!fs) {
    terminal_puts(&main_terminal,
                  "FAT32: unmount failed: invalid filesystem structure\r\n");
    return VFS_ERR;
  }

  int result = VFS_OK;
  terminal_puts(&main_terminal, "FAT32: Starting unmount process\r\n");
  fs->has_errors = 0;

  // Flush FAT cache primero
  if (fs->fat_cache && fs->fat_cache_sector != 0xFFFFFFFF &&
      fs->fat_cache_dirty) {
    if (fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_puts(&main_terminal,
                    "FAT32: Failed to flush FAT cache on unmount\r\n");
      fs->has_errors = 1;
      result = VFS_ERR;
    } else {
      terminal_puts(&main_terminal, "FAT32: FAT cache flushed\r\n");
    }
  }

  // Flush dir cache
  if (fs->dir_cache && fs->dir_cache_sector != 0xFFFFFFFF &&
      fs->dir_cache_dirty) {
    if (fat32_flush_dir_cache(fs) != VFS_OK) {
      terminal_puts(&main_terminal,
                    "FAT32: Failed to flush dir cache on unmount\r\n");
      fs->has_errors = 1;
      result = VFS_ERR;
    } else {
      terminal_puts(&main_terminal, "FAT32: Dir cache flushed\r\n");
    }
  }

  // Recalcular y actualizar FSInfo siempre
  uint32_t free_clusters, next_free;
  if (fat32_calculate_free_clusters(fs, &free_clusters, &next_free) == VFS_OK) {
    serial_printf(COM1_BASE,
                  "FAT32: Calculated %u free clusters, next free: %u\r\n",
                  free_clusters, next_free);
    if (free_clusters > fs->total_clusters) {
      terminal_puts(
          &main_terminal,
          "FAT32: Warning: Calculated free clusters exceed total clusters\r\n");
      fs->has_errors = 1;
      result = VFS_ERR;
    }
    if (free_clusters != fs->fsinfo.free_clusters ||
        next_free != fs->fsinfo.next_free_cluster) {
      terminal_puts(&main_terminal,
                    "FAT32: FSInfo mismatch detected, updating\r\n");
      fs->fsinfo.free_clusters = free_clusters;
      fs->fsinfo.next_free_cluster = next_free;
      if (fat32_update_fsinfo(fs) != VFS_OK) {
        terminal_puts(&main_terminal,
                      "FAT32: Failed to update FSInfo on unmount\r\n");
        fs->has_errors = 1;
        result = VFS_ERR;
      } else {
        terminal_puts(&main_terminal, "FAT32: FSInfo updated successfully\r\n");
      }
    } else {
      terminal_puts(&main_terminal,
                    "FAT32: FSInfo is consistent, no update needed\r\n");
    }
  } else {
    terminal_puts(&main_terminal,
                  "FAT32: Failed to recalculate free clusters on unmount\r\n");
    fs->has_errors = 1;
    result = VFS_ERR;
  }

  // Manejar FAT[1] - clean bit y error bit
  uint32_t fat1 = fat32_get_fat_entry(fs, 1);
  if (fat1 == FAT32_BAD_CLUSTER || (fat1 & 0xF0000000) != 0x0FFFFFFF) {
    serial_printf(
        COM1_BASE,
        "FAT32: Invalid FAT[1] value 0x%08X, repairing to 0x0FFFFFFF\r\n",
        fat1);
    fat1 = 0x0FFFFFFF; // Valor por defecto para FAT[1]
    if (fat32_set_fat_entry(fs, 1, fat1) != VFS_OK) {
      terminal_puts(&main_terminal, "FAT32: Failed to repair FAT[1]\r\n");
      fs->has_errors = 1;
      result = VFS_ERR;
    } else if (fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_puts(
          &main_terminal,
          "FAT32: Failed to flush FAT cache after repairing FAT[1]\r\n");
      fs->has_errors = 1;
      result = VFS_ERR;
    }
  }
  serial_printf(COM1_BASE, "FAT32: Current FAT[1]=0x%08X\r\n", fat1);

  uint32_t new_fat1 = fat1 & 0x0FFFFFFF; // Limpiar bits altos
  new_fat1 |= FAT32_CLN_SHUT_BIT_MASK;   // Set clean (bit 27 = 1)
  if (!fs->has_errors) {
    new_fat1 |= FAT32_HRD_ERR_BIT_MASK; // Set no errors (bit 26 = 1)
    serial_printf(COM1_BASE, "FAT32: Setting clean no-error FAT[1]=0x%08X\r\n",
                  new_fat1);
  } else {
    new_fat1 &= ~FAT32_HRD_ERR_BIT_MASK; // Set errors (bit 26 = 0)
    terminal_printf(&main_terminal,
                    "FAT32: Setting clean with-error FAT[1]=0x%08X\r\n",
                    new_fat1);
  }

  if (fat32_set_fat_entry(fs, 1, new_fat1) != VFS_OK) {
    terminal_puts(&main_terminal,
                  "FAT32: Failed to set FAT[1] bits on unmount\r\n");
    result = VFS_ERR;
  } else {
    // Flushear inmediatamente
    if (fat32_flush_fat_cache(fs) != VFS_OK) {
      terminal_puts(
          &main_terminal,
          "FAT32: Failed to flush FAT cache after setting FAT[1]\r\n");
      result = VFS_ERR;
    } else {
      terminal_puts(&main_terminal, "FAT32: FAT[1] updated successfully\r\n");
    }
  }

  // Flush completo del disco
  if (disk_flush_dispatch(fs->disk) != DISK_ERR_NONE) {
    terminal_puts(&main_terminal, "FAT32: Failed to flush disk on unmount\r\n");
    result = VFS_ERR;
  } else {
    terminal_puts(&main_terminal, "FAT32: Disk flushed successfully\r\n");
  }

  // Liberar recursos
  if (fs->fat_cache) {
    kernel_free(fs->fat_cache);
    fs->fat_cache = NULL;
  }
  if (fs->dir_cache) {
    kernel_free(fs->dir_cache);
    fs->dir_cache = NULL;
  }

  // Liberar nodo raíz si existe
  if (sb->root) {
    if (sb->root->fs_private) {
      kernel_free(sb->root->fs_private);
    }
    kernel_free(sb->root);
    sb->root = NULL;
  }

  kernel_free(fs);
  sb->private = NULL;

  terminal_puts(&main_terminal, "FAT32: Unmount completed\r\n");
  return result;
}

// ========================================================================
// BOOT SECTOR AND FSINFO
// ========================================================================

int fat32_read_boot_sector(fat32_fs_t *fs) {
  disk_err_t err = disk_read_dispatch(fs->disk, 0, 1, &fs->boot_sector);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to read boot sector (error %d)\r\n", err);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->boot_sector.boot_sector_signature != 0xAA55) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid boot sector signature: 0x%x\r\n",
                    fs->boot_sector.boot_sector_signature);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->boot_sector.bytes_per_sector != FAT32_SECTOR_SIZE) {
    terminal_printf(&main_terminal, "FAT32: Unsupported sector size: %u\r\n",
                    fs->boot_sector.bytes_per_sector);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->boot_sector.root_entries != 0) {
    terminal_puts(&main_terminal,
                  "FAT32: Root entries should be 0 for FAT32\r\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->boot_sector.sectors_per_fat_16 != 0) {
    terminal_puts(&main_terminal,
                  "FAT32: Sectors per FAT (16-bit) should be 0 for FAT32\r\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->boot_sector.sectors_per_fat_32 == 0) {
    terminal_puts(&main_terminal, "FAT32: Invalid sectors per FAT\r\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  uint32_t data_sectors =
      fs->boot_sector.total_sectors_32 -
      (fs->boot_sector.reserved_sectors +
       fs->boot_sector.num_fats * fs->boot_sector.sectors_per_fat_32);
  uint32_t total_clusters = data_sectors / fs->boot_sector.sectors_per_cluster;

  if (total_clusters < 65525) {
    terminal_puts(&main_terminal, "FAT32: Cluster count too low for FAT32\r\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  return VFS_OK;
}

int fat32_read_fsinfo(fat32_fs_t *fs) {
  if (!fs || !fs->disk) {
    terminal_printf(&main_terminal, "FAT32: Invalid fs=%p or disk=%p\n", fs,
                    fs ? fs->disk : NULL);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint8_t buffer[FAT32_SECTOR_SIZE];
  uint16_t fsinfo_sector =
      fs->boot_sector.fs_info_sector ? fs->boot_sector.fs_info_sector : 1;
  disk_err_t err = disk_read_dispatch(fs->disk, fsinfo_sector, 1, buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to read FSInfo sector %u (error %d)\n",
                    fsinfo_sector, err);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  memcpy(&fs->fsinfo, buffer, sizeof(fat32_fsinfo_t));
  if (fs->fsinfo.lead_signature != 0x41615252 ||
      fs->fsinfo.struct_signature != 0x61417272 ||
      fs->fsinfo.trail_signature != 0xAA550000) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid FSInfo signatures: lead=0x%08X, "
                    "struct=0x%08X, trail=0x%08X\n",
                    fs->fsinfo.lead_signature, fs->fsinfo.struct_signature,
                    fs->fsinfo.trail_signature);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  serial_printf(COM1_BASE,
                "FAT32: FSInfo read: free_clusters=%u, next_free_cluster=%u\n",
                fs->fsinfo.free_clusters, fs->fsinfo.next_free_cluster);
  return VFS_OK;
}

int fat32_update_fsinfo(fat32_fs_t *fs) {
  if (!fs || !fs->disk) {
    terminal_printf(&main_terminal, "FAT32: Invalid fs=%p or disk=%p\n", fs,
                    fs ? fs->disk : NULL);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->fsinfo.free_clusters == 0xFFFFFFFF ||
      fs->fsinfo.next_free_cluster == 0xFFFFFFFF) {
    uint32_t free_clusters, next_free_cluster;
    if (fat32_calculate_free_clusters(fs, &free_clusters, &next_free_cluster) !=
        VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: Failed to calculate free clusters\n");
      fs->has_errors = 1;
      return VFS_ERR;
    }
    fs->fsinfo.free_clusters = free_clusters;
    fs->fsinfo.next_free_cluster = next_free_cluster;
  }
  uint8_t buffer[FAT32_SECTOR_SIZE];
  memset(buffer, 0, FAT32_SECTOR_SIZE);
  memcpy(buffer, &fs->fsinfo, sizeof(fat32_fsinfo_t));
  uint16_t fsinfo_sector =
      fs->boot_sector.fs_info_sector ? fs->boot_sector.fs_info_sector : 1;
  disk_err_t err = disk_write_dispatch(fs->disk, fsinfo_sector, 1, buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "FAT32: Failed to write primary FSInfo sector %u (error %d)\n",
        fsinfo_sector, err);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint16_t backup_fsinfo = fs->boot_sector.backup_boot_sector
                               ? fs->boot_sector.backup_boot_sector + 1
                               : 7;
  err = disk_write_dispatch(fs->disk, backup_fsinfo, 1, buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "FAT32: Failed to write backup FSInfo sector %u (error %d)\n",
        backup_fsinfo, err);
    fs->has_errors = 1;
    // Continue despite backup failure to avoid blocking
  }
  serial_printf(
      COM1_BASE,
      "FAT32: FSInfo updated: free_clusters=%u, next_free_cluster=%u\n",
      fs->fsinfo.free_clusters, fs->fsinfo.next_free_cluster);
  return VFS_OK;
}

// ========================================================================
// FAT TABLE OPERATIONS
// ========================================================================

uint32_t fat32_get_fat_entry(fat32_fs_t *fs, uint32_t cluster) {
  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, fat_cache=%p, cluster=%u\n",
                    fs, fs ? fs->disk : NULL, fs ? fs->fat_cache : NULL,
                    cluster);
    if (fs)
      fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  if (fs->boot_sector.bytes_per_sector == 0) {
    terminal_printf(&main_terminal, "FAT32: Invalid bytes_per_sector=0\n");
    fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  if (cluster < 1 || cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid cluster %u (total_clusters=%u)\n", cluster,
                    fs->total_clusters);
    fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  uint32_t fat_offset = cluster * 4;
  uint32_t sector =
      fs->fat_start_sector + (fat_offset / fs->boot_sector.bytes_per_sector);
  uint32_t offset = fat_offset % fs->boot_sector.bytes_per_sector;
  if (sector >= fs->fat_start_sector + fs->boot_sector.sectors_per_fat_32) {
    terminal_printf(
        &main_terminal,
        "FAT32: Invalid FAT sector %u for cluster %u (fat_sectors=%u)\n",
        sector, cluster, fs->boot_sector.sectors_per_fat_32);
    fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  if (fs->fat_cache_sector != sector) {
    if (fs->fat_cache_dirty) {
      if (fat32_flush_fat_cache(fs) != VFS_OK) {
        terminal_printf(&main_terminal, "FAT32: Failed to flush FAT cache\n");
        fs->has_errors = 1;
        return FAT32_BAD_CLUSTER;
      }
    }
    disk_err_t err = disk_read_dispatch(fs->disk, sector, 1, fs->fat_cache);
    if (err != DISK_ERR_NONE) {
      terminal_printf(
          &main_terminal,
          "FAT32: Failed to read FAT sector %u for cluster %u (error %d)\n",
          sector, cluster, err);
      fs->has_errors = 1;
      return FAT32_BAD_CLUSTER;
    }
    fs->fat_cache_sector = sector;
  }
  uint32_t value = ((uint32_t *)fs->fat_cache)[offset / 4] & 0x0FFFFFFF;
  if (cluster != 1 && value != FAT32_FREE_CLUSTER && value != FAT32_EOC &&
      value != FAT32_BAD_CLUSTER && value >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid FAT entry value 0x%08X for cluster %u\n",
                    value, cluster);
    fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  if (cluster == 1) {
    // FAT[1] tiene reglas especiales - solo verificar que no sea BAD_CLUSTER
    // y que los bits 28-31 estén limpios (0)
    if (value == FAT32_BAD_CLUSTER || (value & 0xF0000000) != 0) {
      terminal_printf(
          &main_terminal,
          "FAT32: Invalid FAT[1] value 0x%08X, repairing to 0x0FFFFFFF\n",
          value);
      value = 0x0FFFFFFF; // Valor estándar para FAT[1]
      fs->has_errors = 1;
      if (fat32_set_fat_entry(fs, 1, value) != VFS_OK ||
          fat32_flush_fat_cache(fs) != VFS_OK) {
        terminal_printf(&main_terminal, "FAT32: Failed to repair FAT[1]\n");
        fs->has_errors = 1;
        return FAT32_BAD_CLUSTER;
      }
    }
  }
  // serial_printf(COM1_BASE, "FAT32: Get FAT entry for cluster %u: 0x%08X\n",
  // cluster, value);
  return value;
}

int fat32_set_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, fat_cache=%p, cluster=%u\n",
                    fs, fs ? fs->disk : NULL, fs ? fs->fat_cache : NULL,
                    cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->boot_sector.bytes_per_sector == 0) {
    terminal_printf(&main_terminal, "FAT32: Invalid bytes_per_sector=0\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (cluster < 1 || cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid cluster %u (total_clusters=%u)\n", cluster,
                    fs->total_clusters);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint32_t fat_offset = cluster * 4;
  uint32_t sector =
      fs->fat_start_sector + (fat_offset / fs->boot_sector.bytes_per_sector);
  uint32_t offset = fat_offset % fs->boot_sector.bytes_per_sector;
  if (sector >= fs->fat_start_sector + fs->boot_sector.sectors_per_fat_32) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid FAT sector %u for cluster %u\n", sector,
                    cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->fat_cache_sector != sector) {
    if (fs->fat_cache_dirty) {
      if (fat32_flush_fat_cache(fs) != VFS_OK) {
        terminal_printf(&main_terminal, "FAT32: Failed to flush FAT cache\n");
        fs->has_errors = 1;
        return VFS_ERR;
      }
    }
    disk_err_t err = disk_read_dispatch(fs->disk, sector, 1, fs->fat_cache);
    if (err != DISK_ERR_NONE) {
      terminal_printf(
          &main_terminal,
          "FAT32: Failed to read FAT sector %u for cluster %u (error %d)\n",
          sector, cluster, err);
      fs->has_errors = 1;
      return VFS_ERR;
    }
    fs->fat_cache_sector = sector;
  }
  ((uint32_t *)fs->fat_cache)[offset / 4] =
      (value & 0x0FFFFFFF) |
      (((uint32_t *)fs->fat_cache)[offset / 4] & 0xF0000000);
  fs->fat_cache_dirty = 1;
  serial_printf(COM1_BASE, "FAT32: Set FAT entry for cluster %u to 0x%08X\n",
                cluster, value);
  return VFS_OK;
}

uint32_t fat32_allocate_cluster(fat32_fs_t *fs) {
  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(
        &main_terminal,
        "fat32_allocate_cluster: Invalid fs=%p, disk=%p, fat_cache=%p\n", fs,
        fs ? fs->disk : NULL, fs ? fs->fat_cache : NULL);
    if (fs)
      fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }
  if (fs->boot_sector.sectors_per_fat_32 == 0 ||
      fs->boot_sector.bytes_per_sector == 0) {
    terminal_printf(&main_terminal,
                    "fat32_allocate_cluster: Invalid sectors_per_fat=%u, "
                    "bytes_per_sector=%u\n",
                    fs->boot_sector.sectors_per_fat_32,
                    fs->boot_sector.bytes_per_sector);
    fs->has_errors = 1;
    return FAT32_BAD_CLUSTER;
  }

  uint32_t cluster = fs->fsinfo.next_free_cluster;
  if (cluster < 2 || cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "fat32_allocate_cluster: Invalid next_free_cluster=%u, "
                    "resetting to 2\n",
                    cluster);
    cluster = 2;
  }

  uint32_t start_cluster = cluster;
  do {
    uint32_t value = fat32_get_fat_entry(fs, cluster);
    if (value == FAT32_FREE_CLUSTER) {
      if (fat32_set_fat_entry(fs, cluster, FAT32_EOC) != VFS_OK) {
        terminal_printf(
            &main_terminal,
            "fat32_allocate_cluster: Failed to mark cluster %u as EOC\n",
            cluster);
        fs->has_errors = 1;
        return FAT32_BAD_CLUSTER;
      }
      if (fat32_flush_fat_cache(fs) != VFS_OK) {
        terminal_printf(&main_terminal,
                        "fat32_allocate_cluster: Failed to flush FAT cache for "
                        "cluster %u\n",
                        cluster);
        fs->has_errors = 1;
        fat32_set_fat_entry(fs, cluster, FAT32_FREE_CLUSTER); // Rollback
        return FAT32_BAD_CLUSTER;
      }
      if (fs->fsinfo.free_clusters != 0xFFFFFFFF) {
        fs->fsinfo.free_clusters--;
      }
      fs->fsinfo.next_free_cluster =
          (cluster + 1 >= fs->total_clusters + 2) ? 2 : cluster + 1;
      if (fat32_update_fsinfo(fs) != VFS_OK) {
        terminal_printf(
            &main_terminal,
            "fat32_allocate_cluster: Failed to update FSInfo for cluster %u\n",
            cluster);
        fs->has_errors = 1;
        fat32_set_fat_entry(fs, cluster, FAT32_FREE_CLUSTER); // Rollback
        return FAT32_BAD_CLUSTER;
      }
      serial_printf(COM1_BASE, "fat32_allocate_cluster: Allocated cluster %u\n",
                    cluster);
      return cluster;
    }
    cluster = (cluster + 1 >= fs->total_clusters + 2) ? 2 : cluster + 1;
  } while (cluster != start_cluster);

  terminal_printf(&main_terminal,
                  "fat32_allocate_cluster: No free clusters available\n");
  fs->has_errors = 1;
  return FAT32_BAD_CLUSTER;
}

int fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t cluster) {
  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(
        &main_terminal,
        "fat32_free_cluster_chain: Invalid fs=%p, disk=%p, fat_cache=%p\n", fs,
        fs ? fs->disk : NULL, fs ? fs->fat_cache : NULL);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }

  if (cluster < 2 || cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "fat32_free_cluster_chain: Invalid starting cluster %u\n",
                    cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  uint32_t freed_count = 0;
  uint32_t first_freed = cluster;
  uint32_t current = cluster;

  while (current >= 2 && current < fs->total_clusters + 2) {
    uint32_t next = fat32_get_fat_entry(fs, current);

    if (next == FAT32_BAD_CLUSTER ||
        (next >= fs->total_clusters + 2 && next != FAT32_EOC)) {
      terminal_printf(
          &main_terminal,
          "fat32_free_cluster_chain: Invalid next cluster %u at current %u\n",
          next, current);
      fs->has_errors = 1;
      return VFS_ERR;
    }

    if (fat32_set_fat_entry(fs, current, FAT32_FREE_CLUSTER) != VFS_OK) {
      terminal_printf(
          &main_terminal,
          "fat32_free_cluster_chain: Failed to set cluster %u to free\n",
          current);
      fs->has_errors = 1;
      return VFS_ERR;
    }

    freed_count++;
    if (next == FAT32_EOC)
      break; // Terminate on end-of-chain
    current = next;
  }

  if (fat32_flush_fat_cache(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_free_cluster_chain: Failed to flush FAT cache after "
                    "freeing %u clusters\n",
                    freed_count);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  if (fs->fsinfo.free_clusters != 0xFFFFFFFF) {
    fs->fsinfo.free_clusters += freed_count;
  }

  if (first_freed < fs->fsinfo.next_free_cluster) {
    fs->fsinfo.next_free_cluster = first_freed;
  }

  if (fat32_update_fsinfo(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "fat32_free_cluster_chain: Failed to update FSInfo after "
                    "freeing %u clusters\n",
                    freed_count);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  serial_printf(COM1_BASE,
                "fat32_free_cluster_chain: Freed %u clusters starting at %u, "
                "new free_clusters=%u, next_free_cluster=%u\n",
                freed_count, first_freed, fs->fsinfo.free_clusters,
                fs->fsinfo.next_free_cluster);

  return VFS_OK;
}

int fat32_validate_dir_entry(fat32_dir_entry_t *entry, const char *context) {
  if (!entry)
    return 0;

  // Check for invalid characters in name
  for (int i = 0; i < 11; i++) {
    uint8_t c = entry->name[i];
    // Valid characters: A-Z, 0-9, space, and some special chars
    if (c != ' ' && c != 0xE5 && c != 0x00) {
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-' || c == '!' || c == '#' || c == '$' || c == '%' ||
            c == '&' || c == '\'' || c == '(' || c == ')' || c == '+' ||
            c == ',' || c == '.' || c == ';' || c == '=' || c == '@' ||
            c == '[' || c == ']' || c == '^' || c == '`' || c == '{' ||
            c == '}' || c == '~')) {
        serial_printf(COM1_BASE,
                      "FAT32 DEBUG: Invalid char 0x%02x at pos %d in %s\r\n", c,
                      i, context);
        return 0;
      }
    }
  }

  // Check attributes
  if (entry->attributes & 0x80) { // Reserved bit should not be set
    serial_printf(COM1_BASE, "FAT32 DEBUG: Invalid attributes 0x%02x in %s\r\n",
                  entry->attributes, context);
    return 0;
  }

  return 1;
}

// ========================================================================
// CLUSTER OPERATIONS
// ========================================================================

uint32_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster) {
  if (!fs || cluster < 2 || cluster >= fs->total_clusters + 2) {
    terminal_printf(
        &main_terminal,
        "fat32_cluster_to_sector: Invalid fs=%p or cluster=%u (max=%u)\n", fs,
        cluster, fs->total_clusters + 1);
    fs->has_errors = 1;
    return 0;
  }
  if (fs->boot_sector.sectors_per_cluster == 0) {
    terminal_printf(&main_terminal,
                    "fat32_cluster_to_sector: sectors_per_cluster=0\n");
    fs->has_errors = 1;
    return 0;
  }
  uint32_t sector = fs->data_start_sector +
                    ((cluster - 2) * fs->boot_sector.sectors_per_cluster);
  serial_printf(COM1_BASE, "fat32_cluster_to_sector: Cluster %u -> Sector %u\n",
                cluster, sector);
  return sector;
}

int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buffer) {
  if (!fs || !fs->disk || !buffer || cluster < 2 ||
      cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, buffer=%p, cluster=%u\n",
                    fs, fs ? fs->disk : NULL, buffer, cluster);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->boot_sector.bytes_per_sector == 0 ||
      fs->boot_sector.sectors_per_cluster == 0) {
    terminal_printf(
        &main_terminal,
        "FAT32: Invalid bytes_per_sector=%u or sectors_per_cluster=%u\n",
        fs->boot_sector.bytes_per_sector, fs->boot_sector.sectors_per_cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint32_t sector = fat32_cluster_to_sector(fs, cluster);
  if (sector == 0 || sector >= fs->boot_sector.total_sectors_32) {
    terminal_printf(&main_terminal, "FAT32: Invalid sector %u for cluster %u\n",
                    sector, cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  disk_err_t err = disk_read_dispatch(
      fs->disk, sector, fs->boot_sector.sectors_per_cluster, buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "FAT32: Failed to read cluster %u at sector %u (error %d)\n", cluster,
        sector, err);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  serial_printf(COM1_BASE, "FAT32: Read cluster %u from sector %u\n", cluster,
                sector);
  return VFS_OK;
}

int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buffer) {
  if (!fs || !fs->disk || !buffer || cluster < 2 ||
      cluster >= fs->total_clusters + 2) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, buffer=%p, cluster=%u\n",
                    fs, fs ? fs->disk : NULL, buffer, cluster);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->boot_sector.bytes_per_sector == 0 ||
      fs->boot_sector.sectors_per_cluster == 0) {
    terminal_printf(
        &main_terminal,
        "FAT32: Invalid bytes_per_sector=%u or sectors_per_cluster=%u\n",
        fs->boot_sector.bytes_per_sector, fs->boot_sector.sectors_per_cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint32_t sector = fat32_cluster_to_sector(fs, cluster);
  if (sector == 0 || sector >= fs->boot_sector.total_sectors_32) {
    terminal_printf(&main_terminal, "FAT32: Invalid sector %u for cluster %u\n",
                    sector, cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  disk_err_t err = disk_write_dispatch(
      fs->disk, sector, fs->boot_sector.sectors_per_cluster, buffer);

  if (err != DISK_ERR_NONE) {
    terminal_printf(
        &main_terminal,
        "FAT32: Failed to write cluster %u at sector %u (error %d)\n", sector,
        cluster, err);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  serial_printf(COM1_BASE, "FAT32: Successfully wrote cluster %u (sector %u)\n",
                cluster, sector);
  return VFS_OK;
}

// ========================================================================
// DIRECTORY ENTRY UPDATE
// ========================================================================

int fat32_update_dir_entry(fat32_fs_t *fs, fat32_node_t *node_data) {
  if (!fs || !node_data) {
    terminal_printf(&main_terminal,
                    "FAT32: update_dir_entry failed: invalid parameters\n");
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }

  if (node_data->parent_cluster == 0) {
    terminal_puts(&main_terminal, "FAT32: Skipping dir update for root\r\n");
    return VFS_OK; // Root doesn't need update
  }

  // Validar clúster inicial ANTES de usarlo
  if (node_data->first_cluster >= 2) {
    if (node_data->first_cluster >= fs->total_clusters + 2) {
      terminal_printf(&main_terminal,
                      "FAT32: Invalid first cluster %u (max=%u)\n",
                      node_data->first_cluster, fs->total_clusters + 1);
      fs->has_errors = 1;
      return VFS_ERR;
    }
    uint32_t fat_entry = fat32_get_fat_entry(fs, node_data->first_cluster);
    if (fat_entry == FAT32_FREE_CLUSTER) {
      terminal_printf(&main_terminal,
                      "FAT32: First cluster %u for file is free\n",
                      node_data->first_cluster);
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  uint32_t cluster = node_data->parent_cluster ? node_data->parent_cluster
                                               : fs->root_dir_cluster;
  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
  if (!cluster_buffer) {
    terminal_printf(
        &main_terminal,
        "FAT32: update_dir_entry failed: unable to allocate cluster buffer\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  // terminal_printf(&main_terminal, "FAT32: Updating dir entry for '");
  for (int i = 0; i < 11; i++) {
    if (node_data->short_name[i] >= 32 && node_data->short_name[i] <= 126) {
      terminal_putchar(&main_terminal, node_data->short_name[i]);
    } else {
      // serial_printf(COM1_BASE, "\\x%02X", node_data->short_name[i]);
    }
  }
  // serial_printf(COM1_BASE, "', cluster=%u, size=%u\r\n",
  // node_data->first_cluster, node_data->size);

  while (cluster >= 2 && cluster < FAT32_EOC) {
    if (fat32_read_cluster(fs, cluster, cluster_buffer) != VFS_OK) {
      terminal_printf(
          &main_terminal,
          "FAT32: update_dir_entry failed: unable to read cluster %u\n",
          cluster);
      kernel_free(cluster_buffer);
      fs->has_errors = 1;
      return VFS_ERR;
    }

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
    for (uint32_t i = 0; i < fs->cluster_size / FAT32_DIR_ENTRY_SIZE; i++) {
      if (entries[i].name[0] == 0x00) {
        kernel_free(cluster_buffer);
        terminal_printf(&main_terminal,
                        "FAT32: Dir entry not found for update\n");
        fs->has_errors = 1;
        return VFS_ERR;
      }
      if (entries[i].name[0] == 0xE5)
        continue;
      if ((entries[i].attributes & FAT32_ATTR_LONG_NAME) ==
          FAT32_ATTR_LONG_NAME)
        continue;

      if (memcmp(entries[i].name, node_data->short_name, 11) == 0) {
        // FIX: Usar conversión de endianness consistente
        entries[i].first_cluster_low =
            cpu_to_le16(node_data->first_cluster & 0xFFFF);
        entries[i].first_cluster_high =
            cpu_to_le16((node_data->first_cluster >> 16) & 0xFFFF);
        entries[i].file_size = cpu_to_le32(node_data->size);

        // Actualizar timestamps
        entries[i].write_date = cpu_to_le16(0x4B85); // 2025-09-03
        entries[i].write_time = cpu_to_le16(0x3C00); // 11:00 AM

        if (fat32_write_cluster(fs, cluster, cluster_buffer) != VFS_OK) {
          terminal_printf(
              &main_terminal,
              "FAT32: update_dir_entry failed: unable to write cluster %u\n",
              cluster);
          kernel_free(cluster_buffer);
          fs->has_errors = 1;
          return VFS_ERR;
        }

        kernel_free(cluster_buffer);
        serial_printf(COM1_BASE,
                      "FAT32: Dir entry updated successfully for file with "
                      "cluster %u, size %u\n",
                      node_data->first_cluster, node_data->size);
        return VFS_OK;
      }
    }
    cluster = fat32_get_fat_entry(fs, cluster);
  }

  kernel_free(cluster_buffer);
  terminal_printf(&main_terminal, "FAT32: Dir entry not found for update\n");
  fs->has_errors = 1;
  return VFS_ERR;
}

// ========================================================================
// VFS OPERATIONS
// ========================================================================

int fat32_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out) {
  if (!parent || !name || !out)
    return VFS_ERR;

  fat32_node_t *parent_data = (fat32_node_t *)parent->fs_private;
  if (!parent_data || !parent_data->is_directory)
    return VFS_ERR;

  fat32_fs_t *fs = (fat32_fs_t *)parent->sb->private;
  uint32_t cluster = parent_data->first_cluster;

  // Convertir name a uppercase para comparación
  char upper_name[VFS_NAME_MAX];
  strncpy(upper_name, name, VFS_NAME_MAX - 1);
  upper_name[VFS_NAME_MAX - 1] = '\0';
  strupper(upper_name);

  // Parsear a short name para búsqueda directa
  uint8_t fat_name[11];
  if (fat32_parse_short_name(upper_name, fat_name) != VFS_OK)
    return VFS_ERR;

  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
  if (!cluster_buffer)
    return VFS_ERR;

  while (cluster >= 2 && cluster < FAT32_EOC) {
    if (fat32_read_cluster(fs, cluster, cluster_buffer) != VFS_OK) {
      kernel_free(cluster_buffer);
      return VFS_ERR;
    }

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;

    for (uint32_t i = 0; i < fs->cluster_size / FAT32_DIR_ENTRY_SIZE; i++) {
      if (entries[i].name[0] == 0x00)
        break;
      if (entries[i].name[0] == 0xE5 ||
          entries[i].attributes == FAT32_ATTR_LONG_NAME)
        continue;

      if (memcmp(entries[i].name, fat_name, 11) == 0) {
        // Found
        vfs_node_t *node = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
        if (!node) {
          kernel_free(cluster_buffer);
          return VFS_ERR;
        }
        memset(node, 0, sizeof(vfs_node_t));

        if (fat32_format_short_name(entries[i].name, node->name) != VFS_OK) {
          kernel_free(node);
          kernel_free(cluster_buffer);
          return VFS_ERR;
        }
        node->type = (entries[i].attributes & FAT32_ATTR_DIRECTORY)
                         ? VFS_NODE_DIR
                         : VFS_NODE_FILE;
        node->ops = &fat32_vnode_ops;
        node->sb = parent->sb;
        node->refcount = 1;

        fat32_node_t *node_data =
            (fat32_node_t *)kernel_malloc(sizeof(fat32_node_t));
        if (!node_data) {
          kernel_free(node);
          kernel_free(cluster_buffer);
          return VFS_ERR;
        }
        memset(node_data, 0, sizeof(fat32_node_t));

        node_data->first_cluster =
            ((uint32_t)entries[i].first_cluster_high << 16) |
            entries[i].first_cluster_low;
        node_data->current_cluster = node_data->first_cluster;
        node_data->size = entries[i].file_size;
        node_data->attributes = entries[i].attributes;
        node_data->is_directory =
            (entries[i].attributes & FAT32_ATTR_DIRECTORY) ? 1 : 0;
        node_data->parent_cluster = parent_data->first_cluster;
        memcpy(node_data->short_name, entries[i].name, 11);

        node->fs_private = node_data;
        *out = node;

        kernel_free(cluster_buffer);
        return VFS_OK;
      }
    }

    cluster = fat32_get_fat_entry(fs, cluster);
  }

  kernel_free(cluster_buffer);
  return VFS_ERR;
}

int fat32_create(vfs_node_t *parent, const char *name, vfs_node_t **out) {
  if (!parent || !name || !out)
    return VFS_ERR;

  fat32_node_t *parent_data = (fat32_node_t *)parent->fs_private;
  if (!parent_data || !parent_data->is_directory)
    return VFS_ERR;

  fat32_fs_t *fs = (fat32_fs_t *)parent->sb->private;
  uint32_t dir_cluster = parent_data->first_cluster;

  // Check if exists
  vfs_node_t *existing = NULL;
  if (fat32_lookup(parent, name, &existing) == VFS_OK) {
    if (existing) {
      existing->refcount--;
      if (existing->refcount == 0 && existing->ops->release) {
        existing->ops->release(existing);
      }
    }
    return VFS_ERR; // Already exists
  }

  // For empty files, don't allocate cluster yet (FAT32 standard)
  uint32_t new_cluster = 0;

  // Create dir entry
  if (fat32_create_dir_entry(fs, dir_cluster, name, new_cluster, 0,
                             FAT32_ATTR_ARCHIVE) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to create dir entry for %s\r\n", name);
    return VFS_ERR;
  }

  serial_printf(COM1_BASE, "FAT32: Created dir entry for %s, cluster=0\r\n",
                name);

  // Create vnode
  vfs_node_t *node = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
  if (!node)
    return VFS_ERR;
  memset(node, 0, sizeof(vfs_node_t));

  strncpy(node->name, name, VFS_NAME_MAX - 1);
  node->name[VFS_NAME_MAX - 1] = '\0';
  node->type = VFS_NODE_FILE;
  node->ops = &fat32_vnode_ops;
  node->sb = parent->sb;
  node->refcount = 1;

  fat32_node_t *node_data = (fat32_node_t *)kernel_malloc(sizeof(fat32_node_t));
  if (!node_data) {
    kernel_free(node);
    return VFS_ERR;
  }
  memset(node_data, 0, sizeof(fat32_node_t));

  node_data->first_cluster = new_cluster;
  node_data->current_cluster = new_cluster;
  node_data->size = 0;
  node_data->attributes = FAT32_ATTR_ARCHIVE;
  node_data->is_directory = 0;
  node_data->parent_cluster = dir_cluster;

  // Parse short name correctly with uppercase
  char upper_name[VFS_NAME_MAX];
  strncpy(upper_name, name, VFS_NAME_MAX - 1);
  upper_name[VFS_NAME_MAX - 1] = '\0';
  strupper(upper_name); // Convert to uppercase
  fat32_parse_short_name(upper_name, node_data->short_name);

  node->fs_private = node_data;
  *out = node;

  serial_printf(COM1_BASE, "FAT32: Created vnode for %s successfully\r\n",
                name);
  return VFS_OK;
}

int fat32_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset) {
  if (!node || !buf)
    return VFS_ERR;

  fat32_node_t *node_data = (fat32_node_t *)node->fs_private;
  if (!node_data || node_data->is_directory)
    return VFS_ERR;

  fat32_fs_t *fs = (fat32_fs_t *)node->sb->private;
  if (offset >= node_data->size)
    return 0;

  uint32_t bytes_to_read =
      (size > node_data->size - offset) ? node_data->size - offset : size;
  uint32_t bytes_read = 0;

  uint32_t start_cluster = node_data->first_cluster;
  uint32_t cluster_offset = offset / fs->cluster_size;
  uint32_t intra_offset = offset % fs->cluster_size;

  // Skip to starting cluster
  uint32_t cluster = start_cluster;
  for (uint32_t i = 0; i < cluster_offset; i++) {
    cluster = fat32_get_fat_entry(fs, cluster);
    if (cluster >= FAT32_EOC)
      return bytes_read;
  }

  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
  if (!cluster_buffer)
    return VFS_ERR;

  while (bytes_to_read > 0 && cluster >= 2 && cluster < FAT32_EOC) {
    if (fat32_read_cluster(fs, cluster, cluster_buffer) != VFS_OK) {
      kernel_free(cluster_buffer);
      return VFS_ERR;
    }

    uint32_t bytes_in_cluster = fs->cluster_size - intra_offset;
    uint32_t bytes_to_copy =
        (bytes_to_read < bytes_in_cluster) ? bytes_to_read : bytes_in_cluster;

    memcpy(buf + bytes_read, cluster_buffer + intra_offset, bytes_to_copy);

    bytes_read += bytes_to_copy;
    bytes_to_read -= bytes_to_copy;
    intra_offset = 0;

    cluster = fat32_get_fat_entry(fs, cluster);
  }

  kernel_free(cluster_buffer);
  return bytes_read;
}

int fat32_write(vfs_node_t *node, const uint8_t *buf, uint32_t size,
                uint32_t offset) {
  if (!node || !buf || !node->sb || !node->fs_private) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: invalid parameters\n");
    return VFS_ERR;
  }

  fat32_node_t *node_data = (fat32_node_t *)node->fs_private;
  fat32_fs_t *fs = (fat32_fs_t *)node->sb->private;

  if (!fs || !fs->disk || !fs->fat_cache) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: invalid fs structure\n");
    return VFS_ERR;
  }

  if (node_data->is_directory) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: cannot write to directory\n");
    return VFS_ERR;
  }

  if (size == 0)
    return 0;

  // Limitar tamaño máximo para evitar problemas de memoria
  const uint32_t MAX_WRITE_SIZE = 64 * 1024; // 64KB máximo por operación
  if (size > MAX_WRITE_SIZE) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: size too large (%u bytes, max %u)\n",
                    size, MAX_WRITE_SIZE);
    return VFS_ERR;
  }

  // Validate cluster size
  if (fs->cluster_size == 0 || fs->cluster_size > 32768) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: invalid cluster size %u\n",
                    fs->cluster_size);
    return VFS_ERR;
  }

  uint32_t old_size = node_data->size;
  uint32_t new_size = (offset + size > old_size) ? offset + size : old_size;
  uint32_t bytes_written = 0;
  bool first_cluster_changed = false;

  char log_buf[256];
  snprintf(log_buf, sizeof(log_buf),
           "FAT32: Writing %u bytes at offset %u (current size: %u)\n", size,
           offset, old_size);
  serial_write_string(COM1_BASE, log_buf);

  // If file is empty, allocate first cluster
  if (node_data->first_cluster == 0) {
    uint32_t new_cluster = fat32_allocate_cluster(fs);
    if (new_cluster == FAT32_BAD_CLUSTER || new_cluster < 2 ||
        new_cluster >= fs->total_clusters + 2) {
      terminal_printf(
          &main_terminal,
          "FAT32: write failed: cannot allocate first cluster (got %u)\n",
          new_cluster);
      return VFS_ERR;
    }

    // Initialize the new cluster with zeros
    uint8_t *zero_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
    if (!zero_buffer) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: cannot allocate zero buffer\n");
      fat32_free_cluster_chain(fs, new_cluster);
      return VFS_ERR;
    }
    memset(zero_buffer, 0, fs->cluster_size);

    if (fat32_write_cluster(fs, new_cluster, zero_buffer) != VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: cannot initialize cluster %u\n",
                      new_cluster);
      kernel_free(zero_buffer);
      fat32_free_cluster_chain(fs, new_cluster);
      return VFS_ERR;
    }
    kernel_free(zero_buffer);

    node_data->first_cluster = new_cluster;
    node_data->current_cluster = new_cluster;
    first_cluster_changed = true;
    snprintf(log_buf, sizeof(log_buf),
             "FAT32: Allocated and initialized first cluster %u\n",
             new_cluster);
    serial_write_string(COM1_BASE, log_buf);
  }

  // Calculate clusters needed for the entire file
  uint32_t clusters_needed =
      (new_size + fs->cluster_size - 1) / fs->cluster_size;
  uint32_t current_clusters =
      fat32_count_clusters_in_chain(fs, node_data->first_cluster);

  snprintf(log_buf, sizeof(log_buf),
           "FAT32: Need %u clusters, currently have %u\n", clusters_needed,
           current_clusters);
  serial_write_string(COM1_BASE, log_buf);

  // Extend cluster chain if needed
  if (clusters_needed > current_clusters) {
    if (fat32_extend_cluster_chain(fs, node_data->first_cluster,
                                   clusters_needed - current_clusters) !=
        VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: cannot extend cluster chain\n");
      return bytes_written > 0 ? (int)bytes_written : VFS_ERR;
    }
    snprintf(log_buf, sizeof(log_buf),
             "FAT32: Extended cluster chain to %u clusters\n", clusters_needed);
    serial_write_string(COM1_BASE, log_buf);
  }

  // Navigate to starting cluster
  uint32_t cluster_offset = offset / fs->cluster_size;
  uint32_t intra_offset = offset % fs->cluster_size;
  uint32_t current_cluster = node_data->first_cluster;

  // Navigate to the correct starting cluster
  for (uint32_t i = 0; i < cluster_offset && current_cluster < FAT32_EOC; i++) {
    uint32_t next = fat32_get_fat_entry(fs, current_cluster);
    if (next < 2 || next >= FAT32_EOC) {
      terminal_printf(
          &main_terminal,
          "FAT32: write failed: broken cluster chain at cluster %u\n", i);
      return bytes_written > 0 ? (int)bytes_written : VFS_ERR;
    }
    current_cluster = next;
  }

  if (current_cluster >= FAT32_EOC) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: cluster chain too short\n");
    return bytes_written > 0 ? (int)bytes_written : VFS_ERR;
  }

  // Allocate cluster buffer once
  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
  if (!cluster_buffer) {
    terminal_printf(&main_terminal,
                    "FAT32: write failed: cannot allocate cluster buffer\n");
    return bytes_written > 0 ? (int)bytes_written : VFS_ERR;
  }

  // Write data cluster by cluster
  uint32_t remaining = size;
  while (remaining > 0 && current_cluster >= 2 && current_cluster < FAT32_EOC) {
    // Validate current cluster
    if (current_cluster >= fs->total_clusters + 2) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: invalid cluster %u\n",
                      current_cluster);
      break;
    }

    // Read existing cluster if we need partial write
    bool need_read = (intra_offset != 0) || (remaining < fs->cluster_size);
    if (need_read) {
      if (fat32_read_cluster(fs, current_cluster, cluster_buffer) != VFS_OK) {
        terminal_printf(&main_terminal,
                        "FAT32: write failed: cannot read cluster %u\n",
                        current_cluster);
        break;
      }
    } else {
      // Full cluster write, no need to read first
      memset(cluster_buffer, 0, fs->cluster_size);
    }

    // Calculate how much to write in this cluster
    uint32_t space_in_cluster = fs->cluster_size - intra_offset;
    uint32_t bytes_to_copy =
        (remaining < space_in_cluster) ? remaining : space_in_cluster;

    // Validate buffer access
    if (bytes_written + bytes_to_copy > size) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: buffer overflow protection\n");
      break;
    }

    // Copy data to cluster buffer with bounds checking
    if (intra_offset + bytes_to_copy > fs->cluster_size) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: cluster buffer overflow\n");
      break;
    }

    memcpy(cluster_buffer + intra_offset, buf + bytes_written, bytes_to_copy);

    // Write the cluster
    if (fat32_write_cluster(fs, current_cluster, cluster_buffer) != VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: write failed: cannot write cluster %u\n",
                      current_cluster);
      break;
    }

    bytes_written += bytes_to_copy;
    remaining -= bytes_to_copy;
    intra_offset = 0; // Only first cluster may have offset

    // Move to next cluster if we have more data
    if (remaining > 0) {
      uint32_t next_cluster = fat32_get_fat_entry(fs, current_cluster);
      if (next_cluster < 2 || next_cluster >= FAT32_EOC) {
        terminal_printf(
            &main_terminal,
            "FAT32: write failed: unexpected end of cluster chain\n");
        break;
      }
      current_cluster = next_cluster;
    }

    // Progress feedback for large writes
    if (bytes_written % 4096 == 0) {
      snprintf(log_buf, sizeof(log_buf),
               "FAT32: Progress: %u/%u bytes written\n", bytes_written, size);
      serial_write_string(COM1_BASE, log_buf);
    }
  }

  kernel_free(cluster_buffer);

  // Update file size if we extended it
  if (offset + bytes_written > node_data->size) {
    node_data->size = offset + bytes_written;
    snprintf(log_buf, sizeof(log_buf),
             "FAT32: Updated file size from %u to %u\n", old_size,
             node_data->size);
    serial_write_string(COM1_BASE, log_buf);
  }

  // Update directory entry if needed
  if (first_cluster_changed || node_data->size != old_size) {
    if (fat32_update_dir_entry(fs, node_data) != VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: write warning: cannot update dir entry\n");
    }
  }

  // Flush caches - CRITICO PARA PERSISTENCIA
  if (fat32_flush_fat_cache(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: write warning: failed to flush FAT cache\n");
  }

  // Flush directory cache always after write to ensure size/cluster updates are
  // saved
  if (fat32_flush_dir_cache(fs) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: write warning: failed to flush dir cache\n");
  }

  // Force disk sync
  disk_flush_dispatch(fs->disk);

  snprintf(log_buf, sizeof(log_buf),
           "FAT32: Write completed: %u bytes written\n", bytes_written);
  serial_write_string(COM1_BASE, log_buf);
  return bytes_written;
}

int fat32_readdir(vfs_node_t *node, vfs_dirent_t *buf, uint32_t *count,
                  uint32_t offset) {
  if (!node || !buf || !count || !node->sb || !node->fs_private) {
    terminal_printf(&main_terminal, "FAT32: readdir invalid params\n");
    return VFS_ERR;
  }

  fat32_fs_t *fs = (fat32_fs_t *)node->sb->private;
  fat32_node_t *node_data = (fat32_node_t *)node->fs_private;

  if (!node_data->is_directory) {
    terminal_printf(&main_terminal, "FAT32: readdir on non-directory\n");
    return VFS_ERR;
  }

  uint32_t max_count = *count;
  *count = 0;
  uint32_t entry_index = 0; // Current entry across all clusters
  uint32_t current_cluster = node_data->first_cluster;
  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);

  if (!cluster_buffer) {
    terminal_printf(&main_terminal, "FAT32: readdir malloc failed\n");
    return VFS_ERR;
  }

  // terminal_printf(&main_terminal,
  //     "FAT32: readdir start, first_cluster=%u, offset=%u, max_count=%u\n",
  //     current_cluster, offset, max_count);

  while (current_cluster >= 2 && current_cluster < FAT32_EOC &&
         *count < max_count) {
    // Read the current cluster
    int read_ret = fat32_read_cluster(fs, current_cluster, cluster_buffer);
    if (read_ret != VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: readdir failed to read cluster %u (error %d)\n",
                      current_cluster, read_ret);
      kernel_free(cluster_buffer);
      return VFS_ERR;
    }

    // Parse entries in the cluster (32-byte each)
    for (uint32_t pos = 0; pos < fs->cluster_size;
         pos += FAT32_DIR_ENTRY_SIZE) {
      uint8_t *entry = cluster_buffer + pos;

      // End of directory
      if (entry[0] == 0x00) {
        // terminal_printf(&main_terminal, "FAT32: readdir reached end of
        // directory\n");
        kernel_free(cluster_buffer);
        return VFS_OK;
      }

      // Deleted entry
      if (entry[0] == 0xE5) {
        entry_index++;
        continue;
      }

      // LFN entry (skip for now, short names only)
      if (entry[11] == 0x0F) {
        entry_index++;
        continue;
      }

      // Skip volume label
      if (entry[11] & FAT32_ATTR_VOLUME_ID) {
        entry_index++;
        continue;
      }

      // Valid entry: parse short name (8.3 with '.' if extension)
      if (entry_index >= offset) {
        char name[13]; // 8 + '.' + 3 + null
        memset(name, 0, sizeof(name));
        strncpy(name, (char *)entry, 8); // Base name
        int base_len = strnlen(name, 8);

        // Add '.' if extension present
        if (entry[8] != ' ') {
          name[base_len] = '.';
          strncpy(name + base_len + 1, (char *)(entry + 8), 3);
        }

        // Special handling for . and ..
        if (strncmp(name, "        ", 8) == 0 && entry[8] == ' ' &&
            entry[9] == ' ' && entry[10] == ' ') {
          continue; // Invalid
        }
        if (strncmp((char *)entry, ".       ", 8) == 0)
          strncpy(name, ".", 2);
        if (strncmp((char *)entry, "..      ", 8) == 0)
          strncpy(name, "..", 3);

        // Fill dirent
        strncpy(buf[*count].name, name, VFS_NAME_MAX);
        buf[*count].type =
            (entry[11] & FAT32_ATTR_DIRECTORY) ? VFS_NODE_DIR : VFS_NODE_FILE;
        (*count)++;

        // terminal_printf(&main_terminal, "FAT32: readdir found %s (%s)\n",
        //     buf[*count - 1].name,
        //     buf[*count - 1].type == VFS_NODE_DIR ? "dir" : "file");

        if (*count >= max_count) {
          kernel_free(cluster_buffer);
          return VFS_OK;
        }
      }

      entry_index++;
    }

    // Next cluster
    current_cluster = fat32_get_fat_entry(fs, current_cluster);
  }

  kernel_free(cluster_buffer);
  terminal_printf(&main_terminal, "FAT32: readdir completed, count=%u\n",
                  *count);
  return VFS_OK;
}

void fat32_flush_cache(fat32_fs_t *fs) {
  if (fs->fat_cache_dirty && fs->fat_cache_sector != 0xFFFFFFFF) {
    // Escribir en ambas FATs
    disk_write_dispatch(fs->disk, fs->fat_cache_sector, 1, fs->fat_cache);
    disk_write_dispatch(
        fs->disk, fs->fat_cache_sector + fs->boot_sector.sectors_per_fat_32, 1,
        fs->fat_cache);
    fs->fat_cache_dirty = 0;
  }

  if (fs->dir_cache_dirty && fs->dir_cache_sector != 0xFFFFFFFF) {
    disk_write_dispatch(fs->disk, fs->dir_cache_sector, 1, fs->dir_cache);
    fs->dir_cache_dirty = 0;
  }
}

void fat32_release(vfs_node_t *node) {
  if (!node) {
    terminal_puts(&main_terminal, "FAT32: release failed: invalid node\r\n");
    return;
  }

  // Verificar integridad del nodo ANTES de usar
  if (node->fs_private) {
    fat32_node_t *node_data = (fat32_node_t *)node->fs_private;

    // Verificar si el puntero parece válido antes de dereferenciarlo
    if ((uintptr_t)node_data < 0x100000 || (uintptr_t)node_data > 0xFFFFFFFF) {
      terminal_printf(
          &main_terminal,
          "FAT32: release warning: suspicious node_data pointer %p\r\n",
          node_data);
    } else {
      // Solo validar si parece un puntero válido
      memset(node_data, 0, sizeof(fat32_node_t)); // Clear before free
    }

    kernel_free(node->fs_private);
    node->fs_private = NULL;
  }

  // Verificar filesystem structure
  fat32_fs_t *fs = node->sb ? (fat32_fs_t *)node->sb->private : NULL;
  if (fs && fs->disk) {
    // Solo flush si el filesystem parece válido
    if ((uintptr_t)fs > 0x100000 && (uintptr_t)fs < 0xFFFFFFFF) {
      // Flush caches si están sucios
      if (fs->fat_cache && fs->fat_cache_dirty &&
          fs->fat_cache_sector != 0xFFFFFFFF) {
        disk_write_dispatch(fs->disk, fs->fat_cache_sector, 1, fs->fat_cache);
        fs->fat_cache_dirty = 0;
        terminal_puts(&main_terminal,
                      "FAT32: Flushed FAT cache in release\r\n");
      }

      if (fs->dir_cache && fs->dir_cache_dirty &&
          fs->dir_cache_sector != 0xFFFFFFFF) {
        disk_write_dispatch(fs->disk, fs->dir_cache_sector, 1, fs->dir_cache);
        fs->dir_cache_dirty = 0;
        terminal_puts(&main_terminal,
                      "FAT32: Flushed dir cache in release\r\n");
      }
    }
  }

  // Clear node structure before freeing
  memset(node, 0, sizeof(vfs_node_t));
  kernel_free(node);
}

// ========================================================================
// UTILITY FUNCTIONS
// ========================================================================

static int fat32_write_zero_sector(fat32_fs_t *fs, uint64_t sector,
                                   uint32_t count) {
  uint8_t *zero_buf = (uint8_t *)kernel_malloc(FAT32_SECTOR_SIZE * count);
  if (!zero_buf)
    return VFS_ERR;
  memset(zero_buf, 0, FAT32_SECTOR_SIZE * count);
  disk_err_t err = disk_write_dispatch(fs->disk, sector, count, zero_buf);
  kernel_free(zero_buf);
  return (err == DISK_ERR_NONE) ? VFS_OK : VFS_ERR;
}

bool check_fat32_signature(uint8_t *boot_sector) {
  if (!boot_sector) {
    terminal_printf(&main_terminal, "FAT32: No boot sector\n");
    return false;
  }

  // **PRIMERO: Verificar firma de boot sector**
  terminal_printf(&main_terminal,
                  "FAT32: Checking boot signature: 0x%02X 0x%02X\n",
                  boot_sector[510], boot_sector[511]);

  if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA) {
    terminal_printf(&main_terminal, "FAT32: Invalid boot signature\n");
    return false;
  }

  terminal_puts(&main_terminal, "FAT32: ✓ Boot signature OK\n");

  // **SEGUNDO: Mostrar información del sector para diagnóstico**
  uint16_t bytes_per_sector = *(uint16_t *)&boot_sector[11];
  uint8_t sectors_per_cluster = boot_sector[13];
  uint16_t reserved_sectors = *(uint16_t *)&boot_sector[14];
  uint8_t num_fats = boot_sector[16];
  uint16_t root_entries = *(uint16_t *)&boot_sector[17];
  uint16_t total_sectors_16 = *(uint16_t *)&boot_sector[19];
  uint16_t sectors_per_fat_16 = *(uint16_t *)&boot_sector[22];
  uint32_t total_sectors_32 = *(uint32_t *)&boot_sector[32];
  uint32_t sectors_per_fat_32 = *(uint32_t *)&boot_sector[36];
  uint32_t root_cluster = *(uint32_t *)&boot_sector[44];

  terminal_printf(&main_terminal, "FAT32: BPB Information:\n");
  terminal_printf(&main_terminal, "  Bytes per sector: %u\n", bytes_per_sector);
  terminal_printf(&main_terminal, "  Sectors per cluster: %u\n",
                  sectors_per_cluster);
  terminal_printf(&main_terminal, "  Reserved sectors: %u\n", reserved_sectors);
  terminal_printf(&main_terminal, "  Number of FATs: %u\n", num_fats);
  terminal_printf(&main_terminal, "  Root entries: %u (0 for FAT32)\n",
                  root_entries);
  terminal_printf(&main_terminal, "  Total sectors (16): %u (0 for FAT32)\n",
                  total_sectors_16);
  terminal_printf(&main_terminal, "  Sectors per FAT (16): %u (0 for FAT32)\n",
                  sectors_per_fat_16);
  terminal_printf(&main_terminal, "  Total sectors (32): %u\n",
                  total_sectors_32);
  terminal_printf(&main_terminal, "  Sectors per FAT (32): %u\n",
                  sectors_per_fat_32);
  terminal_printf(&main_terminal, "  Root cluster: %u\n", root_cluster);

  // **TERCERO: Verificar campos de sistema de archivos**
  terminal_printf(&main_terminal, "FAT32: Filesystem type at offset 54: ");
  for (int i = 54; i < 62; i++) {
    terminal_printf(&main_terminal, "%c",
                    boot_sector[i] >= 32 ? boot_sector[i] : '.');
  }
  terminal_puts(&main_terminal, "");

  terminal_printf(&main_terminal, "FAT32: Filesystem type at offset 82: ");
  for (int i = 82; i < 90; i++) {
    terminal_printf(&main_terminal, "%c",
                    boot_sector[i] >= 32 ? boot_sector[i] : '.');
  }
  terminal_puts(&main_terminal, "");

  // **CUARTO: Verificaciones específicas de FAT32**
  bool is_fat32 = false;

  // Método 1: Por campos BPB
  if (bytes_per_sector == 512 && sectors_per_cluster > 0 &&
      reserved_sectors > 0 && num_fats > 0 && root_entries == 0 &&
      total_sectors_16 == 0 && sectors_per_fat_16 == 0 &&
      sectors_per_fat_32 > 0) {
    terminal_puts(&main_terminal, "FAT32: ✓ Detected by BPB fields\n");
    is_fat32 = true;
  }

  // Método 2: Por cadena "FAT32"
  if (memcmp(&boot_sector[54], "FAT32   ", 8) == 0 ||
      memcmp(&boot_sector[82], "FAT32   ", 8) == 0) {
    terminal_puts(&main_terminal, "FAT32: ✓ Detected by filesystem string\n");
    is_fat32 = true;
  }

  // Método 3: Por OEM name (algunas implementaciones)
  terminal_printf(&main_terminal, "FAT32: OEM name: ");
  for (int i = 3; i < 11; i++) {
    terminal_printf(&main_terminal, "%c",
                    boot_sector[i] >= 32 ? boot_sector[i] : '.');
  }
  terminal_puts(&main_terminal, "");

  if (is_fat32) {
    terminal_puts(&main_terminal, "FAT32: ✓ Confirmed as FAT32 filesystem\n");
    return true;
  }

  // **QUINTO: Posibles problemas**
  if (bytes_per_sector != 512) {
    terminal_printf(&main_terminal,
                    "FAT32: Warning: Unusual bytes per sector: %u\n",
                    bytes_per_sector);
  }

  if (root_entries != 0) {
    terminal_printf(&main_terminal,
                    "FAT32: Warning: root_entries=%u (expected 0 for FAT32)\n",
                    root_entries);
    // Esto podría ser FAT16
  }

  if (sectors_per_fat_16 != 0) {
    terminal_printf(
        &main_terminal,
        "FAT32: Warning: sectors_per_fat_16=%u (expected 0 for FAT32)\n",
        sectors_per_fat_16);
  }

  terminal_puts(&main_terminal, "FAT32: ✗ Not identified as FAT32\n");
  return false;
}

int fat32_parse_short_name(const char *name, uint8_t *fat_name) {
  if (!name || !fat_name) {
    return VFS_ERR;
  }

  size_t name_len = strnlen(name, VFS_NAME_MAX);
  if (name_len == 0 || name_len > VFS_NAME_MAX) {
    return VFS_ERR;
  }

  // Inicializar con espacios de manera segura
  memset(fat_name, 0x20, 11); // 0x20 = espacio ASCII

  const char *dot = strrchr(name, '.');
  size_t base_len = dot ? (size_t)(dot - name) : name_len;
  size_t ext_len = dot ? strnlen(dot + 1, 3) : 0;

  if (base_len == 0 || base_len > 8 || ext_len > 3) {
    return VFS_ERR;
  }

  // Copiar nombre base
  for (size_t i = 0; i < base_len && i < 8; i++) {
    char c = name[i];
    if (c >= 'a' && c <= 'z')
      c = c - 'a' + 'A'; // A mayúscula
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '-' || c == '~') {
      fat_name[i] = (uint8_t)c;
    } else {
      return VFS_ERR; // Carácter inválido
    }
  }

  // Copiar extensión si existe
  if (dot && ext_len > 0) {
    const char *ext = dot + 1;
    for (size_t i = 0; i < ext_len && i < 3; i++) {
      char c = ext[i];
      if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A'; // A mayúscula
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == '~') {
        fat_name[8 + i] = (uint8_t)c;
      } else {
        return VFS_ERR; // Carácter inválido
      }
    }
  }

  return VFS_OK;
}

int fat32_format_short_name(const uint8_t *fat_name, char *name) {
  if (!fat_name || !name) {
    return VFS_ERR;
  }

  // Verificar que el fat_name no esté corrompido
  for (int i = 0; i < 11; i++) {
    if (fat_name[i] != 0x20 && (fat_name[i] < 0x21 || fat_name[i] > 0x7E)) {
      return VFS_ERR; // Carácter inválido detectado
    }
  }

  int pos = 0;

  // Copiar nombre base (hasta 8 caracteres)
  for (int i = 0; i < 8; i++) {
    if (fat_name[i] == 0x20)
      break; // Parar en primer espacio
    name[pos++] = (char)fat_name[i];
    if (pos >= VFS_NAME_MAX - 1)
      return VFS_ERR; // Protección overflow
  }

  // Verificar si hay extensión
  bool has_ext = false;
  for (int i = 8; i < 11; i++) {
    if (fat_name[i] != 0x20) {
      has_ext = true;
      break;
    }
  }

  if (has_ext) {
    if (pos >= VFS_NAME_MAX - 1)
      return VFS_ERR;
    name[pos++] = '.';

    for (int i = 8; i < 11; i++) {
      if (fat_name[i] == 0x20)
        break;
      if (pos >= VFS_NAME_MAX - 1)
        return VFS_ERR;
      name[pos++] = (char)fat_name[i];
    }
  }

  name[pos] = '\0';
  return pos > 0 ? VFS_OK : VFS_ERR;
}

uint8_t fat32_calculate_checksum(const uint8_t *short_name) {
  uint8_t checksum = 0;
  for (int i = 0; i < 11; i++) {
    checksum = ((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + short_name[i];
  }
  return checksum;
}

int fat32_find_free_dir_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                              uint32_t *sector, uint32_t *offset) {
  if (!fs || !fs->disk || !sector || !offset || dir_cluster < 2 ||
      dir_cluster >= fs->total_clusters + 2) {
    terminal_printf(
        &main_terminal,
        "FAT32: Invalid fs=%p, disk=%p, sector=%p, offset=%p, dir_cluster=%u\n",
        fs, fs ? fs->disk : NULL, sector, offset, dir_cluster);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fs->boot_sector.bytes_per_sector == 0 ||
      fs->boot_sector.sectors_per_cluster == 0) {
    terminal_printf(
        &main_terminal,
        "FAT32: Invalid bytes_per_sector=%u or sectors_per_cluster=%u\n",
        fs->boot_sector.bytes_per_sector, fs->boot_sector.sectors_per_cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint8_t *cluster_buffer = (uint8_t *)kernel_malloc(fs->cluster_size);
  if (!cluster_buffer) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to allocate cluster buffer\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }
  uint32_t cluster = dir_cluster;
  uint32_t last_valid_cluster = dir_cluster;
  while (cluster >= 2 && cluster < FAT32_EOC) {
    uint32_t first_sector = fat32_cluster_to_sector(fs, cluster);
    if (first_sector == 0 || first_sector >= fs->boot_sector.total_sectors_32) {
      terminal_printf(&main_terminal,
                      "FAT32: Invalid sector %u for cluster %u\n", first_sector,
                      cluster);
      kernel_free(cluster_buffer);
      fs->has_errors = 1;
      return VFS_ERR;
    }
    for (uint32_t i = 0; i < fs->boot_sector.sectors_per_cluster; i++) {
      uint32_t current_sector = first_sector + i;
      if (disk_read_dispatch(fs->disk, current_sector, 1, cluster_buffer) !=
          DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "FAT32: Failed to read sector %u\n",
                        current_sector);
        kernel_free(cluster_buffer);
        fs->has_errors = 1;
        return VFS_ERR;
      }
      fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
      uint32_t entries_per_sector =
          fs->boot_sector.bytes_per_sector / FAT32_DIR_ENTRY_SIZE;
      for (uint32_t j = 0; j < entries_per_sector; j++) {
        if (entries[j].name[0] == 0x00 || entries[j].name[0] == 0xE5) {
          *sector = current_sector;
          *offset = j * FAT32_DIR_ENTRY_SIZE;
          serial_printf(COM1_BASE,
                        "FAT32: Found free entry at sector %u, offset %u\n",
                        *sector, *offset);
          kernel_free(cluster_buffer);
          return VFS_OK;
        }
      }
    }
    last_valid_cluster = cluster;
    cluster = fat32_get_fat_entry(fs, cluster);
    if (cluster == FAT32_BAD_CLUSTER) {
      terminal_printf(&main_terminal,
                      "FAT32: Invalid FAT entry for cluster %u\n", cluster);
      kernel_free(cluster_buffer);
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }
  uint32_t new_cluster = fat32_allocate_cluster(fs);
  if (new_cluster == FAT32_BAD_CLUSTER) {
    terminal_printf(&main_terminal, "FAT32: Failed to allocate new cluster\n");
    kernel_free(cluster_buffer);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (last_valid_cluster != new_cluster && last_valid_cluster >= 2) {
    if (fat32_set_fat_entry(fs, last_valid_cluster, new_cluster) != VFS_OK) {
      terminal_printf(&main_terminal,
                      "FAT32: Failed to link cluster %u to %u\n",
                      last_valid_cluster, new_cluster);
      kernel_free(cluster_buffer);
      fat32_set_fat_entry(fs, new_cluster, FAT32_FREE_CLUSTER);
      fat32_flush_fat_cache(fs);
      fs->has_errors = 1;
      return VFS_ERR;
    }
  }
  if (fat32_set_fat_entry(fs, new_cluster, FAT32_EOC) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to mark new cluster %u as EOC\n",
                    new_cluster);
    kernel_free(cluster_buffer);
    fat32_set_fat_entry(fs, new_cluster, FAT32_FREE_CLUSTER);
    fat32_flush_fat_cache(fs);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  if (fat32_flush_fat_cache(fs) != VFS_OK) {
    terminal_printf(
        &main_terminal,
        "FAT32: Failed to flush FAT cache after linking new cluster\n");
    kernel_free(cluster_buffer);
    fat32_set_fat_entry(fs, new_cluster, FAT32_FREE_CLUSTER);
    fat32_flush_fat_cache(fs);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  memset(cluster_buffer, 0, fs->cluster_size);
  cluster_buffer[0] = 0x00;
  if (fat32_write_cluster(fs, new_cluster, cluster_buffer) != VFS_OK) {
    terminal_printf(&main_terminal, "FAT32: Failed to write new cluster %u\n",
                    new_cluster);
    kernel_free(cluster_buffer);
    fat32_set_fat_entry(fs, new_cluster, FAT32_FREE_CLUSTER);
    fat32_flush_fat_cache(fs);
    fs->has_errors = 1;
    return VFS_ERR;
  }
  *sector = fat32_cluster_to_sector(fs, new_cluster);
  *offset = 0;
  serial_printf(COM1_BASE,
                "FAT32: Allocated new cluster %u for directory entry, sector "
                "%u, offset %u\n",
                new_cluster, *sector, *offset);
  kernel_free(cluster_buffer);
  return VFS_OK;
}

// 1. Fix fat32_set_current_time function - it's setting invalid timestamps
void fat32_set_current_time(fat32_dir_entry_t *entry) {
  // Use valid DOS timestamps instead of zeros
  // DOS date format: bits 15-9=year-1980, bits 8-5=month, bits 4-0=day
  // DOS time format: bits 15-11=hour, bits 10-5=minute/2, bits 4-0=second/2

  // Set to Jan 1, 2000, 12:00:00 as a reasonable default
  entry->creation_date = 0x2821; // Jan 1, 2000 (20 << 9 | 1 << 5 | 1)
  entry->creation_time = 0x6000; // 12:00:00 (12 << 11)
  entry->creation_time_tenth = 0;
  entry->last_access_date = 0x2821;
  entry->write_date = 0x2821;
  entry->write_time = 0x6000;
}

int fat32_create_dir_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                           const char *name, uint32_t first_cluster,
                           uint32_t size, uint8_t attributes) {
  if (!fs || !fs->disk || !name || strnlen(name, VFS_NAME_MAX + 1) == 0 ||
      strnlen(name, VFS_NAME_MAX + 1) > VFS_NAME_MAX) {
    terminal_printf(&main_terminal,
                    "FAT32: Invalid fs=%p, disk=%p, name=%p, name_len=%u\n", fs,
                    fs ? fs->disk : NULL, name,
                    name ? strnlen(name, VFS_NAME_MAX + 1) : 0);
    if (fs)
      fs->has_errors = 1;
    return VFS_ERR;
  }

  // Validar caracteres del nombre
  for (size_t i = 0; i < strnlen(name, VFS_NAME_MAX + 1); i++) {
    if (name[i] < 0x20 || name[i] > 0x7E) {
      terminal_printf(
          &main_terminal,
          "FAT32: Invalid character 0x%02X in name at position %u\n",
          (unsigned char)name[i], i);
      if (fs)
        fs->has_errors = 1;
      return VFS_ERR;
    }
  }

  serial_printf(COM1_BASE, "FAT32: Creating %s in cluster %u\n", name,
                dir_cluster);

  uint32_t sector, offset;
  if (fat32_find_free_dir_entry(fs, dir_cluster, &sector, &offset) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: No free directory entry in cluster %u\n",
                    dir_cluster);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  uint8_t *sector_buffer =
      (uint8_t *)kernel_malloc(fs->boot_sector.bytes_per_sector);
  if (!sector_buffer) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to allocate sector buffer\n");
    fs->has_errors = 1;
    return VFS_ERR;
  }

  disk_err_t err = disk_read_dispatch(fs->disk, sector, 1, sector_buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to read sector %u (error %d)\n", sector,
                    err);
    kernel_free(sector_buffer);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(sector_buffer + offset);
  memset(entry, 0, sizeof(fat32_dir_entry_t));

  uint8_t fat_name[11];
  if (fat32_parse_short_name(name, fat_name) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to parse short name for %s\n", name);
    kernel_free(sector_buffer);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  memcpy(entry->name, fat_name, 11);
  entry->attributes = attributes;

  // FIX: Usar conversión de endianness consistente
  entry->first_cluster_low = cpu_to_le16(first_cluster & 0xFFFF);
  entry->first_cluster_high = cpu_to_le16((first_cluster >> 16) & 0xFFFF);
  entry->file_size = cpu_to_le32(size);

  // Establecer timestamps válidos
  entry->creation_date = cpu_to_le16(0x4B85); // 2025-09-03
  entry->creation_time = cpu_to_le16(0x3C00); // 11:00 AM
  entry->creation_time_tenth = 0;
  entry->last_access_date = cpu_to_le16(0x4B85);
  entry->write_date = cpu_to_le16(0x4B85);
  entry->write_time = cpu_to_le16(0x3C00);

  err = disk_write_dispatch(fs->disk, sector, 1, sector_buffer);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal,
                    "FAT32: Failed to write sector %u (error %d)\n", sector,
                    err);
    kernel_free(sector_buffer);
    fs->has_errors = 1;
    return VFS_ERR;
  }

  kernel_free(sector_buffer);
  serial_printf(COM1_BASE,
                "FAT32: Successfully created %s with cluster %u, size %u\n",
                name, first_cluster, size);
  return VFS_OK;
}