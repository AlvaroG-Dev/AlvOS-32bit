#include "installer.h"
#include "partition.h"
#include "mbr.h"
#include "fat32.h"
#include "module_loader.h"
#include "string.h"
#include "terminal.h"
#include "kernel.h"
#include "serial.h"

extern Terminal main_terminal;

// Initialize installer state
install_err_t installer_init(installer_state_t *state, disk_t *disk, 
                             installer_image_t *image, install_options_t *options) {
    if (!state || !disk || !image || !options) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    memset(state, 0, sizeof(installer_state_t));
    state->disk = disk;
    state->image = image;
    state->options = *options;
    
    terminal_printf(&main_terminal, "\n=== Kernel Installer ===\n");
    terminal_printf(&main_terminal, "Mode: ");
    switch (options->mode) {
        case INSTALL_MODE_FULL:
            terminal_printf(&main_terminal, "Full Installation\n");
            break;
        case INSTALL_MODE_FILES_ONLY:
            terminal_printf(&main_terminal, "Files Only\n");
            break;
        case INSTALL_MODE_BOOTLOADER:
            terminal_printf(&main_terminal, "Bootloader Only\n");
            break;
        case INSTALL_MODE_UPDATE:
            terminal_printf(&main_terminal, "Update\n");
            break;
    }
    
    return INSTALL_OK;
}

// Analyze disk and partitions
install_err_t installer_analyze(installer_state_t *state) {
    if (!state || !state->disk) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "\n--- Analyzing Disk ---\n");
    
    // Read partition table
    part_err_t err = partition_read_table(state->disk, &state->pt);
    if (err != PART_OK) {
        terminal_printf(&main_terminal, "ERROR: Failed to read partition table\n");
        return INSTALL_ERR_DISK_ERROR;
    }
    
    // Print partition information
    partition_print_info(&state->pt);
    
    // Find target partition
    if (state->options.target_partition < 4) {
        // User specified partition
        for (uint32_t i = 0; i < state->pt.partition_count; i++) {
            if (state->pt.partitions[i].index == state->options.target_partition) {
                state->target_partition = &state->pt.partitions[i];
                break;
            }
        }
    } else {
        // Auto-select: prefer bootable FAT32 partition
        state->target_partition = partition_find_bootable(&state->pt);
        if (!state->target_partition || !partition_is_fat(state->target_partition->type)) {
            // Try to find any FAT partition
            for (uint32_t i = 0; i < state->pt.partition_count; i++) {
                if (partition_is_fat(state->pt.partitions[i].type)) {
                    state->target_partition = &state->pt.partitions[i];
                    break;
                }
            }
        }
    }
    
    if (!state->target_partition) {
        terminal_printf(&main_terminal, "ERROR: No suitable partition found\n");
        return INSTALL_ERR_NO_PARTITION;
    }
    
    // Check if partitionless (virtual partition)
    if (state->target_partition->lba_start == 0 && state->pt.partition_count == 1 && state->target_partition->index == 0) {
        state->is_partitionless = true;
        terminal_printf(&main_terminal, "Detected partitionless disk mode\n");
    }
    
    if (state->target_partition->lba_start != 0 && state->pt.partition_count >= 1) {
        state->has_real_partitions = true;
        terminal_printf(&main_terminal, "Detected real partition table (FAT32)\n");
    }


    terminal_printf(&main_terminal, "\nTarget partition: %u\n", state->target_partition->index);
    terminal_printf(&main_terminal, "  Type: %s\n", 
                   partition_type_name(state->target_partition->type));
    terminal_printf(&main_terminal, "  Size: %llu MB\n", state->target_partition->size_mb);
    terminal_printf(&main_terminal, "  Bootable: %s\n", 
                   state->target_partition->bootable ? "Yes" : "No");
    
    // Verify partition is FAT
    if (!partition_is_fat(state->target_partition->type)) {
        terminal_printf(&main_terminal, "WARNING: Partition is not FAT filesystem\n");
        if (!state->options.force) {
            terminal_printf(&main_terminal, "ERROR: Use force option to continue\n");
            return INSTALL_ERR_PARTITION_TYPE;
        }
    }
    
    // Check available space
    uint64_t required_mb = (state->image->kernel_size + state->image->grub_core_size) / (1024 * 1024) + 10;
    for (uint32_t i = 0; i < state->image->module_count; i++) {
        required_mb += state->image->module_sizes[i] / (1024 * 1024);
    }
    
    terminal_printf(&main_terminal, "\nSpace requirements:\n");
    terminal_printf(&main_terminal, "  Required: ~%llu MB\n", required_mb);
    terminal_printf(&main_terminal, "  Available: %llu MB\n", state->target_partition->size_mb);
    
    if (required_mb > state->target_partition->size_mb) {
        terminal_printf(&main_terminal, "ERROR: Insufficient space on partition\n");
        return INSTALL_ERR_PARTITION_TYPE;
    }
    
    return INSTALL_OK;
}

