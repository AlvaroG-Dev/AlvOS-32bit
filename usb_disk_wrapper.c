#include "usb_disk_wrapper.h"
#include "usb_core.h"
#include "usb_mass_storage.h"
#include "usb_uhci.h"
#include "usb_ehci.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"

// ========================================================================
// INITIALIZATION
// ========================================================================

void usb_disk_init_system(void) {
    // Register USB Mass Storage driver
    usb_msc_register_driver();
}

void usb_scan_for_storage(void) {
    terminal_puts(&main_terminal, "Scanning for USB storage devices...\r\n");
    
    // Trigger device enumeration on all controllers
    for (uint8_t i = 0; i < usb_controller_count; i++) {
        usb_controller_t* ctrl = &usb_controllers[i];
        if (!ctrl->initialized) continue;
        
        // Re-scan ports for new devices
        switch (ctrl->type) {
            case USB_TYPE_UHCI:
                uhci_detect_ports(ctrl);
                break;
            case USB_TYPE_EHCI:
                ehci_detect_ports(ctrl);
                break;
            // Add other controller types as implemented
        }
    }
    
    // List found devices
    usb_msc_list_devices();
}

// ========================================================================
// DISK INITIALIZATION
// ========================================================================

disk_err_t usb_disk_init(disk_t* disk, uint32_t usb_device_id) {
    if (!disk) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    if (usb_device_id >= usb_msc_get_device_count()) {
        terminal_printf(&main_terminal, "USB: Invalid device ID %u\r\n", usb_device_id);
        return DISK_ERR_INVALID_PARAM;
    }
    
    usb_msc_device_t* msc = usb_msc_get_device(usb_device_id);
    if (!msc || !msc->initialized) {
        terminal_puts(&main_terminal, "USB: Device not initialized\r\n");
        return DISK_ERR_NOT_INITIALIZED;
    }
    
    // Initialize disk structure
    memset(disk, 0, sizeof(disk_t));
    
    disk->drive_number = USB_DISK_BASE_ID + usb_device_id;
    disk->type = DEVICE_TYPE_USB_DISK;
    disk->initialized = 1;
    disk->present = 1;
    disk->supports_lba48 = 1;  // USB supports large capacities
    
    // Handle different block sizes
    if (msc->block_size == 512) {
        // Standard sector size
        disk->sector_count = msc->block_count;
    } else if (msc->block_size == 4096) {
        // Advanced Format (4K native)
        disk->sector_count = (uint64_t)msc->block_count * 8;
    } else if (msc->block_size == 2048) {
        // CD/DVD sector size
        disk->sector_count = (uint64_t)msc->block_count * 4;
    } else {
        // Generic conversion
        disk->sector_count = ((uint64_t)msc->block_count * msc->block_size) / 512;
    }
    
    terminal_printf(&main_terminal, "USB disk initialized: %llu sectors (%u MB)\r\n",
                   disk->sector_count,
                   (uint32_t)((disk->sector_count * 512) / (1024 * 1024)));
    terminal_printf(&main_terminal, "  Native block size: %u bytes\r\n", msc->block_size);
    
    return DISK_ERR_NONE;
}

uint32_t usb_disk_get_count(void) {
    return usb_msc_get_device_count();
}

// ========================================================================
// DISK OPERATIONS
// ========================================================================

