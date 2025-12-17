#include "mbr.h"
#include "kernel.h"
#include "string.h"
#include "terminal.h"

extern Terminal main_terminal;

// Read MBR from disk
mbr_err_t mbr_read(disk_t *disk, mbr_t *mbr) {
  if (!disk || !mbr) {
    return MBR_ERR_INVALID_DISK;
  }

  disk_err_t err = disk_read_dispatch(disk, 0, 1, mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "MBR: Failed to read MBR (error %d)\n",
                    err);
    return MBR_ERR_READ_FAILED;
  }

  if (!mbr_verify_signature(mbr)) {
    terminal_printf(&main_terminal, "MBR: Invalid signature 0x%04X\n",
                    mbr->signature);
    return MBR_ERR_INVALID_SIGNATURE;
  }

  return MBR_OK;
}

// Write MBR to disk
mbr_err_t mbr_write(disk_t *disk, const mbr_t *mbr) {
  if (!disk || !mbr) {
    return MBR_ERR_INVALID_DISK;
  }

  if (!mbr_verify_signature(mbr)) {
    terminal_printf(&main_terminal, "MBR: Refusing to write invalid MBR\n");
    return MBR_ERR_INVALID_SIGNATURE;
  }

  terminal_printf(&main_terminal, "MBR: Writing MBR to disk...\n");

  disk_err_t err = disk_write_dispatch(disk, 0, 1, mbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "MBR: Failed to write MBR (error %d)\n",
                    err);
    return MBR_ERR_WRITE_FAILED;
  }

  // Verify write
  mbr_t verify_mbr;
  err = disk_read_dispatch(disk, 0, 1, &verify_mbr);
  if (err != DISK_ERR_NONE || memcmp(mbr, &verify_mbr, sizeof(mbr_t)) != 0) {
    terminal_printf(&main_terminal, "MBR: Verification failed!\n");
    return MBR_ERR_VERIFY_FAILED;
  }

  terminal_printf(&main_terminal,
                  "MBR: MBR written and verified successfully\n");
  return MBR_OK;
}

// Install boot code to MBR
mbr_err_t mbr_install_bootcode(disk_t *disk, const uint8_t *boot_code,
                               uint32_t size) {
  if (!disk || !boot_code || size == 0 || size > MBR_BOOT_CODE_SIZE) {
    terminal_printf(&main_terminal,
                    "MBR: Invalid boot code (size=%u, max=%u)\n", size,
                    MBR_BOOT_CODE_SIZE);
    return MBR_ERR_BUFFER_TOO_SMALL;
  }

  terminal_printf(&main_terminal, "MBR: Installing boot code (%u bytes)...\n",
                  size);

  // Read existing MBR to preserve partition table
  mbr_t mbr;
  mbr_err_t err = mbr_read(disk, &mbr);
  if (err != MBR_OK) {
    return err;
  }

  // Clear boot code area and copy new boot code
  memset(mbr.boot_code, 0, MBR_BOOT_CODE_SIZE);
  memcpy(mbr.boot_code, boot_code, size);

  // Ensure signature is set to 0xAA55
  mbr.signature = 0xAA55;

  // Write back the full MBR
  err = mbr_write(disk, &mbr);
  if (err != MBR_OK) {
    return err;
  }

  terminal_printf(&main_terminal, "MBR: Boot code installed successfully\n");
  return MBR_OK;
}

// Backup MBR
mbr_err_t mbr_backup(disk_t *disk, uint8_t *backup_buffer,
                     uint32_t buffer_size) {
  if (!disk || !backup_buffer || buffer_size < sizeof(mbr_t)) {
    return MBR_ERR_BUFFER_TOO_SMALL;
  }

  mbr_t mbr;
  mbr_err_t err = mbr_read(disk, &mbr);
  if (err != MBR_OK) {
    return err;
  }

  memcpy(backup_buffer, &mbr, sizeof(mbr_t));
  terminal_printf(&main_terminal, "MBR: Backup created\n");
  return MBR_OK;
}

// Restore MBR from backup
mbr_err_t mbr_restore(disk_t *disk, const uint8_t *backup_buffer,
                      uint32_t buffer_size) {
  if (!disk || !backup_buffer || buffer_size < sizeof(mbr_t)) {
    return MBR_ERR_BUFFER_TOO_SMALL;
  }

  const mbr_t *mbr = (const mbr_t *)backup_buffer;

  if (!mbr_verify_signature(mbr)) {
    terminal_printf(&main_terminal, "MBR: Invalid backup signature\n");
    return MBR_ERR_INVALID_SIGNATURE;
  }

  terminal_printf(&main_terminal, "MBR: Restoring from backup...\n");
  return mbr_write(disk, mbr);
}

// Verify MBR signature
bool mbr_verify_signature(const mbr_t *mbr) {
  if (!mbr)
    return false;

  // Verificar bytes individuales (little-endian)
  const uint8_t *bytes = (const uint8_t *)mbr;
  return (bytes[510] == 0x55 && bytes[511] == 0xAA);
}