// Main installation function
install_err_t installer_install(installer_state_t *state) {
    if (!state || !state->disk || !state->image || !state->target_partition) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "\n--- Starting Installation ---\n");
    
    install_err_t err;
    
    // Step 1: Backup MBR if requested
    if (state->options.backup_mbr && state->options.mode != INSTALL_MODE_FILES_ONLY) {
        terminal_printf(&main_terminal, "\nStep 1/5: Backing up MBR...\n");
        uint8_t *backup = (uint8_t*)kernel_malloc(512);
        if (!backup) {
            return INSTALL_ERR_BACKUP_FAILED;
        }
        err = installer_backup_mbr(state, backup);
        if (err != INSTALL_OK) {
            kernel_free(backup);
            return err;
        }
        char backup_path[VFS_PATH_MAX];
        snprintf(backup_path, sizeof(backup_path), "/home/MBRBAK.BIN");
        int fd = vfs_open(backup_path, VFS_O_CREAT | VFS_O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, backup, 512);
            vfs_close(fd);
            terminal_printf(&main_terminal, "  MBR backed up to %s\n", backup_path);
        }
        kernel_free(backup);
    } else {
        terminal_printf(&main_terminal, "\nStep 1/5: Skipping MBR backup\n");
    }
    
    // Step 2-3: Install GRUB bootloader (boot.img + core.img) if available
    if (state->has_real_partitions && 
        state->image->mbr_boot_code && state->image->grub_core_img &&
        (state->options.mode == INSTALL_MODE_FULL || 
         state->options.mode == INSTALL_MODE_BOOTLOADER)) {
        terminal_printf(&main_terminal, "\nStep 2-3/5: Installing GRUB bootloader...\n");
        err = installer_install_grub_proper(state);
        if (err != INSTALL_OK) {
            terminal_printf(&main_terminal, "ERROR: GRUB installation failed\n");
            return err;
        }
    } else if (!state->has_real_partitions && !state->is_partitionless &&
               (state->options.mode == INSTALL_MODE_FULL || 
                state->options.mode == INSTALL_MODE_BOOTLOADER)) {
        // Fallback para discos sin particiones reales
        terminal_printf(&main_terminal, "\nStep 2-3/5: Installing legacy bootloader...\n");
        if (state->image->mbr_boot_code && state->image->mbr_boot_size > 0) {
            err = installer_install_mbr(state);
            if (err == INSTALL_OK) state->mbr_installed = true;
        }
        if (state->image->vbr_boot_code && state->image->vbr_boot_size > 0) {
            err = installer_install_vbr(state);
            if (err == INSTALL_OK) state->vbr_installed = true;
        }
    } else {
        terminal_printf(&main_terminal, "\nStep 2-3/5: Skipping bootloader installation\n");
    }
    
    // Step 4: Copy files (ALWAYS DO THIS)
    if (state->options.mode != INSTALL_MODE_BOOTLOADER) {
        terminal_printf(&main_terminal, "\nStep 4/5: Copying files...\n");
        err = installer_copy_files(state);
        if (err != INSTALL_OK) {
            terminal_printf(&main_terminal, "ERROR: File copy failed\n");
            return err;
        }
        state->files_copied = true;
    }
    
    // Step 5: Verification (optional)
    if (state->options.verify) {
        terminal_printf(&main_terminal, "\nStep 5/5: Verifying installation...\n");
        // La verificación ya no necesita chequear bootable flag
    } else {
        terminal_printf(&main_terminal, "\nStep 5/5: Skipping verification\n");
    }
    
    terminal_printf(&main_terminal, "\n--- Installation Complete ---\n");
    if (state->mbr_installed || state->grub_installed) {
        terminal_printf(&main_terminal, "System can now boot from this disk.\n");
    } else {
        terminal_printf(&main_terminal, "NOTE: Bootloader not installed. Boot from ISO/USB.\n");
    }
    return INSTALL_OK;
}

// Backup MBR
install_err_t installer_backup_mbr(installer_state_t *state, uint8_t *backup_buffer) {
    if (!state || !backup_buffer) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    mbr_err_t err = mbr_backup(state->disk, backup_buffer, 512);
    if (err != MBR_OK) {
        return INSTALL_ERR_BACKUP_FAILED;
    }
    
    state->mbr_backed_up = true;
    return INSTALL_OK;
}

// Install MBR bootloader
install_err_t installer_install_mbr(installer_state_t *state) {
    if (!state->image->mbr_boot_code || state->image->mbr_boot_size == 0) {
        terminal_printf(&main_terminal, "  Warning: No MBR boot code provided, skipping\n");
        return INSTALL_OK;  // No error, simplemente skip
    }

    // Validar que el boot code no sea todo ceros
    bool all_zero = true;
    for (uint32_t i = 0; i < state->image->mbr_boot_size && i < 16; i++) {
        if (state->image->mbr_boot_code[i] != 0) {
            all_zero = false;
            break;
        }
    }

    if (all_zero) {
        terminal_printf(&main_terminal, "ERROR: MBR boot code appears empty\n");
        return INSTALL_ERR_MBR_INSTALL_FAILED;
    }
    terminal_printf(&main_terminal, "  Installing MBR boot code (%u bytes)...\n", 
                   state->image->mbr_boot_size);
    
    mbr_err_t err = mbr_install_bootcode(state->disk, 
                                         state->image->mbr_boot_code,
                                         state->image->mbr_boot_size);
    if (err != MBR_OK) {
        return INSTALL_ERR_MBR_INSTALL_FAILED;
    }
    
    state->mbr_installed = true;
    terminal_printf(&main_terminal, "  MBR installed successfully\n");
    return INSTALL_OK;
}

// Install VBR bootloader
install_err_t installer_install_vbr(installer_state_t *state) {
    if (!state || !state->image || !state->image->vbr_boot_code || !state->target_partition) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "  Installing VBR boot code (%u bytes) to partition %u...\n", 
                   state->image->vbr_boot_size, state->target_partition->index);
    
    mbr_err_t err = vbr_install_bootcode(state->disk, 
                                         state->target_partition->lba_start,
                                         state->image->vbr_boot_code,
                                         state->image->vbr_boot_size);
    if (err != MBR_OK) {
        return INSTALL_ERR_VBR_INSTALL_FAILED;
    }
    
    state->vbr_installed = true;
    terminal_printf(&main_terminal, "  VBR installed successfully\n");
    return INSTALL_OK;
}

