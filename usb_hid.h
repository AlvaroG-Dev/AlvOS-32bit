#ifndef USB_HID_H
#define USB_HID_H

#include "usb_core.h"

// HID Class/Subclass/Protocol
#define USB_CLASS_HID 0x03
#define USB_HID_SUBCLASS_BOOT 0x01
#define USB_HID_PROTO_KEYBOARD 0x01
#define USB_HID_PROTO_MOUSE 0x02

// HID Requests
#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_GET_IDLE 0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

// HID Descriptor Types
#define HID_DESC_HID 0x21
#define HID_DESC_REPORT 0x22
#define HID_DESC_PHYSICAL 0x23

typedef struct {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bcdHID;
  uint8_t bCountryCode;
  uint8_t bNumDescriptors;
  uint8_t bReportDescriptorType;
  uint16_t wReportDescriptorLength;
} __attribute__((packed)) usb_hid_descriptor_t;

typedef struct {
  usb_device_t *device;
  bool initialized;
  uint8_t interface_num;
  uint8_t ep_in;  // Interrupt IN endpoint
  uint8_t ep_out; // Interrupt OUT endpoint (optional)
  uint16_t max_packet_size;
  uint8_t poll_interval; // In frames/microframes

  // Status
  uint8_t protocol; // 0=Boot, 1=Report
  bool is_keyboard;
  bool is_mouse;

  // Buffer for interrupt transfers
  uint8_t *transfer_buffer;
} usb_hid_device_t;

void usb_hid_register_driver(void);
bool usb_hid_probe(usb_device_t *device);
bool usb_hid_init(usb_device_t *device);
void usb_hid_cleanup(usb_device_t *device);

// Polling function (should be called periodically if no IRQ support)
void usb_hid_poll(void);

#endif // USB_HID_H