// Read VBR (Volume Boot Record)
mbr_err_t vbr_read(disk_t *disk, uint64_t partition_lba, vbr_fat32_t *vbr) {
  if (!disk || !vbr) {
    return MBR_ERR_INVALID_DISK;
  }

  disk_err_t err = disk_read_dispatch(disk, partition_lba, 1, vbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "VBR: Failed to read VBR (error %d)\n",
                    err);
    return MBR_ERR_READ_FAILED;
  }

  if (vbr->signature != BOOT_SIGNATURE) {
    terminal_printf(&main_terminal, "VBR: Invalid signature 0x%04X\n",
                    vbr->signature);
    return MBR_ERR_INVALID_SIGNATURE;
  }

  return MBR_OK;
}

// Write VBR
mbr_err_t vbr_write(disk_t *disk, uint64_t partition_lba,
                    const vbr_fat32_t *vbr) {
  if (!disk || !vbr) {
    return MBR_ERR_INVALID_DISK;
  }

  if (vbr->signature != BOOT_SIGNATURE) {
    terminal_printf(&main_terminal, "VBR: Invalid signature\n");
    return MBR_ERR_INVALID_SIGNATURE;
  }

  terminal_printf(&main_terminal, "VBR: Writing VBR to LBA %llu...\n",
                  partition_lba);

  disk_err_t err = disk_write_dispatch(disk, partition_lba, 1, vbr);
  if (err != DISK_ERR_NONE) {
    terminal_printf(&main_terminal, "VBR: Failed to write VBR (error %d)\n",
                    err);
    return MBR_ERR_WRITE_FAILED;
  }

  // Verify write
  vbr_fat32_t verify_vbr;
  err = disk_read_dispatch(disk, partition_lba, 1, &verify_vbr);
  if (err != DISK_ERR_NONE ||
      memcmp(vbr, &verify_vbr, sizeof(vbr_fat32_t)) != 0) {
    terminal_printf(&main_terminal, "VBR: Verification failed!\n");
    return MBR_ERR_VERIFY_FAILED;
  }

  terminal_printf(&main_terminal, "VBR: VBR written and verified\n");
  return MBR_OK;
}

// Install boot code to VBR
mbr_err_t vbr_install_bootcode(disk_t *disk, uint64_t partition_lba,
                               const uint8_t *boot_code, uint32_t size) {
  if (!disk || !boot_code || size == 0 || size > 420) {
    terminal_printf(&main_terminal, "VBR: Invalid boot code size %u\n", size);
    return MBR_ERR_BUFFER_TOO_SMALL;
  }

  terminal_printf(&main_terminal, "VBR: Installing boot code (%u bytes)...\n",
                  size);

  // Read existing VBR to preserve filesystem information
  vbr_fat32_t vbr;
  mbr_err_t err = vbr_read(disk, partition_lba, &vbr);
  if (err != MBR_OK) {
    return err;
  }

  // Verify it's FAT32
  if (!vbr_verify_fat32(&vbr)) {
    terminal_printf(&main_terminal, "VBR: Not a valid FAT32 VBR\n");
    return MBR_ERR_INVALID_SIGNATURE;
  }

  // Clear boot code area
  memset(vbr.boot_code, 0, sizeof(vbr.boot_code));

  // Copy new boot code
  memcpy(vbr.boot_code, boot_code, size);

  // Write back
  err = vbr_write(disk, partition_lba, &vbr);
  if (err != MBR_OK) {
    return err;
  }

  terminal_printf(&main_terminal, "VBR: Boot code installed successfully\n");
  return MBR_OK;
}

// Verify FAT32 VBR
bool vbr_verify_fat32(const vbr_fat32_t *vbr) {
  if (!vbr)
    return false;

  // Check signature
  if (vbr->signature != BOOT_SIGNATURE)
    return false;

  // Check filesystem type
  if (memcmp(vbr->fs_type, "FAT32   ", 8) != 0) {
    // Some implementations don't set this, so also check other indicators
    if (vbr->sectors_per_fat_16 != 0 || vbr->sectors_per_fat_32 == 0) {
      return false;
    }
  }

  // Sanity checks
  if (vbr->bytes_per_sector != 512)
    return false;
  if (vbr->sectors_per_cluster == 0)
    return false;
  if (vbr->num_fats == 0)
    return false;
  if (vbr->root_entries != 0)
    return false; // FAT32 specific
  if (vbr->total_sectors_16 != 0)
    return false; // FAT32 specific

  return true;
}

// Print hex dump
void mbr_print_hex(const uint8_t *data, uint32_t size) {
  if (!data)
    return;

  for (uint32_t i = 0; i < size; i += 16) {
    terminal_printf(&main_terminal, "%04X: ", i);

    // Hex values
    for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
      terminal_printf(&main_terminal, "%02X ", data[i + j]);
    }

    // Padding if less than 16 bytes
    for (uint32_t j = (size - i); j < 16; j++) {
      terminal_printf(&main_terminal, "   ");
    }

    terminal_printf(&main_terminal, " | ");

    // ASCII representation
    for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
      char c = data[i + j];
      terminal_putchar(&main_terminal, (c >= 32 && c <= 126) ? c : '.');
    }

    terminal_printf(&main_terminal, "\n");
  }
}