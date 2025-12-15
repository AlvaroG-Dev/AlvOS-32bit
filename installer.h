#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"
#include "partition.h"
#include "vfs.h"

// Installation modes
typedef enum {
    INSTALL_MODE_FULL,         // Full installation (MBR + VBR + files)
    INSTALL_MODE_FILES_ONLY,   // Only copy files (safest)
    INSTALL_MODE_BOOTLOADER,   // Only install bootloader
    INSTALL_MODE_UPDATE        // Update existing installation
} install_mode_t;

// Installation options
typedef struct {
    install_mode_t mode;
    bool force;                // Force installation even if warnings
    bool verify;               // Verify after installation
    bool backup_mbr;           // Backup MBR before writing
    bool set_bootable;         // Set partition as bootable
    uint8_t target_partition;  // Target partition index (0-3)
} install_options_t;

// Installation image (from modules or pre-loaded)
typedef struct {
    // Bootloader components
    uint8_t *mbr_boot_code;       // MBR boot code (max 446 bytes)
    uint32_t mbr_boot_size;
    uint8_t *vbr_boot_code;       // VBR boot code (max 420 bytes)
    uint32_t vbr_boot_size;
    uint8_t *grub_core_img;       // GRUB core.img
    uint32_t grub_core_size;
    
    // Kernel and modules
    uint8_t *kernel_img;
    uint32_t kernel_size;
    uint8_t **module_imgs;        // Array of module images
    uint32_t *module_sizes;
    char **module_names;
    uint32_t module_count;
    
    // Configuration
    char *grub_cfg_content;
    char volume_label[12];
} installer_image_t;

// Installation state
typedef struct {
    disk_t *disk;
    partition_table_t pt;
    partition_info_t *target_partition;
    install_options_t options;
    installer_image_t *image;
    
    // State tracking
    bool mbr_backed_up;
    bool mbr_installed;
    bool vbr_installed;
    bool files_copied;
    bool grub_installed;
    bool is_partitionless;
    bool has_real_partitions;

    // Statistics
    uint32_t bytes_written;
    uint32_t files_written;
} installer_state_t;

// Error codes
typedef enum {
    INSTALL_OK = 0,
    INSTALL_ERR_INVALID_PARAM,
    INSTALL_ERR_DISK_ERROR,
    INSTALL_ERR_NO_PARTITION,
    INSTALL_ERR_PARTITION_TYPE,
    INSTALL_ERR_MOUNT_FAILED,
    INSTALL_ERR_WRITE_FAILED,
    INSTALL_ERR_VERIFY_FAILED,
    INSTALL_ERR_BACKUP_FAILED,
    INSTALL_ERR_MBR_INSTALL_FAILED,
    INSTALL_ERR_VBR_INSTALL_FAILED,
    INSTALL_ERR_FILE_COPY_FAILED,
    INSTALL_ERR_USER_ABORT
} install_err_t;

// Main installation functions
install_err_t installer_init(installer_state_t *state, disk_t *disk, 
                             installer_image_t *image, install_options_t *options);
install_err_t installer_analyze(installer_state_t *state);
install_err_t installer_install(installer_state_t *state);
install_err_t installer_verify(installer_state_t *state);
void installer_cleanup(installer_state_t *state);

// Step-by-step installation
install_err_t installer_backup_mbr(installer_state_t *state, uint8_t *backup_buffer);
install_err_t installer_install_mbr(installer_state_t *state);
install_err_t installer_install_vbr(installer_state_t *state);
install_err_t installer_copy_files(installer_state_t *state);
install_err_t installer_install_grub(installer_state_t *state);
install_err_t installer_create_grub_config(installer_state_t *state);

// Utility functions
install_err_t installer_format_partition(disk_t *disk, uint8_t partition_index, 
                                        const char *label);
install_err_t installer_mount_partition(installer_state_t *state, const char *mountpoint);
const char* installer_error_string(install_err_t error);
void installer_print_progress(installer_state_t *state);
void installer_print_summary(installer_state_t *state);
install_err_t installer_install_grub_proper(installer_state_t *state);

// Interactive installation
install_err_t installer_interactive(disk_t *disk, installer_image_t *image);

// Image loading from modules
install_err_t installer_load_image_from_modules(installer_image_t *image);
void installer_free_image(installer_image_t *image);
install_err_t install_os_complete(disk_t *disk, install_options_t *options);

#endif // INSTALLER_H