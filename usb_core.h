#ifndef USB_CORE_H
#define USB_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

// USB Classes
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09

// USB Request Types
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

// USB Descriptor Types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

// USB Speeds
#define USB_SPEED_LOW           0
#define USB_SPEED_FULL          1
#define USB_SPEED_HIGH          2

// USB Controller Types
#define USB_TYPE_UHCI           0  // USB 1.1
#define USB_TYPE_OHCI           1  // USB 1.1 (alternate)
#define USB_TYPE_EHCI           2  // USB 2.0
#define USB_TYPE_XHCI           3  // USB 3.0

#define USB_MAX_CONTROLLERS     8
#define USB_MAX_DEVICES         32
#define USB_MAX_ENDPOINTS       16

// USB Device Descriptor
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

// USB Configuration Descriptor
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

// USB Interface Descriptor
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor_t;

// USB Endpoint Descriptor
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

// USB Setup Packet
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

// USB Device
typedef struct {
    uint8_t address;
    uint8_t speed;
    uint8_t controller_id;
    uint8_t port;
    bool connected;
    
    usb_device_descriptor_t descriptor;
    uint8_t config_value;
    
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    
    void* driver_data;  // Driver-specific data
    
    uint8_t ep_toggles[32];  // Added: Index = (ep_num & 0x0F) + (is_in ? 16 : 0); 0=OUT, 1=IN directions
} usb_device_t;

// USB Controller
typedef struct {
    uint8_t type;
    uint8_t id;
    pci_device_t* pci_dev;
    void* regs;
    uint32_t regs_physical;
    bool initialized;
    
    usb_device_t devices[USB_MAX_DEVICES];
    uint8_t device_count;
} usb_controller_t;

// USB Driver Interface
typedef struct {
    const char* name;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    
    bool (*probe)(usb_device_t* device);
    bool (*init)(usb_device_t* device);
    void (*cleanup)(usb_device_t* device);
} usb_driver_t;

// Global USB state
extern usb_controller_t usb_controllers[USB_MAX_CONTROLLERS];
extern uint8_t usb_controller_count;

// Core functions
bool usb_init(void);
void usb_cleanup(void);
usb_controller_t* usb_detect_controllers(void);

// Device management
usb_device_t* usb_allocate_device(usb_controller_t* controller);
bool usb_enumerate_device(usb_controller_t* controller, uint8_t port);
void usb_remove_device(usb_device_t* device);

// Transfer functions (controller-specific, implemented by each driver)
bool usb_control_transfer(usb_device_t* device, usb_setup_packet_t* setup, 
                          void* data, uint16_t length);
bool usb_bulk_transfer(usb_device_t* device, uint8_t endpoint, 
                       void* data, uint32_t length, bool is_in);

// Descriptor functions
bool usb_get_device_descriptor(usb_device_t* device);
bool usb_get_config_descriptor(usb_device_t* device, uint8_t config_index, 
                               void* buffer, uint16_t length);
bool usb_set_configuration(usb_device_t* device, uint8_t config_value);
bool usb_set_address(usb_device_t* device, uint8_t address);

// Driver registration
bool usb_register_driver(usb_driver_t* driver);
void usb_scan_for_drivers(usb_device_t* device);
bool usb_clear_endpoint_halt(usb_device_t* device, uint8_t endpoint);

// Utility functions
void usb_list_devices(void);
const char* usb_get_class_name(uint8_t class_code);
const char* usb_get_speed_name(uint8_t speed);

#endif // USB_CORE_H