#ifndef USB_UHCI_H
#define USB_UHCI_H

#include "usb_core.h"
#include "dma.h"

// UHCI Register Offsets
#define UHCI_REG_USBCMD     0x00
#define UHCI_REG_USBSTS     0x02
#define UHCI_REG_USBINTR    0x04
#define UHCI_REG_FRNUM      0x06
#define UHCI_REG_FRBASEADD  0x08
#define UHCI_REG_SOFMOD     0x0C
#define UHCI_REG_PORTSC1    0x10
#define UHCI_REG_PORTSC2    0x12

// UHCI Command Register Bits
#define UHCI_CMD_RS         (1 << 0)  // Run/Stop
#define UHCI_CMD_HCRESET    (1 << 1)  // Host Controller Reset
#define UHCI_CMD_GRESET     (1 << 2)  // Global Reset
#define UHCI_CMD_EGSM       (1 << 3)  // Enter Global Suspend Mode
#define UHCI_CMD_FGR        (1 << 4)  // Force Global Resume
#define UHCI_CMD_SWDBG      (1 << 5)  // Software Debug
#define UHCI_CMD_CF         (1 << 6)  // Configure Flag
#define UHCI_CMD_MAXP       (1 << 7)  // Max Packet (64 bytes)

// UHCI Status Register Bits
#define UHCI_STS_USBINT     (1 << 0)  // USB Interrupt
#define UHCI_STS_ERROR      (1 << 1)  // USB Error Interrupt
#define UHCI_STS_RD         (1 << 2)  // Resume Detect
#define UHCI_STS_HSE        (1 << 3)  // Host System Error
#define UHCI_STS_HCPE       (1 << 4)  // Host Controller Process Error
#define UHCI_STS_HCH        (1 << 5)  // HC Halted

// Port Status Bits
#define UHCI_PORT_CCS       (1 << 0)  // Current Connect Status
#define UHCI_PORT_CSC       (1 << 1)  // Connect Status Change
#define UHCI_PORT_PE        (1 << 2)  // Port Enable
#define UHCI_PORT_PEC       (1 << 3)  // Port Enable Change
#define UHCI_PORT_LS        (3 << 4)  // Line Status
#define UHCI_PORT_RD        (1 << 6)  // Resume Detect
#define UHCI_PORT_LSDA      (1 << 8)  // Low Speed Device Attached
#define UHCI_PORT_PR        (1 << 9)  // Port Reset
#define UHCI_PORT_SUSP      (1 << 12) // Suspend

// Transfer Descriptor (TD) bits
#define UHCI_TD_ACTLEN_MASK 0x7FF
#define UHCI_TD_STATUS_MASK 0xFF
#define UHCI_TD_SPD         (1 << 29) // Short Packet Detect
#define UHCI_TD_LS          (1 << 26) // Low Speed
#define UHCI_TD_IOC         (1 << 24) // Interrupt on Complete
#define UHCI_TD_ACTIVE      (1 << 23) // Active
#define UHCI_TD_STALLED     (1 << 22) // Stalled
#define UHCI_TD_DBUF        (1 << 21) // Data Buffer Error
#define UHCI_TD_BABBLE      (1 << 20) // Babble Detected
#define UHCI_TD_NAK         (1 << 19) // NAK Received
#define UHCI_TD_CRCTO       (1 << 18) // CRC/Time Out Error
#define UHCI_TD_BITSTUFF    (1 << 17) // Bit Stuff Error

// PID tokens
#define UHCI_PID_SETUP      0x2D
#define UHCI_PID_IN         0x69
#define UHCI_PID_OUT        0xE1

#define UHCI_FRAME_LIST_SIZE 1024

// UHCI Transfer Descriptor
typedef struct uhci_td {
    uint32_t link_ptr;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
    
    // Software-only fields
    uint32_t reserved[4];
} __attribute__((packed, aligned(16))) uhci_td_t;

// UHCI Queue Head
typedef struct uhci_qh {
    uint32_t head_link_ptr;
    uint32_t element_link_ptr;
    
    // Software-only fields
    uint32_t reserved[14];
} __attribute__((packed, aligned(16))) uhci_qh_t;

// UHCI Controller Data
typedef struct {
    uint16_t io_base;
    
    dma_buffer_t* frame_list_buffer;
    uint32_t* frame_list;
    
    dma_buffer_t* qh_buffer;
    uhci_qh_t* control_qh;
    uhci_qh_t* bulk_qh;
    
    dma_buffer_t* td_pool_buffer;
    uhci_td_t* td_pool;
    bool td_used[64];
} uhci_data_t;

// Function prototypes
bool uhci_init(usb_controller_t* controller);
void uhci_cleanup(usb_controller_t* controller);
bool uhci_detect_ports(usb_controller_t* controller);
bool uhci_reset_port(usb_controller_t* controller, uint8_t port);

// Transfer functions
bool uhci_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                           void* data, uint16_t length);
bool uhci_bulk_transfer(usb_device_t* device, uint8_t endpoint,
                        void* data, uint32_t length, bool is_in);

// Internal functions
uhci_td_t* uhci_alloc_td(uhci_data_t* uhci);
void uhci_free_td(uhci_data_t* uhci, uhci_td_t* td);
bool uhci_wait_for_td(uhci_td_t* td, uint32_t timeout_ms);

#endif // USB_UHCI_H