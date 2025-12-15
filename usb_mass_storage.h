#ifndef USB_MASS_STORAGE_H
#define USB_MASS_STORAGE_H

#include "usb_core.h"
#include <stdint.h>
#include <stdbool.h>

// Mass Storage Class Codes
#define USB_MSC_SUBCLASS_SCSI       0x06  // SCSI Transparent
#define USB_MSC_PROTOCOL_BBB        0x50  // Bulk-Only Transport (BOT)

// SCSI Commands
#define SCSI_CMD_TEST_UNIT_READY    0x00
#define SCSI_CMD_REQUEST_SENSE      0x03
#define SCSI_CMD_INQUIRY            0x12
#define SCSI_CMD_READ_CAPACITY_10   0x25
#define SCSI_CMD_READ_10            0x28
#define SCSI_CMD_WRITE_10           0x2A

// CBW (Command Block Wrapper)
#define CBW_SIGNATURE               0x43425355  // "USBC"
#define CBW_FLAG_DATA_IN            0x80
#define CBW_FLAG_DATA_OUT           0x00

// CSW (Command Status Wrapper)
#define CSW_SIGNATURE               0x53425355  // "USBS"
#define CSW_STATUS_GOOD             0x00
#define CSW_STATUS_FAILED           0x01
#define CSW_STATUS_PHASE_ERROR      0x02

#define USB_MSC_MAX_DEVICES         8

// Command Block Wrapper
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} usb_msc_cbw_t;

// Command Status Wrapper
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} usb_msc_csw_t;

// SCSI Inquiry Response
typedef struct __attribute__((packed)) {
    uint8_t  peripheral_device_type;
    uint8_t  removable;
    uint8_t  version;
    uint8_t  response_data_format;
    uint8_t  additional_length;
    uint8_t  flags[3];
    uint8_t  vendor_id[8];
    uint8_t  product_id[16];
    uint8_t  revision[4];
} scsi_inquiry_response_t;

// SCSI Read Capacity Response
typedef struct __attribute__((packed)) {
    uint32_t last_lba;
    uint32_t block_size;
} scsi_read_capacity_response_t;

// USB Mass Storage Device
typedef struct {
    usb_device_t* usb_device;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t max_lun;
    
    uint32_t block_count;
    uint32_t block_size;
    
    uint32_t tag_counter;
    bool initialized;
} usb_msc_device_t;

// Global array of mass storage devices
extern usb_msc_device_t usb_msc_devices[USB_MSC_MAX_DEVICES];
extern uint8_t usb_msc_device_count;

// Driver registration
void usb_msc_register_driver(void);

// Device management
bool usb_msc_probe(usb_device_t* device);
bool usb_msc_init(usb_device_t* device);
void usb_msc_cleanup(usb_device_t* device);

// SCSI operations
bool usb_msc_inquiry(usb_msc_device_t* msc);
bool usb_msc_test_unit_ready(usb_msc_device_t* msc);
bool usb_msc_read_capacity(usb_msc_device_t* msc);
bool usb_msc_read_blocks(usb_msc_device_t* msc, uint32_t lba, uint16_t count, void* buffer);
bool usb_msc_write_blocks(usb_msc_device_t* msc, uint32_t lba, uint16_t count, const void* buffer);

// Utility functions
void usb_msc_list_devices(void);
usb_msc_device_t* usb_msc_get_device(uint8_t index);
uint8_t usb_msc_get_device_count(void);

#endif // USB_MASS_STORAGE_H