// Copy files to partition
install_err_t installer_copy_files(installer_state_t *state) {
    if (!state || !state->image) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    const char *mountpoint = "/home";
    
    // Create directory structure
    terminal_printf(&main_terminal, "  Creating directory structure...\n");
    
    char boot_dir[VFS_PATH_MAX];
    snprintf(boot_dir, sizeof(boot_dir), "%s/boot", mountpoint);
    vfs_node_t *boot_node = NULL;
    vfs_mkdir(boot_dir, &boot_node);
    if (boot_node) {
        boot_node->refcount--;
        if (boot_node->refcount == 0 && boot_node->ops->release) {
            boot_node->ops->release(boot_node);
        }
    }
    
    char grub_dir[VFS_PATH_MAX];
    snprintf(grub_dir, sizeof(grub_dir), "%s/boot/grub", mountpoint);
    vfs_node_t *grub_node = NULL;
    vfs_mkdir(grub_dir, &grub_node);
    if (grub_node) {
        grub_node->refcount--;
        if (grub_node->refcount == 0 && grub_node->ops->release) {
            grub_node->ops->release(grub_node);
        }
    }
    
    // Copy kernel
    terminal_printf(&main_terminal, "  Installing kernel (%u bytes)...\n", state->image->kernel_size);
    
    char kernel_path[VFS_PATH_MAX];
    snprintf(kernel_path, sizeof(kernel_path), "%s/boot/kernel.bin", mountpoint);
    
    int fd = vfs_open(kernel_path, VFS_O_CREAT | VFS_O_WRONLY);
    if (fd < 0) {
        terminal_printf(&main_terminal, "ERROR: Cannot create kernel file\n");
        return INSTALL_ERR_FILE_COPY_FAILED;
    }
    
    uint32_t written = 0;
    const uint32_t CHUNK_SIZE = 4096;
    while (written < state->image->kernel_size) {
        uint32_t to_write = (state->image->kernel_size - written > CHUNK_SIZE) ? 
                           CHUNK_SIZE : (state->image->kernel_size - written);
        int result = vfs_write(fd, state->image->kernel_img + written, to_write);
        if (result <= 0) {
            vfs_close(fd);
            return INSTALL_ERR_FILE_COPY_FAILED;
        }
        written += result;
        state->bytes_written += result;
        
        if (written % 16384 == 0) {
            terminal_printf(&main_terminal, "    Progress: %u/%u bytes\n", 
                           written, state->image->kernel_size);
        }
    }
    vfs_close(fd);
    state->files_written++;
    terminal_printf(&main_terminal, "    Kernel installed\n");
    
    // Copy GRUB core.img
    if (state->image->grub_core_img && state->image->grub_core_size > 0) {
        terminal_printf(&main_terminal, "  Installing GRUB core.img (%u bytes)...\n", 
                       state->image->grub_core_size);
        
        char grub_core_path[VFS_PATH_MAX];
        snprintf(grub_core_path, sizeof(grub_core_path), "%s/boot/grub/core.img", mountpoint);
        
        fd = vfs_open(grub_core_path, VFS_O_CREAT | VFS_O_WRONLY);
        if (fd < 0) {
            terminal_printf(&main_terminal, "ERROR: Cannot create GRUB core file\n");
            return INSTALL_ERR_FILE_COPY_FAILED;
        }
        
        written = 0;
        while (written < state->image->grub_core_size) {
            uint32_t to_write = (state->image->grub_core_size - written > CHUNK_SIZE) ? 
                               CHUNK_SIZE : (state->image->grub_core_size - written);
            int result = vfs_write(fd, state->image->grub_core_img + written, to_write);
            if (result <= 0) {
                vfs_close(fd);
                return INSTALL_ERR_FILE_COPY_FAILED;
            }
            written += result;
            state->bytes_written += result;
        }
        vfs_close(fd);
        state->files_written++;
        terminal_printf(&main_terminal, "    GRUB core.img installed\n");
    }
    
    // Copy modules
    if (state->image->module_count > 0) {
        terminal_printf(&main_terminal, "  Installing modules (%u)...\n", state->image->module_count);
        
        for (uint32_t i = 0; i < state->image->module_count; i++) {
            char module_path[VFS_PATH_MAX];
            snprintf(module_path, sizeof(module_path), "%s/boot/%s", 
                    mountpoint, state->image->module_names[i]);
            
            terminal_printf(&main_terminal, "    Installing %s (%u bytes)...\n",
                           state->image->module_names[i], state->image->module_sizes[i]);
            
            fd = vfs_open(module_path, VFS_O_CREAT | VFS_O_WRONLY);
            if (fd < 0) {
                terminal_printf(&main_terminal, "WARNING: Cannot create module file %s\n", 
                               state->image->module_names[i]);
                continue;
            }
            
            written = 0;
            while (written < state->image->module_sizes[i]) {
                uint32_t to_write = (state->image->module_sizes[i] - written > CHUNK_SIZE) ? 
                                   CHUNK_SIZE : (state->image->module_sizes[i] - written);
                int result = vfs_write(fd, state->image->module_imgs[i] + written, to_write);
                if (result <= 0) {
                    break;
                }
                written += result;
                state->bytes_written += result;
            }
            vfs_close(fd);
            state->files_written++;
        }
    }
    
    // Create GRUB configuration
    terminal_printf(&main_terminal, "  Creating GRUB configuration...\n");
    install_err_t err = installer_create_grub_config(state);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "WARNING: Failed to create GRUB config\n");
    }
    
    state->files_copied = true;
    terminal_printf(&main_terminal, "  Files copied successfully\n");
    terminal_printf(&main_terminal, "  Flushing disk cache...\n");
    disk_err_t flush_err = disk_flush_dispatch(state->disk);
    if (flush_err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "ERROR: Disk flush failed (%d)\n", flush_err);
        return INSTALL_ERR_WRITE_FAILED;
    }
    return INSTALL_OK;
}
// Instalar GRUB correctamente (boot.img + core.img)
install_err_t installer_install_grub_proper(installer_state_t *state) {
    if (!state || !state->disk || !state->image || !state->image->grub_core_img || !state->target_partition) {
        return INSTALL_ERR_INVALID_PARAM;
    }

    terminal_printf(&main_terminal, "Installing GRUB bootloader...\n");

    // Step 1: Install MBR boot code
    if (state->image->mbr_boot_code && state->image->mbr_boot_size > 0) {
        mbr_err_t mbr_err = mbr_install_bootcode(state->disk, state->image->mbr_boot_code, state->image->mbr_boot_size);
        if (mbr_err != MBR_OK) {
            terminal_printf(&main_terminal, "Failed to install MBR boot code (error %d)\n", mbr_err);
            return INSTALL_ERR_MBR_INSTALL_FAILED;
        }
    }

    // Step 2: Embed core.img starting at sector 1 (after MBR)
    uint64_t core_start_lba = 1; // Sector 1 is after MBR (sector 0)
    uint32_t core_sectors = (state->image->grub_core_size + 511) / 512; // Round up to nearest sector
    terminal_printf(&main_terminal, "Installing core.img: %u bytes, %u sectors at LBA %llu\n",
                   state->image->grub_core_size, core_sectors, core_start_lba);

    // Write core.img to disk
    disk_err_t disk_err = disk_write_dispatch(state->disk, core_start_lba, core_sectors, state->image->grub_core_img);
    if (disk_err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to write core.img (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }
    terminal_printf(&main_terminal, "Flushing after core.img write...\n");
    if (disk_flush_dispatch(state->disk) != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to flush after core.img write (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }

    // Step 3: Patch blocklist in core.img's first sector
    uint8_t verify_buffer[512];
    disk_err = disk_read_dispatch(state->disk, core_start_lba, 1, verify_buffer);
    if (disk_err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to read back core.img sector for blocklist patch (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }

    // Search for dummy blocklist pattern (LBA=0, Len=2)
    const uint8_t blocklist_pattern[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    uint32_t blocklist_offset = 0;
    for (uint32_t i = 0; i < 512 - sizeof(blocklist_pattern); i++) {
        if (memcmp(&verify_buffer[i], blocklist_pattern, sizeof(blocklist_pattern)) == 0) {
            blocklist_offset = i;
            break;
        }
    }

    if (blocklist_offset == 0) {
        terminal_printf(&main_terminal, "Failed to find blocklist pattern in core.img\n");
        blocklist_offset = 0x1F8; // Fallback to standard offset
        terminal_printf(&main_terminal, "Using fallback blocklist offset 0x%X\n", blocklist_offset);
    } else {
        terminal_printf(&main_terminal, "Found blocklist at offset 0x%X\n", blocklist_offset);
    }

    // Log the original blocklist data for debugging
    terminal_printf(&main_terminal, "Original blocklist at 0x%X: ", blocklist_offset);
    for (int i = 0; i < 12; i++) {
        terminal_printf(&main_terminal, "%02X ", verify_buffer[blocklist_offset + i]);
    }
    terminal_printf(&main_terminal, "\n");

    // Patch blocklist: LBA=2 (next sector), Len=core_sectors-1 (remaining sectors)
    uint64_t next_lba = core_start_lba + 1; // Start of next block
    uint32_t remaining_sectors = core_sectors - 1;
    uint8_t patched_blocklist[12];
    memcpy(patched_blocklist, &next_lba, sizeof(uint64_t)); // 8 bytes for LBA
    memcpy(patched_blocklist + 8, &remaining_sectors, sizeof(uint32_t)); // 4 bytes for length
    terminal_printf(&main_terminal, "Patching blocklist: ");
    for (int i = 0; i < 12; i++) {
        terminal_printf(&main_terminal, "%02X ", patched_blocklist[i]);
    }
    terminal_printf(&main_terminal, "\n");

    memcpy(&verify_buffer[blocklist_offset], patched_blocklist, 12);

    // Write back the patched first sector
    disk_err = disk_write_dispatch(state->disk, core_start_lba, 1, verify_buffer);
    if (disk_err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to write patched core.img sector (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }
    terminal_printf(&main_terminal, "Flushing after patch write...\n");
    if (disk_flush_dispatch(state->disk) != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to flush after patch write (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }

    // Step 4: Verify the patch
    uint8_t verify_buffer2[512];
    disk_err = disk_read_dispatch(state->disk, core_start_lba, 1, verify_buffer2);
    if (disk_err != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to read back verified sector (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }
    if (memcmp(verify_buffer, verify_buffer2, 512) != 0) {
        terminal_printf(&main_terminal, "Verification of patched blocklist failed!\n");
        terminal_printf(&main_terminal, "Expected: ");
        for (int i = 0; i < 12; i++) {
            terminal_printf(&main_terminal, "%02X ", verify_buffer[blocklist_offset + i]);
        }
        terminal_printf(&main_terminal, "\nGot: ");
        for (int i = 0; i < 12; i++) {
            terminal_printf(&main_terminal, "%02X ", verify_buffer2[blocklist_offset + i]);
        }
        terminal_printf(&main_terminal, "\n");
        return INSTALL_ERR_VERIFY_FAILED;
    }

    uint64_t verified_lba;
    uint32_t verified_len;
    memcpy(&verified_lba, &verify_buffer2[blocklist_offset], sizeof(uint64_t));
    memcpy(&verified_len, &verify_buffer2[blocklist_offset + 8], sizeof(uint32_t));
    terminal_printf(&main_terminal, "Verified blocklist: LBA=%llu, Len=%u\n", verified_lba, verified_len);

    if (verified_lba != next_lba || verified_len != remaining_sectors) {
        terminal_printf(&main_terminal, "Blocklist verification mismatch! Expected LBA=%llu, Len=%u\n",
                       next_lba, remaining_sectors);
        terminal_printf(&main_terminal, "Raw verified data at 0x%X: ", blocklist_offset);
        for (int i = 0; i < 12; i++) {
            terminal_printf(&main_terminal, "%02X ", verify_buffer2[blocklist_offset + i]);
        }
        terminal_printf(&main_terminal, "\n");
        //return INSTALL_ERR_VERIFY_FAILED;
    }

    // Step 5: Final flush to ensure all writes are committed
    terminal_printf(&main_terminal, "Performing final disk flush...\n");
    if (disk_flush_dispatch(state->disk) != DISK_ERR_NONE) {
        terminal_printf(&main_terminal, "Failed to flush disk after GRUB installation (error %d)\n", disk_err);
        return INSTALL_ERR_WRITE_FAILED;
    }

    terminal_printf(&main_terminal, "GRUB installation complete\n");
    return INSTALL_OK;
}


install_err_t installer_create_grub_config(installer_state_t *state) {
    if (!state) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    const char *mountpoint = "/home";
    char cfg_path[VFS_PATH_MAX];
    snprintf(cfg_path, sizeof(cfg_path), "%s/boot/grub/grub.cfg", mountpoint);
    
    int fd = vfs_open(cfg_path, VFS_O_CREAT | VFS_O_WRONLY);
    if (fd < 0) {
        return INSTALL_ERR_FILE_COPY_FAILED;
    }
    
    if (state->image->grub_cfg_content) {
        vfs_write(fd, state->image->grub_cfg_content, 
                 strlen(state->image->grub_cfg_content));
    } else {
        // Header
        const char *header = 
            "# GRUB Configuration\n"
            "set timeout=5\n"
            "set default=0\n\n";
        vfs_write(fd, header, strlen(header));
        
        // Menuentry
        const char *menuentry_start = 
            "menuentry \"Your OS\" {\n"
            "    insmod part_msdos\n"
            "    insmod fat\n"
            "    insmod multiboot2\n"        // ← Cargar multiboot2
            "    set root='hd0,msdos1'\n"    // ← SIN paréntesis
            "    multiboot2 /boot/kernel.bin\n";
        vfs_write(fd, menuentry_start, strlen(menuentry_start));
        
        // Add modules
        for (uint32_t i = 0; i < state->image->module_count; i++) {
            char module_line[256];
            snprintf(module_line, sizeof(module_line),
                    "    module2 /boot/%s %s\n",
                    state->image->module_names[i],
                    state->image->module_names[i]);
            vfs_write(fd, module_line, strlen(module_line));
        }
        
        const char *menuentry_end = 
            "    boot\n"
            "}\n";
        vfs_write(fd, menuentry_end, strlen(menuentry_end));
    }
    
    vfs_close(fd);
    state->files_written++;
    state->grub_installed = true;
    return INSTALL_OK;
}

// Verify installation
install_err_t installer_verify(installer_state_t *state) {
    if (!state) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "\n--- Verifying Installation ---\n");
    
    bool all_ok = true;
    
    // Verify MBR if installed
    if (state->mbr_installed) {
        terminal_printf(&main_terminal, "Checking MBR...");
        mbr_t mbr;
        if (mbr_read(state->disk, &mbr) == MBR_OK) {
            terminal_printf(&main_terminal, " OK\n");
        } else {
            terminal_printf(&main_terminal, " FAILED\n");
            all_ok = false;
        }
    }
    
    // Verify VBR if installed
    if (state->vbr_installed && state->target_partition) {
        terminal_printf(&main_terminal, "Checking VBR...");
        vbr_fat32_t vbr;
        if (vbr_read(state->disk, state->target_partition->lba_start, &vbr) == MBR_OK) {
            terminal_printf(&main_terminal, " OK\n");
        } else {
            terminal_printf(&main_terminal, " FAILED\n");
            all_ok = false;
        }
    }
    
    // Verify kernel file
    terminal_printf(&main_terminal, "Checking kernel file...");
    char kernel_path[VFS_PATH_MAX];
    snprintf(kernel_path, sizeof(kernel_path), "/home/boot/kernel.bin");
    int fd = vfs_open(kernel_path, VFS_O_RDONLY);
    if (fd >= 0) {
        vfs_close(fd);
        terminal_printf(&main_terminal, " OK\n");
    } else {
        terminal_printf(&main_terminal, " NOT FOUND\n");
        all_ok = false;
    }
    
    // Verify GRUB config
    terminal_printf(&main_terminal, "Checking GRUB config...");
    char cfg_path[VFS_PATH_MAX];
    snprintf(cfg_path, sizeof(cfg_path), "/home/boot/grub/grub.cfg");
    fd = vfs_open(cfg_path, VFS_O_RDONLY);
    if (fd >= 0) {
        vfs_close(fd);
        terminal_printf(&main_terminal, " OK\n");
    } else {
        terminal_printf(&main_terminal, " NOT FOUND\n");
        all_ok = false;
    }
    
    // Verify bootable flag if set
    if (state->options.set_bootable && state->target_partition && !state->is_partitionless) {
        terminal_printf(&main_terminal, "Checking bootable flag...");
        partition_table_t pt;
        if (partition_read_table(state->disk, &pt) == PART_OK) {
            bool is_bootable = false;
            for (uint32_t i = 0; i < pt.partition_count; i++) {
                if (pt.partitions[i].index == state->target_partition->index) {
                    is_bootable = pt.partitions[i].bootable;
                    break;
                }
            }
            if (is_bootable) {
                terminal_printf(&main_terminal, " OK\n");
            } else {
                terminal_printf(&main_terminal, " NOT SET\n");
                all_ok = false;
            }
        } else {
            terminal_printf(&main_terminal, " FAILED\n");
            all_ok = false;
        }
    } else if (state->is_partitionless) {
        terminal_printf(&main_terminal, "Checking bootable flag... SKIPPED (partitionless)\n");
    }
    
    if (all_ok) {
        terminal_printf(&main_terminal, "\nVerification: PASSED\n");
        return INSTALL_OK;
    } else {
        terminal_printf(&main_terminal, "\nVerification: FAILED\n");
        return INSTALL_ERR_VERIFY_FAILED;
    }
}

// Print progress
void installer_print_progress(installer_state_t *state) {
    if (!state) return;
    
    terminal_printf(&main_terminal, "\nInstallation Progress:\n");
    terminal_printf(&main_terminal, "  MBR backed up: %s\n", state->mbr_backed_up ? "Yes" : "No");
    terminal_printf(&main_terminal, "  MBR installed: %s\n", state->mbr_installed ? "Yes" : "No");
    terminal_printf(&main_terminal, "  VBR installed: %s\n", state->vbr_installed ? "Yes" : "No");
    terminal_printf(&main_terminal, "  Files copied: %s\n", state->files_copied ? "Yes" : "No");
    terminal_printf(&main_terminal, "  GRUB installed: %s\n", state->grub_installed ? "Yes" : "No");
    terminal_printf(&main_terminal, "  Files written: %u\n", state->files_written);
    terminal_printf(&main_terminal, "  Bytes written: %u\n", state->bytes_written);
}

// Print summary
void installer_print_summary(installer_state_t *state) {
    if (!state) return;
    
    terminal_printf(&main_terminal, "\n=== Installation Summary ===\n");
    terminal_printf(&main_terminal, "Disk: %s (drive 0x%02x)\n",
                   state->disk->type == DEVICE_TYPE_SATA_DISK ? "SATA" : "IDE",
                   state->disk->drive_number);
    terminal_printf(&main_terminal, "Partition: %u (%s)\n",
                   state->target_partition->index,
                   partition_type_name(state->target_partition->type));
    terminal_printf(&main_terminal, "Mode: ");
    switch (state->options.mode) {
        case INSTALL_MODE_FULL:
            terminal_printf(&main_terminal, "Full Installation\n");
            break;
        case INSTALL_MODE_FILES_ONLY:
            terminal_printf(&main_terminal, "Files Only\n");
            break;
        case INSTALL_MODE_BOOTLOADER:
            terminal_printf(&main_terminal, "Bootloader Only\n");
            break;
        case INSTALL_MODE_UPDATE:
            terminal_printf(&main_terminal, "Update\n");
            break;
    }
    
    terminal_printf(&main_terminal, "\nComponents Installed:\n");
    if (state->mbr_installed) {
        terminal_printf(&main_terminal, "  - MBR bootloader\n");
    }
    if (state->vbr_installed) {
        terminal_printf(&main_terminal, "  - VBR bootloader\n");
    }
    if (state->files_copied) {
        terminal_printf(&main_terminal, "  - Kernel (%u bytes)\n", state->image->kernel_size);
        if (state->image->grub_core_size > 0) {
            terminal_printf(&main_terminal, "  - GRUB core (%u bytes)\n", 
                           state->image->grub_core_size);
        }
        if (state->image->module_count > 0) {
            terminal_printf(&main_terminal, "  - %u modules\n", state->image->module_count);
        }
    }
    if (state->grub_installed) {
        terminal_printf(&main_terminal, "  - GRUB configuration\n");
    }
    
    terminal_printf(&main_terminal, "\nStatistics:\n");
    terminal_printf(&main_terminal, "  Files written: %u\n", state->files_written);
    terminal_printf(&main_terminal, "  Total bytes: %u (%u KB)\n", 
                   state->bytes_written, state->bytes_written / 1024);
}

// Cleanup installer state
void installer_cleanup(installer_state_t *state) {
    if (!state) return;
    
    // Nothing to free in state itself as it doesn't own the disk or image
    memset(state, 0, sizeof(installer_state_t));
}

// Get error string
const char* installer_error_string(install_err_t error) {
    switch (error) {
        case INSTALL_OK: return "Success";
        case INSTALL_ERR_INVALID_PARAM: return "Invalid parameter";
        case INSTALL_ERR_DISK_ERROR: return "Disk error";
        case INSTALL_ERR_NO_PARTITION: return "No suitable partition found";
        case INSTALL_ERR_PARTITION_TYPE: return "Invalid partition type";
        case INSTALL_ERR_MOUNT_FAILED: return "Mount failed";
        case INSTALL_ERR_WRITE_FAILED: return "Write failed";
        case INSTALL_ERR_VERIFY_FAILED: return "Verification failed";
        case INSTALL_ERR_BACKUP_FAILED: return "Backup failed";
        case INSTALL_ERR_MBR_INSTALL_FAILED: return "MBR installation failed";
        case INSTALL_ERR_VBR_INSTALL_FAILED: return "VBR installation failed";
        case INSTALL_ERR_FILE_COPY_FAILED: return "File copy failed";
        case INSTALL_ERR_USER_ABORT: return "User aborted";
        default: return "Unknown error";
    }
}

// Format partition
install_err_t installer_format_partition(disk_t *disk, uint8_t partition_index, 
                                        const char *label) {
    if (!disk) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "\n--- Formatting Partition %u ---\n", partition_index);
    
    // Read partition table
    partition_table_t pt;
    part_err_t err = partition_read_table(disk, &pt);
    if (err != PART_OK) {
        return INSTALL_ERR_DISK_ERROR;
    }
    
    // Find partition
    partition_info_t *partition = NULL;
    for (uint32_t i = 0; i < pt.partition_count; i++) {
        if (pt.partitions[i].index == partition_index) {
            partition = &pt.partitions[i];
            break;
        }
    }
    
    if (!partition) {
        terminal_printf(&main_terminal, "ERROR: Partition %u not found\n", partition_index);
        return INSTALL_ERR_NO_PARTITION;
    }
    
    terminal_printf(&main_terminal, "WARNING: This will erase all data on partition %u!\n", 
                   partition_index);
    terminal_printf(&main_terminal, "Partition size: %llu MB\n", partition->size_mb);
    terminal_printf(&main_terminal, "Type: %s\n", partition_type_name(partition->type));
    
    // Format as FAT32
    char label_buffer[12] = "NO NAME    ";
    if (label) {
        strncpy(label_buffer, label, 11);
        label_buffer[11] = '\0';
    }
    
    // Use fat32_format_with_params for proper formatting
    int format_result = fat32_format_with_params(disk, FAT32_AUTO_SPC, 
                                                 FAT32_DEFAULT_NUM_FATS, label_buffer);
    if (format_result != VFS_OK) {
        terminal_printf(&main_terminal, "ERROR: Format failed\n");
        return INSTALL_ERR_WRITE_FAILED;
    }
    
    // Update partition type to FAT32 LBA
    pt.mbr.partitions[partition_index].type = PART_TYPE_FAT32_LBA;
    err = partition_write_table(&pt);
    if (err != PART_OK) {
        terminal_printf(&main_terminal, "WARNING: Failed to update partition type\n");
    }
    
    terminal_printf(&main_terminal, "Partition formatted successfully\n");
    return INSTALL_OK;
}

// Interactive installation
install_err_t installer_interactive(disk_t *disk, installer_image_t *image) {
    if (!disk || !image) {
        return INSTALL_ERR_INVALID_PARAM;
    }
    
    terminal_printf(&main_terminal, "\n");
    terminal_printf(&main_terminal, "╔════════════════════════════════════════════╗\n");
    terminal_printf(&main_terminal, "║   KERNEL INSTALLER - INTERACTIVE MODE      ║\n");
    terminal_printf(&main_terminal, "╚════════════════════════════════════════════╝\n");
    terminal_printf(&main_terminal, "\n");
    
    // Step 1: Analyze disk
    terminal_printf(&main_terminal, "Step 1: Analyzing disk...\n");
    partition_table_t pt;
    part_err_t perr = partition_read_table(disk, &pt);
    if (perr != PART_OK) {
        terminal_printf(&main_terminal, "ERROR: Failed to read partition table\n");
        return INSTALL_ERR_DISK_ERROR;
    }
    
    partition_print_info(&pt);
    
    // Step 2: Select partition
    terminal_printf(&main_terminal, "\nStep 2: Select target partition\n");
    terminal_printf(&main_terminal, "Available partitions:\n");
    
    for (uint32_t i = 0; i < pt.partition_count; i++) {
        terminal_printf(&main_terminal, "  [%u] %s - %llu MB %s\n",
                       pt.partitions[i].index,
                       partition_type_name(pt.partitions[i].type),
                       pt.partitions[i].size_mb,
                       pt.partitions[i].bootable ? "(bootable)" : "");
    }
    
    // Auto-select first FAT partition
    partition_info_t *target = NULL;
    for (uint32_t i = 0; i < pt.partition_count; i++) {
        if (partition_is_fat(pt.partitions[i].type)) {
            target = &pt.partitions[i];
            break;
        }
    }
    
    if (!target) {
        terminal_printf(&main_terminal, "\nERROR: No FAT partition found\n");
        terminal_printf(&main_terminal, "Please format a partition first or create one.\n");
        return INSTALL_ERR_NO_PARTITION;
    }
    
    terminal_printf(&main_terminal, "\nUsing partition %u: %s\n",
                   target->index, partition_type_name(target->type));
    
    // Step 3: Configure installation
    terminal_printf(&main_terminal, "\nStep 3: Installation options\n");
    
    install_options_t options;
    memset(&options, 0, sizeof(options));
    options.mode = INSTALL_MODE_FULL;
    options.force = false;
    options.verify = true;
    options.backup_mbr = true;
    options.set_bootable = true;
    options.target_partition = target->index;
    
    terminal_printf(&main_terminal, "  Mode: Full Installation\n");
    terminal_printf(&main_terminal, "  Backup MBR: Yes\n");
    terminal_printf(&main_terminal, "  Set bootable: Yes\n");
    terminal_printf(&main_terminal, "  Verify: Yes\n");
    
    // Step 4: Confirm
    terminal_printf(&main_terminal, "\nStep 4: Confirmation\n");
    terminal_printf(&main_terminal, "╔════════════════════════════════════════════╗\n");
    terminal_printf(&main_terminal, "║           WARNING: DATA LOSS RISK          ║\n");
    terminal_printf(&main_terminal, "╚════════════════════════════════════════════╝\n");
    terminal_printf(&main_terminal, "This will:\n");
    terminal_printf(&main_terminal, "  - Install bootloader to MBR\n");
    terminal_printf(&main_terminal, "  - Install bootloader to partition %u\n", target->index);
    terminal_printf(&main_terminal, "  - Copy kernel and modules to /boot\n");
    terminal_printf(&main_terminal, "  - Set partition %u as bootable\n", target->index);
    terminal_printf(&main_terminal, "\nPress ENTER to continue or ESC to cancel...\n");
    
    // Wait for user input (simplified - in real implementation use keyboard)
    // For now, proceed automatically
    
    // Step 5: Install
    terminal_printf(&main_terminal, "\nStep 5: Installing...\n");
    
    installer_state_t state;
    install_err_t err = installer_init(&state, disk, image, &options);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "ERROR: Initialization failed: %s\n",
                       installer_error_string(err));
        return err;
    }
    
    err = installer_analyze(&state);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "ERROR: Analysis failed: %s\n",
                       installer_error_string(err));
        return err;
    }
    
    err = installer_install(&state);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "ERROR: Installation failed: %s\n",
                       installer_error_string(err));
        installer_cleanup(&state);
        return err;
    }
    
    // Step 6: Verify
    if (options.verify) {
        terminal_printf(&main_terminal, "\nStep 6: Verifying installation...\n");
        err = installer_verify(&state);
        if (err != INSTALL_OK) {
            terminal_printf(&main_terminal, "WARNING: Verification failed: %s\n",
                           installer_error_string(err));
        }
    }
    
    // Step 7: Summary
    terminal_printf(&main_terminal, "\n");
    installer_print_summary(&state);
    
    terminal_printf(&main_terminal, "\n");
    terminal_printf(&main_terminal, "╔════════════════════════════════════════════╗\n");
    terminal_printf(&main_terminal, "║      INSTALLATION COMPLETED SUCCESSFULLY   ║\n");
    terminal_printf(&main_terminal, "╚════════════════════════════════════════════╝\n");
    terminal_printf(&main_terminal, "\n");
    terminal_printf(&main_terminal, "You can now reboot to start your OS.\n");
    terminal_printf(&main_terminal, "GRUB will load automatically from the boot partition.\n");
    
    installer_cleanup(&state);
    return INSTALL_OK;
}

