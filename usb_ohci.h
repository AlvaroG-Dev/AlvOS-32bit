#ifndef USB_OHCI_H
#define USB_OHCI_H

#include "usb_core.h"
#include <stdbool.h>
#include <stdint.h>

// OHCI Registers
#define OHCI_REG_REVISION 0x00
#define OHCI_REG_CONTROL 0x04
#define OHCI_REG_CMDSTATUS 0x08
#define OHCI_REG_INTSTATUS 0x0C
#define OHCI_REG_INTENABLE 0x10
#define OHCI_REG_INTDISABLE 0x14
#define OHCI_REG_HCCA 0x18
#define OHCI_REG_PERIODCURRENT 0x1C
#define OHCI_REG_CONTROLHEAD 0x20
#define OHCI_REG_CONTROLCURRENT 0x24
#define OHCI_REG_BULKHEAD 0x28
#define OHCI_REG_BULKCURRENT 0x2C
#define OHCI_REG_DONEHEAD 0x30
#define OHCI_REG_FMINTERVAL 0x34
#define OHCI_REG_FMREMAINING 0x38
#define OHCI_REG_FMNUMBER 0x3C
#define OHCI_REG_PERIODSTART 0x40
#define OHCI_REG_LSINFO 0x44
#define OHCI_REG_RHDESCRIPTORA 0x48
#define OHCI_REG_RHDESCRIPTORB 0x4C
#define OHCI_REG_RHSTATUS 0x50
#define OHCI_REG_RHPORTSTATUS 0x54 // + index * 4

// OHCI Control Register bits
#define OHCI_CTRL_CBSR 0x00000003 // Control/Bulk Service Ratio
#define OHCI_CTRL_PLE 0x00000004  // Periodic List Enable
#define OHCI_CTRL_IE 0x00000008   // Isochronous Enable
#define OHCI_CTRL_CLE 0x00000010  // Control List Enable
#define OHCI_CTRL_BLE 0x00000020  // Bulk List Enable
#define OHCI_CTRL_HCFS 0x000000C0 // Host Controller Functional State
#define OHCI_CTRL_HCFS_RESET 0x00000000
#define OHCI_CTRL_HCFS_RESUME 0x00000040
#define OHCI_CTRL_HCFS_OPERATIONAL 0x00000080
#define OHCI_CTRL_HCFS_SUSPEND 0x000000C0
#define OHCI_CTRL_IR 0x00000100  // Interrupt Routing
#define OHCI_CTRL_RWC 0x00000200 // Remote Wakeup Connected
#define OHCI_CTRL_RWE 0x00000400 // Remote Wakeup Enable

// OHCI Endpoint Descriptor
typedef struct {
  uint32_t info;    // FA, EN, D, S, K, F, MPS
  uint32_t tail_p;  // TD Queue Tail Pointer
  uint32_t head_p;  // TD Queue Head Pointer (toggle carrying)
  uint32_t next_ed; // Next ED
} __attribute__((packed, aligned(16))) ohci_ed_t;

// OHCI Transfer Descriptor
typedef struct {
  uint32_t info;    // CC, EC, T, DI, DP, R
  uint32_t cbp;     // Current Buffer Pointer
  uint32_t next_td; // Next TD
  uint32_t be;      // Buffer End
} __attribute__((packed, aligned(16))) ohci_td_t;

// OHCI HCCA (Host Controller Communication Area)
typedef struct {
  uint32_t interrupt_table[32]; // Interrupt table pointers
  uint16_t frame_number;
  uint16_t pad1;
  uint32_t done_head;
  uint8_t reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

// Driver Data
typedef struct {
  uint32_t mmio_base;

  // DMA Buffers (metadata)
  void *hcca_buffer;
  ohci_hcca_t *hcca;

  void *ed_pool_buffer;
  ohci_ed_t *ed_pool;
  bool ed_used[64];

  void *td_pool_buffer;
  ohci_td_t *td_pool;
  bool td_used[128];

  // Heads
  ohci_ed_t *control_head_ed;
  ohci_ed_t *bulk_head_ed;

} ohci_data_t;

// Functions
bool ohci_init(usb_controller_t *controller);
void ohci_cleanup(usb_controller_t *controller);
bool ohci_detect_ports(usb_controller_t *controller);
bool ohci_control_transfer(usb_device_t *device, usb_setup_packet_t *setup,
                           void *data, uint16_t length);
bool ohci_bulk_transfer(usb_device_t *device, uint8_t endpoint, void *data,
                        uint32_t length, bool is_in);

#endif // USB_OHCI_H
