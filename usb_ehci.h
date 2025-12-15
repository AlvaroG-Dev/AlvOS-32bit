#ifndef USB_EHCI_H
#define USB_EHCI_H

#include "usb_core.h"
#include "dma.h"

// EHCI Capability Registers (offsets from base)
#define EHCI_CAP_CAPLENGTH  0x00
#define EHCI_CAP_HCIVERSION 0x02
#define EHCI_CAP_HCSPARAMS  0x04
#define EHCI_CAP_HCCPARAMS  0x08

// EHCI Operational Registers (offsets from CAPLENGTH)
#define EHCI_OP_USBCMD      0x00
#define EHCI_OP_USBSTS      0x04
#define EHCI_OP_USBINTR     0x08
#define EHCI_OP_FRINDEX     0x0C
#define EHCI_OP_CTRLDSSEGMENT 0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR 0x18
#define EHCI_OP_CONFIGFLAG  0x40
#define EHCI_OP_PORTSC_BASE 0x44

// USBCMD Register Bits
#define EHCI_CMD_RS         (1 << 0)   // Run/Stop
#define EHCI_CMD_HCRESET    (1 << 1)   // Host Controller Reset
#define EHCI_CMD_FLS_1024   (0 << 2)   // Frame List Size 1024
#define EHCI_CMD_PSE        (1 << 4)   // Periodic Schedule Enable
#define EHCI_CMD_ASE        (1 << 5)   // Async Schedule Enable
#define EHCI_CMD_IAAD       (1 << 6)   // Interrupt on Async Advance Doorbell
#define EHCI_CMD_LHCRESET   (1 << 7)   // Light Host Controller Reset

// USBSTS Register Bits
#define EHCI_STS_USBINT     (1 << 0)   // USB Interrupt
#define EHCI_STS_USBERRINT  (1 << 1)   // USB Error Interrupt
#define EHCI_STS_PCD        (1 << 2)   // Port Change Detect
#define EHCI_STS_FLR        (1 << 3)   // Frame List Rollover
#define EHCI_STS_HSE        (1 << 4)   // Host System Error
#define EHCI_STS_IAA        (1 << 5)   // Interrupt on Async Advance
#define EHCI_STS_HCHALTED   (1 << 12)  // HC Halted
#define EHCI_STS_RECLAMATION (1 << 13) // Reclamation
#define EHCI_STS_PSS        (1 << 14)  // Periodic Schedule Status
#define EHCI_STS_ASS        (1 << 15)  // Async Schedule Status

// Port Status and Control Register Bits
#define EHCI_PORT_CCS       (1 << 0)   // Current Connect Status
#define EHCI_PORT_CSC       (1 << 1)   // Connect Status Change
#define EHCI_PORT_PE        (1 << 2)   // Port Enabled
#define EHCI_PORT_PEC       (1 << 3)   // Port Enable Change
#define EHCI_PORT_OCA       (1 << 4)   // Over-current Active
#define EHCI_PORT_OCC       (1 << 5)   // Over-current Change
#define EHCI_PORT_FPR       (1 << 6)   // Force Port Resume
#define EHCI_PORT_SUSPEND   (1 << 7)   // Suspend
#define EHCI_PORT_RESET     (1 << 8)   // Port Reset
#define EHCI_PORT_LS_MASK   (3 << 10)  // Line Status
#define EHCI_PORT_PP        (1 << 12)  // Port Power
#define EHCI_PORT_OWNER     (1 << 13)  // Port Owner (companion controller)
#define EHCI_PORT_IC_MASK   (3 << 14)  // Port Indicator Control

// Queue Head (QH) Link Pointer Bits
#define EHCI_QH_TYPE_ITD    0x00
#define EHCI_QH_TYPE_QH     0x02
#define EHCI_QH_TYPE_SITD   0x04
#define EHCI_QH_TYPE_FSTN   0x06
#define EHCI_QH_TERMINATE   0x01