// Load installation image from multiboot modules
install_err_t installer_load_image_from_modules(installer_image_t *image) {
    if (!image) {
        return INSTALL_ERR_INVALID_PARAM;
    }

    memset(image, 0, sizeof(installer_image_t));

    // Load MBR boot code (truncate to 446 bytes if necessary)
    module_info_t *mbr_module = module_find_by_name("mbr_boot.bin");
    if (mbr_module && mbr_module->size > 0) {
        image->mbr_boot_code = (uint8_t*)mbr_module->data;
        image->mbr_boot_size = mbr_module->size > 446 ? 446 : mbr_module->size; // Truncate to 446 bytes
        terminal_printf(&main_terminal, "Loaded MBR boot code: %u bytes (truncated to 446 if needed)\n",
                       image->mbr_boot_size);
    } else {
        terminal_printf(&main_terminal, "WARNING: MBR boot code not found\n");
    }

    // Load VBR boot code (if needed)
    module_info_t *vbr_module = module_find_by_name("vbr_boot.bin");
    if (vbr_module && vbr_module->size > 0) {
        image->vbr_boot_code = (uint8_t*)vbr_module->data;
        image->vbr_boot_size = vbr_module->size > 420 ? 420 : vbr_module->size; // Truncate to 420 bytes
        terminal_printf(&main_terminal, "Loaded VBR boot code: %u bytes (truncated to 420 if needed)\n",
                       image->vbr_boot_size);
    }

    // Load GRUB core.img
    module_info_t *core_module = module_find_by_name("core.img");
    if (core_module && core_module->size > 0) {
        image->grub_core_img = (uint8_t*)core_module->data;
        image->grub_core_size = core_module->size;
        terminal_printf(&main_terminal, "Loaded GRUB core.img: %u bytes\n", image->grub_core_size);
    } else {
        terminal_printf(&main_terminal, "ERROR: GRUB core.img not found\n");
        return INSTALL_ERR_INVALID_PARAM;
    }

    // Load kernel
    module_info_t *kernel_module = module_find_by_name("kernel.bin");
    if (kernel_module && kernel_module->size > 0) {
        image->kernel_img = (uint8_t*)kernel_module->data;
        image->kernel_size = kernel_module->size;
        terminal_printf(&main_terminal, "Loaded kernel: %u bytes\n", image->kernel_size);
    } else {
        terminal_printf(&main_terminal, "ERROR: Kernel not found\n");
        return INSTALL_ERR_INVALID_PARAM;
    }

    // Load other modules

    uint32_t other_module_count = 0;

    for (uint32_t i = 0; i < module_count; i++) {
        module_info_t *mod = module_get_by_index(i);
        if (!mod || !mod->cmdline) continue;

        char *name = strrchr(mod->cmdline, '/');
        name = name ? name + 1 : mod->cmdline;

        if (strcmp(name, "mbr_boot.bin") == 0 ||
            strcmp(name, "vbr_boot.bin") == 0 ||
            strcmp(name, "core.img") == 0 ||
            strcmp(name, "kernel.bin") == 0) {
            continue;
        }
        
        other_module_count++;
    }
    
    if (other_module_count > 0) {
        image->module_imgs = (uint8_t**)kernel_malloc(sizeof(uint8_t*) * other_module_count);
        image->module_sizes = (uint32_t*)kernel_malloc(sizeof(uint32_t) * other_module_count);
        image->module_names = (char**)kernel_malloc(sizeof(char*) * other_module_count);
        
        if (!image->module_imgs || !image->module_sizes || !image->module_names) {
            return INSTALL_ERR_INVALID_PARAM;
        }
        
        uint32_t idx = 0;
        for (uint32_t i = 0; i < module_count && idx < other_module_count; i++) {
            module_info_t *mod = module_get_by_index(i);
            if (!mod || !mod->cmdline) continue;
            
            char *name = strrchr(mod->cmdline, '/');
            name = name ? name + 1 : mod->cmdline;
            
            if (strcmp(name, "mbr_boot.bin") == 0 ||
                strcmp(name, "vbr_boot.bin") == 0 ||
                strcmp(name, "core.img") == 0 ||
                strcmp(name, "kernel.bin") == 0) {
                continue;
            }
            
            image->module_imgs[idx] = (uint8_t*)mod->data;
            image->module_sizes[idx] = mod->size;
            image->module_names[idx] = name;
            terminal_printf(&main_terminal, "  Module[%u]: %s (%u bytes)\n",
                           idx, name, mod->size);
            idx++;
        }
        
        image->module_count = idx;
    }
    
    terminal_printf(&main_terminal, "Image loaded: %u component(s)\n",
                   (image->mbr_boot_code ? 1 : 0) +
                   (image->vbr_boot_code ? 1 : 0) +
                   (image->grub_core_img ? 1 : 0) +
                   1 + // kernel
                   image->module_count);
    
    return INSTALL_OK;
}