disk_err_t usb_disk_read(disk_t* disk, uint64_t lba, uint32_t count, void* buffer) {
    if (!disk || !disk->initialized || !buffer) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    if (!disk_is_usb(disk)) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    uint32_t usb_id = disk->drive_number - USB_DISK_BASE_ID;
    usb_msc_device_t* msc = usb_msc_get_device(usb_id);
    
    if (!msc || !msc->initialized) {
        return DISK_ERR_NOT_INITIALIZED;
    }
    
    // Handle different block sizes
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t sectors_read = 0;
    
    if (msc->block_size == 512) {
        // Direct 1:1 mapping
        while (sectors_read < count) {
            uint32_t chunk = count - sectors_read;
            if (chunk > 256) chunk = 256;
            
            if (!usb_msc_read_blocks(msc, lba + sectors_read, chunk, buf)) {
                terminal_printf(&main_terminal, "USB disk read failed at LBA %llu\r\n", 
                               lba + sectors_read);
                return DISK_ERR_ATA;
            }
            
            sectors_read += chunk;
            buf += chunk * 512;
            for (volatile int i = 0; i < 10000; i++);
        }
    } else if (msc->block_size == 4096) {
        // 4K native - 8 sectors per block
        uint32_t start_block = lba / 8;
        uint32_t end_block = (lba + count - 1) / 8;
        uint32_t total_blocks = end_block - start_block + 1;
        
        // Allocate temporary buffer for aligned reads
        uint8_t* temp = (uint8_t*)kernel_malloc(total_blocks * 4096);
        if (!temp) return DISK_ERR_ATA;
        
        if (!usb_msc_read_blocks(msc, start_block, total_blocks, temp)) {
            kernel_free(temp);
            return DISK_ERR_ATA;
        }
        
        // Copy relevant data
        uint32_t offset = (lba % 8) * 512;
        memcpy(buffer, temp + offset, count * 512);
        kernel_free(temp);
    } else {
        // Generic handling
        uint32_t sectors_per_block = msc->block_size / 512;
        uint32_t start_block = lba / sectors_per_block;
        uint32_t end_block = (lba + count - 1) / sectors_per_block;
        uint32_t total_blocks = end_block - start_block + 1;
        
        uint8_t* temp = (uint8_t*)kernel_malloc(total_blocks * msc->block_size);
        if (!temp) return DISK_ERR_ATA;
        
        if (!usb_msc_read_blocks(msc, start_block, total_blocks, temp)) {
            kernel_free(temp);
            return DISK_ERR_ATA;
        }
        
        uint32_t offset = (lba % sectors_per_block) * 512;
        memcpy(buffer, temp + offset, count * 512);
        kernel_free(temp);
    }
    
    return DISK_ERR_NONE;
}

disk_err_t usb_disk_write(disk_t* disk, uint64_t lba, uint32_t count, const void* buffer) {
    if (!disk || !disk->initialized || !buffer) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    if (!disk_is_usb(disk)) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    uint32_t usb_id = disk->drive_number - USB_DISK_BASE_ID;
    usb_msc_device_t* msc = usb_msc_get_device(usb_id);
    
    if (!msc || !msc->initialized) {
        return DISK_ERR_NOT_INITIALIZED;
    }
    
    // Write in chunks
    uint32_t sectors_written = 0;
    const uint8_t* buf = (const uint8_t*)buffer;
    
    while (sectors_written < count) {
        uint32_t chunk = count - sectors_written;
        if (chunk > 256) chunk = 256;  // Write max 256 sectors at a time
        
        if (!usb_msc_write_blocks(msc, lba + sectors_written, chunk, buf)) {
            terminal_printf(&main_terminal, "USB disk write failed at LBA %llu\r\n",
                           lba + sectors_written);
            return DISK_ERR_ATA;
        }
        
        sectors_written += chunk;
        buf += chunk * 512;
        for (volatile int i = 0; i < 10000; i++);
    }
    
    return DISK_ERR_NONE;
}

disk_err_t usb_disk_flush(disk_t* disk) {
    if (!disk || !disk->initialized) {
        return DISK_ERR_NOT_INITIALIZED;
    }
    
    if (!disk_is_usb(disk)) {
        return DISK_ERR_INVALID_PARAM;
    }
    
    // USB Mass Storage doesn't require explicit flush
    // Commands are synchronous
    return DISK_ERR_NONE;
}

bool disk_is_usb(disk_t* disk) {
    return disk && disk->type == DEVICE_TYPE_USB_DISK && 
           disk->drive_number >= USB_DISK_BASE_ID &&
           disk->drive_number < (USB_DISK_BASE_ID + USB_MSC_MAX_DEVICES);
}