// Queue Transfer Descriptor (qTD) Token Bits
#define EHCI_QTD_STATUS_ACTIVE      (1 << 7)
#define EHCI_QTD_STATUS_HALTED      (1 << 6)
#define EHCI_QTD_STATUS_DBERR       (1 << 5)
#define EHCI_QTD_STATUS_BABBLE      (1 << 4)
#define EHCI_QTD_STATUS_XACTERR     (1 << 3)
#define EHCI_QTD_STATUS_MISSED_uF   (1 << 2)
#define EHCI_QTD_STATUS_SPLIT_STATE (1 << 1)
#define EHCI_QTD_STATUS_PING_STATE  (1 << 0)

#define EHCI_QTD_PID_OUT    0x00
#define EHCI_QTD_PID_IN     0x01
#define EHCI_QTD_PID_SETUP  0x02

#define EHCI_QTD_IOC        (1 << 15)  // Interrupt on Complete
#define EHCI_QTD_CERR_MASK  (3 << 10)  // Error Counter

// Frame List Size
#define EHCI_FRAMELIST_SIZE 1024

// Maximum QH and qTD pools
#define EHCI_MAX_QH         32
#define EHCI_MAX_QTD        128

// Queue Transfer Descriptor (qTD)
typedef struct ehci_qtd {
    uint32_t next_qtd_ptr;
    uint32_t alt_next_qtd_ptr;
    uint32_t token;
    uint32_t buffer_ptr[5];
    
    // Software fields (not accessed by hardware)
    uint32_t reserved[7];
} __attribute__((packed, aligned(32))) ehci_qtd_t;

// Queue Head (QH)
typedef struct ehci_qh {
    uint32_t qh_link_ptr;
    uint32_t ep_characteristics;
    uint32_t ep_capabilities;
    uint32_t current_qtd_ptr;
    
    // Overlay area (matches qTD structure)
    uint32_t next_qtd_ptr;
    uint32_t alt_next_qtd_ptr;
    uint32_t token;
    uint32_t buffer_ptr[5];
    
    // Software fields
    uint32_t reserved[4];
} __attribute__((packed, aligned(32))) ehci_qh_t;

// EHCI Controller Data
typedef struct {
    uint8_t* cap_regs;          // Capability registers (virtual)
    uint8_t* op_regs;           // Operational registers (virtual)
    uint32_t cap_regs_phys;     // Capability registers (physical)
    uint32_t op_regs_phys;      // Operational registers (physical)
    
    uint8_t num_ports;
    uint8_t cap_length;
    
    // Frame list (periodic schedule)
    dma_buffer_t* framelist_buffer;
    uint32_t* framelist;
    
    // Async schedule
    dma_buffer_t* async_qh_buffer;
    ehci_qh_t* async_qh;
    
    // QH and qTD pools
    dma_buffer_t* qh_pool_buffer;
    ehci_qh_t* qh_pool;
    bool qh_used[EHCI_MAX_QH];
    
    dma_buffer_t* qtd_pool_buffer;
    ehci_qtd_t* qtd_pool;
    bool qtd_used[EHCI_MAX_QTD];
} ehci_data_t;

// Function prototypes
bool ehci_init(usb_controller_t* controller);
void ehci_cleanup(usb_controller_t* controller);
bool ehci_detect_ports(usb_controller_t* controller);
bool ehci_reset_port(usb_controller_t* controller, uint8_t port);

// Transfer functions
bool ehci_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                           void* data, uint16_t length);
bool ehci_bulk_transfer(usb_device_t* device, uint8_t endpoint,
                        void* data, uint32_t length, bool is_in);

// Internal functions
ehci_qh_t* ehci_alloc_qh(ehci_data_t* ehci);
void ehci_free_qh(ehci_data_t* ehci, ehci_qh_t* qh);
ehci_qtd_t* ehci_alloc_qtd(ehci_data_t* ehci);
void ehci_free_qtd(ehci_data_t* ehci, ehci_qtd_t* qtd);
bool ehci_wait_for_qtd(ehci_qtd_t* qtd, uint32_t timeout_ms);

#endif // USB_EHCI_H