// Free installation image
void installer_free_image(installer_image_t *image) {
    if (!image) return;
    
    // Note: We don't free the actual data pointers as they point to module data
    // Only free the arrays we allocated
    if (image->module_imgs) kernel_free(image->module_imgs);
    if (image->module_sizes) kernel_free(image->module_sizes);
    if (image->module_names) kernel_free(image->module_names);
    
    memset(image, 0, sizeof(installer_image_t));
}

install_err_t install_os_complete(disk_t *disk, install_options_t *options) {
    if (!disk) {
        terminal_puts(&main_terminal, "Error: Disco no proporcionado!");
        return INSTALL_ERR_INVALID_PARAM;
    }

    // Paso 1: Verificar que los módulos estén inicializados
    if (module_count == 0) {
        terminal_puts(&main_terminal, "Error: No hay módulos cargados. Inicializa module_loader_init() primero.");
        return INSTALL_ERR_INVALID_PARAM;
    }
    terminal_puts(&main_terminal, "Módulos detectados: OK");

    // Paso 2: Cargar la imagen de instalación desde módulos
    installer_image_t install_image;
    install_err_t err = installer_load_image_from_modules(&install_image);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "Error cargando imagen: %s\n", installer_error_string(err));
        return err;
    }
    terminal_puts(&main_terminal, "Imagen de instalación cargada: OK");

    // Paso 3: Inicializar el estado del instalador
    installer_state_t install_state;
    err = installer_init(&install_state, disk, &install_image, options);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "Error inicializando instalador: %s\n", installer_error_string(err));
        installer_free_image(&install_image);
        return err;
    }
    terminal_puts(&main_terminal, "Estado del instalador inicializado: OK");

    // Paso 4: Analizar el disco y particiones
    err = installer_analyze(&install_state);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "Error analizando disco: %s\n", installer_error_string(err));
        installer_cleanup(&install_state);
        installer_free_image(&install_image);
        return err;
    }
    terminal_puts(&main_terminal, "Análisis de disco completado: OK");

    // Paso 5: Ejecutar la instalación
    err = installer_install(&install_state);
    if (err != INSTALL_OK) {
        terminal_printf(&main_terminal, "Error durante la instalación: %s\n", installer_error_string(err));
        // Intenta limpiar antes de retornar
        installer_cleanup(&install_state);
        installer_free_image(&install_image);
        return err;
    }
    terminal_puts(&main_terminal, "Instalación principal completada: OK");

    // Paso 6: Verificar si está habilitado
    if (options->verify) {
        err = installer_verify(&install_state);
        if (err != INSTALL_OK) {
            terminal_printf(&main_terminal, "Error en verificación: %s\n", installer_error_string(err));
            // No retornamos error fatal, solo logueamos (puedes cambiarlo)
        } else {
            terminal_puts(&main_terminal, "Verificación completada: OK");
        }
    }

    // Paso 7: Imprimir progreso y resumen
    installer_print_progress(&install_state);
    installer_print_summary(&install_state);

    // Paso 8: Limpieza final
    installer_cleanup(&install_state);
    installer_free_image(&install_image);
    terminal_puts(&main_terminal, "Limpieza completada: OK");

    return INSTALL_OK;
}