#ifndef USB_DISK_WRAPPER_H
#define USB_DISK_WRAPPER_H

#include "disk.h"
#include "usb_mass_storage.h"

// USB disk IDs start at 0xF0 to avoid conflicts with other disks
#define USB_DISK_BASE_ID 0xF0

// Initialize USB disk wrapper system
void usb_disk_init_system(void);

// Scan for USB storage devices
void usb_scan_for_storage(void);

// Initialize a disk_t structure for a USB device
disk_err_t usb_disk_init(disk_t* disk, uint32_t usb_device_id);

// Get number of USB storage devices
uint32_t usb_disk_get_count(void);

// Disk operations for USB devices
disk_err_t usb_disk_read(disk_t* disk, uint64_t lba, uint32_t count, void* buffer);
disk_err_t usb_disk_write(disk_t* disk, uint64_t lba, uint32_t count, const void* buffer);
disk_err_t usb_disk_flush(disk_t* disk);

// Helper to check if a disk_t is a USB disk
bool disk_is_usb(disk_t* disk);

#endif // USB_DISK_WRAPPER_H