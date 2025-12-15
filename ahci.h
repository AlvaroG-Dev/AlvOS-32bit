#ifndef AHCI_H
#define AHCI_H

#include "dma.h"
#include "pci.h"
#include <stdbool.h>
#include <stdint.h>

// AHCI PCI Class/Subclass
#define AHCI_PCI_CLASS 0x01    // Mass Storage
#define AHCI_PCI_SUBCLASS 0x06 // SATA
#define AHCI_PCI_PROG_IF 0x01  // AHCI

// AHCI Generic Host Control Registers
#define AHCI_HBA_CAP 0x00       // Host Capabilities
#define AHCI_HBA_GHC 0x04       // Global Host Control
#define AHCI_HBA_IS 0x08        // Interrupt Status
#define AHCI_HBA_PI 0x0C        // Port Implemented
#define AHCI_HBA_VS 0x10        // Version
#define AHCI_HBA_CCC_CTL 0x14   // Command Completion Coalescing Control
#define AHCI_HBA_CCC_PORTS 0x18 // Command Completion Coalescing Ports
#define AHCI_HBA_EM_LOC 0x1C    // Enclosure Management Location
#define AHCI_HBA_EM_CTL 0x20    // Enclosure Management Control
#define AHCI_HBA_CAP2 0x24      // Extended Host Capabilities
#define AHCI_HBA_BOHC 0x28      // BIOS/OS Handoff Control and Status

// AHCI Port Registers (each port has 0x80 bytes, starting at 0x100)
#define AHCI_PORT_BASE 0x100
#define AHCI_PORT_SIZE 0x80
#define AHCI_PORT_CLB 0x00  // Command List Base Address
#define AHCI_PORT_CLBU 0x04 // Command List Base Address Upper 32-bits
#define AHCI_PORT_FB 0x08   // FIS Base Address
#define AHCI_PORT_FBU 0x0C  // FIS Base Address Upper 32-bits
#define AHCI_PORT_IS 0x10   // Interrupt Status
#define AHCI_PORT_IE 0x14   // Interrupt Enable
#define AHCI_PORT_CMD 0x18  // Command and Status
#define AHCI_PORT_TFD 0x20  // Task File Data
#define AHCI_PORT_SIG 0x24  // Signature
#define AHCI_PORT_SSTS 0x28 // SATA Status (SCR0: SStatus)
#define AHCI_PORT_SCTL 0x2C // SATA Control (SCR2: SControl)
#define AHCI_PORT_SERR 0x30 // SATA Error (SCR1: SError)
#define AHCI_PORT_SACT 0x34 // SATA Active (SCR3: SActive)
#define AHCI_PORT_CI 0x38   // Command Issue

// Host Capabilities (CAP) register bits
#define AHCI_CAP_NP_MASK 0x1F  // Number of ports
#define AHCI_CAP_SXS (1 << 5)  // External SATA
#define AHCI_CAP_EMS (1 << 6)  // Enclosure Management
#define AHCI_CAP_CCCS (1 << 7) // Command Completion Coalescing
#define AHCI_CAP_NCS_SHIFT 8   // Number of Command Slots
#define AHCI_CAP_NCS_MASK 0x1F
#define AHCI_CAP_PSC (1 << 13)  // Partial State Capable
#define AHCI_CAP_SSC (1 << 14)  // Slumber State Capable
#define AHCI_CAP_PMD (1 << 15)  // PIO Multiple DRQ Block
#define AHCI_CAP_FBSS (1 << 16) // FIS-based Switching
#define AHCI_CAP_SPM (1 << 17)  // Port Multiplier
#define AHCI_CAP_SAM (1 << 18)  // AHCI mode only
#define AHCI_CAP_SNZO (1 << 19) // Non-Zero DMA Offsets
#define AHCI_CAP_ISS_SHIFT 20   // Interface Speed Support
#define AHCI_CAP_ISS_MASK 0xF
#define AHCI_CAP_SCLO (1 << 24)  // Command List Override
#define AHCI_CAP_SAL (1 << 25)   // Activity LED
#define AHCI_CAP_SALP (1 << 26)  // Aggressive Link Power Management
#define AHCI_CAP_SSS (1 << 27)   // Staggered Spin-up
#define AHCI_CAP_SMPS (1 << 28)  // Mechanical Presence Switch
#define AHCI_CAP_SSNTF (1 << 29) // SNotification Register
#define AHCI_CAP_SNCQ (1 << 30)  // Native Command Queuing
#define AHCI_CAP_S64A (1 << 31)  // 64-bit Addressing

// Global Host Control (GHC) register bits
#define AHCI_GHC_HR (1 << 0)   // HBA Reset
#define AHCI_GHC_IE (1 << 1)   // Interrupt Enable
#define AHCI_GHC_MRSM (1 << 2) // MSI Revert to Single Message
#define AHCI_GHC_AE (1 << 31)  // AHCI Enable

// Port Command (PxCMD) register bits
#define AHCI_PORT_CMD_ST (1 << 0)  // Start
#define AHCI_PORT_CMD_SUD (1 << 1) // Spin-Up Device
#define AHCI_PORT_CMD_POD (1 << 2) // Power On Device
#define AHCI_PORT_CMD_CLO (1 << 3) // Command List Override
#define AHCI_PORT_CMD_FRE (1 << 4) // FIS Receive Enable
#define AHCI_PORT_CMD_CCS_SHIFT 8  // Current Command Slot
#define AHCI_PORT_CMD_CCS_MASK 0x1F
#define AHCI_PORT_CMD_MPSS (1 << 13)  // Mechanical Presence Switch State
#define AHCI_PORT_CMD_FR (1 << 14)    // FIS Receive Running
#define AHCI_PORT_CMD_CR (1 << 15)    // Command List Running
#define AHCI_PORT_CMD_CPS (1 << 16)   // Cold Presence State
#define AHCI_PORT_CMD_PMA (1 << 17)   // Port Multiplier Attached
#define AHCI_PORT_CMD_HPCP (1 << 18)  // Hot Plug Capable Port
#define AHCI_PORT_CMD_MPSP (1 << 19)  // Mechanical Presence Switch
#define AHCI_PORT_CMD_CPD (1 << 20)   // Cold Presence Detection
#define AHCI_PORT_CMD_ESP (1 << 21)   // External SATA Port
#define AHCI_PORT_CMD_FBSCP (1 << 22) // FIS-based Switching Capable
#define AHCI_PORT_CMD_APSTE (1 << 23) // Automatic Partial to Slumber
#define AHCI_PORT_CMD_ATAPI (1 << 24) // Device is ATAPI
#define AHCI_PORT_CMD_DLAE (1 << 25)  // Drive LED on ATAPI Enable
#define AHCI_PORT_CMD_ALPE (1 << 26)  // Aggressive Link Power Management Enable
#define AHCI_PORT_CMD_ASP (1 << 27)   // Aggressive Slumber/Partial
#define AHCI_PORT_CMD_ICC_SHIFT 28    // Interface Communication Control
#define AHCI_PORT_CMD_ICC_MASK 0xF
#define AHCI_PORT_CMD_ICC_ACTIVE 0x1 // Interface Communication Control: Active

// Port Interrupt Status (PxIS) register bits
#define AHCI_PORT_IS_DHRS (1 << 0)  // Device to Host Register FIS Interrupt
#define AHCI_PORT_IS_PSS (1 << 1)   // PIO Setup FIS Interrupt
#define AHCI_PORT_IS_DSS (1 << 2)   // DMA Setup FIS Interrupt
#define AHCI_PORT_IS_SDBS (1 << 3)  // Set Device Bits Interrupt
#define AHCI_PORT_IS_UFS (1 << 4)   // Unknown FIS Interrupt
#define AHCI_PORT_IS_DPS (1 << 5)   // Descriptor Processed
#define AHCI_PORT_IS_PCS (1 << 6)   // Port Connect Change Status
#define AHCI_PORT_IS_DMPS (1 << 7)  // Device Mechanical Presence Status
#define AHCI_PORT_IS_PRCS (1 << 22) // PhyRdy Change Status
#define AHCI_PORT_IS_IPMS (1 << 23) // Incorrect Port Multiplier Status
#define AHCI_PORT_IS_OFS (1 << 24)  // Overflow Status
#define AHCI_PORT_IS_INFS (1 << 26) // Interface Non-fatal Error Status
#define AHCI_PORT_IS_IFS (1 << 27)  // Interface Fatal Error Status
#define AHCI_PORT_IS_HBDS (1 << 28) // Host Bus Data Error Status
#define AHCI_PORT_IS_HBFS (1 << 29) // Host Bus Fatal Error Status
#define AHCI_PORT_IS_TFES (1 << 30) // Task File Error Status
#define AHCI_PORT_IS_CPDS (1 << 31) // Cold Port Detect Status

// SATA Status (PxSSTS) register bits
#define AHCI_PORT_SSTS_DET_MASK 0x0000000F // Device Detection
#define AHCI_PORT_SSTS_SPD_SHIFT 4         // Current Interface Speed
#define AHCI_PORT_SSTS_SPD_MASK 0xF
#define AHCI_PORT_SSTS_IPM_SHIFT 8 // Interface Power Management
#define AHCI_PORT_SSTS_IPM_MASK 0xF
#define AHCI_PORT_SSTS_DET_PRESENT 0x00000003
#define AHCI_PORT_SSTS_IPM_ACTIVE 0x00000001

// ATA Status Register bits
#define ATA_SR_BSY (1 << 7)
#define ATA_SR_DRQ (1 << 3)
#define ATA_SR_ERR (1 << 0)
#define ATA_SR_DF (1 << 5)

// Device Detection values
#define AHCI_PORT_DET_NONE 0x0        // No device detected
#define AHCI_PORT_DET_PRESENT 0x1     // Device present but not established
#define AHCI_PORT_DET_ESTABLISHED 0x3 // Device present and established

// Interface Speed values
#define AHCI_PORT_SPD_NONE 0x0
#define AHCI_PORT_SPD_GEN1 0x1 // 1.5 Gbps
#define AHCI_PORT_SPD_GEN2 0x2 // 3.0 Gbps
#define AHCI_PORT_SPD_GEN3 0x3 // 6.0 Gbps

// SATA signatures
#define AHCI_SIG_ATA 0x00000101   // SATA drive
#define AHCI_SIG_ATAPI 0xEB140101 // ATAPI drive
#define AHCI_SIG_SEMB 0xC33C0101  // Enclosure management bridge
#define AHCI_SIG_PM 0x96690101    // Port multiplier

// FIS Types
#define FIS_TYPE_REG_H2D 0x27   // Register FIS - host to device
#define FIS_TYPE_REG_D2H 0x34   // Register FIS - device to host
#define FIS_TYPE_DMA_ACT 0x39   // DMA activate FIS
#define FIS_TYPE_DMA_SETUP 0x41 // DMA setup FIS
#define FIS_TYPE_DATA 0x46      // Data FIS
#define FIS_TYPE_BIST 0x58      // BIST activate FIS
#define FIS_TYPE_PIO_SETUP 0x5F // PIO setup FIS
#define FIS_TYPE_DEV_BITS 0xA1  // Set device bits FIS

// ATA Commands
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

// Command slot and FIS sizes
#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMDS 32
#define AHCI_CMD_SLOT_SIZE 32
#define AHCI_RX_FIS_SIZE 256
#define AHCI_CMD_TBL_SIZE 0x80

#define AHCI_PORT_IE_MASK                                                      \
  (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | AHCI_PORT_IS_DSS |                   \
   AHCI_PORT_IS_SDBS | AHCI_PORT_IS_UFS | AHCI_PORT_IS_TFES |                  \
   AHCI_PORT_IS_PCS | AHCI_PORT_IS_PRCS)

// Structure definitions
typedef struct __attribute__((packed)) {
  // DWORD 0
  uint8_t fis_type;
  uint8_t pmport : 4;
  uint8_t rsv0 : 3;
  uint8_t c : 1;
  uint8_t command;
  uint8_t featurel;

  // DWORD 1
  uint8_t lba0;
  uint8_t lba1;
  uint8_t lba2;
  uint8_t device;

  // DWORD 2
  uint8_t lba3;
  uint8_t lba4;
  uint8_t lba5;
  uint8_t featureh;

  // DWORD 3
  uint8_t countl;
  uint8_t counth;
  uint8_t icc;
  uint8_t control;

  // DWORD 4
  uint8_t rsv1[4];
} fis_reg_h2d_t;

typedef struct __attribute__((packed)) {
  // DWORD 0
  uint8_t fis_type;
  uint8_t pmport : 4;
  uint8_t rsv0 : 2;
  uint8_t i : 1;
  uint8_t rsv1 : 1;
  uint8_t status;
  uint8_t error;

  // DWORD 1
  uint8_t lba0;
  uint8_t lba1;
  uint8_t lba2;
  uint8_t device;

  // DWORD 2
  uint8_t lba3;
  uint8_t lba4;
  uint8_t lba5;
  uint8_t rsv2;

  // DWORD 3
  uint8_t countl;
  uint8_t counth;
  uint8_t rsv3[2];

  // DWORD 4
  uint8_t rsv4[4];
} fis_reg_d2h_t;

typedef struct __attribute__((packed)) {
  // DWORD 0
  uint32_t cfl : 5;    // Command FIS length in DWORDS, 2 ~ 16
  uint32_t a : 1;      // ATAPI
  uint32_t w : 1;      // Write, 1: H2D, 0: D2H
  uint32_t p : 1;      // Prefetchable
  uint32_t r : 1;      // Reset
  uint32_t b : 1;      // BIST
  uint32_t c : 1;      // Clear busy upon R_OK
  uint32_t rsv0 : 1;   // Reserved
  uint32_t pmp : 4;    // Port multiplier port
  uint32_t prdtl : 16; // Physical region descriptor table length

  // DWORD 1
  volatile uint32_t prdbc; // Physical region descriptor byte count

  // DWORD 2, 3
  uint32_t ctba;  // Command table descriptor base address
  uint32_t ctbau; // Command table descriptor base address upper 32 bits

  // DWORD 4 - 7
  uint32_t rsv1[4]; // Reserved
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
  uint32_t dba;      // Data base address
  uint32_t dbau;     // Data base address upper 32 bits
  uint32_t rsv0;     // Reserved
  uint32_t dbc : 22; // Byte count, 4M max
  uint32_t rsv1 : 9; // Reserved
  uint32_t i : 1;    // Interrupt on completion
} hba_prdt_entry_t;

typedef struct __attribute__((packed)) {
  uint8_t cfis[64];               // Command FIS
  uint8_t acmd[16];               // ATAPI command, 12 or 16 bytes
  uint8_t rsv[48];                // Reserved
  hba_prdt_entry_t prdt_entry[1]; // Physical region descriptor table entries
} hba_cmd_tbl_t;

typedef struct __attribute__((packed)) {
  // 0x00 - 0x2B, Generic Host Control
  volatile uint32_t cap;     // 0x00, Host capability
  volatile uint32_t ghc;     // 0x04, Global host control
  volatile uint32_t is;      // 0x08, Interrupt status
  volatile uint32_t pi;      // 0x0C, Port implemented
  volatile uint32_t vs;      // 0x10, Version
  volatile uint32_t ccc_ctl; // 0x14, Command completion coalescing control
  volatile uint32_t ccc_pts; // 0x18, Command completion coalescing ports
  volatile uint32_t em_loc;  // 0x1C, Enclosure management location
  volatile uint32_t em_ctl;  // 0x20, Enclosure management control
  volatile uint32_t cap2;    // 0x24, Host capabilities extended
  volatile uint32_t bohc;    // 0x28, BIOS/OS handoff control and status

  // 0x2C - 0x9F, Reserved
  uint8_t rsv[0xA0 - 0x2C];

  // 0xA0 - 0xFF, Vendor specific registers
  uint8_t vendor[0x100 - 0xA0];
} hba_mem_t;

typedef struct __attribute__((packed)) {
  volatile uint32_t clb;  // 0x00, command list base address, 1K-byte aligned
  volatile uint32_t clbu; // 0x04, command list base address upper 32 bits
  volatile uint32_t fb;   // 0x08, FIS base address, 256-byte aligned
  volatile uint32_t fbu;  // 0x0C, FIS base address upper 32 bits
  volatile uint32_t is;   // 0x10, interrupt status
  volatile uint32_t ie;   // 0x14, interrupt enable
  volatile uint32_t cmd;  // 0x18, command and status
  volatile uint32_t rsv0; // 0x1C, Reserved
  volatile uint32_t tfd;  // 0x20, task file data
  volatile uint32_t sig;  // 0x24, signature
  volatile uint32_t ssts; // 0x28, SATA status (SCR0:SStatus)
  volatile uint32_t sctl; // 0x2C, SATA control (SCR2:SControl)
  volatile uint32_t serr; // 0x30, SATA error (SCR1:SError)
  volatile uint32_t sact; // 0x34, SATA active (SCR3:SActive)
  volatile uint32_t ci;   // 0x38, command issue
  volatile uint32_t sntf; // 0x3C, SATA notification (SCR4:SNotification)
  volatile uint32_t fbs;  // 0x40, FIS-based switch control
  volatile uint32_t rsv1[11];  // 0x44 ~ 0x6F, Reserved
  volatile uint32_t vendor[4]; // 0x70 ~ 0x7F, vendor specific
} hba_port_t;

// AHCI Port structure
typedef struct {
  uint8_t port_num;
  bool present;
  bool initialized;
  uint8_t device_type; // ATA, ATAPI, etc
  uint32_t signature;

  hba_port_t *port_regs;
  hba_cmd_header_t *cmd_list;
  uint8_t *fis_base;
  hba_cmd_tbl_t *cmd_tables[AHCI_MAX_CMDS];

  dma_buffer_t *cmd_list_buffer;
  dma_buffer_t *fis_buffer;
  dma_buffer_t *cmd_table_buffers[AHCI_MAX_CMDS];

  bool command_slots[AHCI_MAX_CMDS]; // Track used slots
} ahci_port_t;

// AHCI Controller structure
typedef struct {
  bool initialized;
  pci_device_t *pci_device;
  hba_mem_t *abar; // AHCI Base Address Register
  uint32_t abar_physical;

  uint32_t port_count;
  uint32_t command_slots;
  bool supports_64bit;
  bool supports_ncq;

  ahci_port_t ports[AHCI_MAX_PORTS];
  uint32_t ports_implemented;
} ahci_controller_t;

// Global AHCI controller
extern ahci_controller_t ahci_controller;

// Function prototypes
bool ahci_init(void);
void ahci_cleanup(void);
bool ahci_detect_controller(void);
bool ahci_initialize_controller(void);
bool ahci_initialize_port(uint8_t port_num);
bool ahci_spin_up_device(uint8_t port_num);
bool ahci_start_port(uint8_t port_num);
bool ahci_stop_port(uint8_t port_num);

// Command functions
int ahci_find_cmdslot(ahci_port_t *port);
bool ahci_send_command(uint8_t port_num, uint8_t slot, uint8_t *fis,
                       void *buffer, uint32_t buffer_size, bool write);
bool ahci_identify_device(uint8_t port_num, void *buffer);
bool ahci_identify_device_pio(uint8_t port_num, void *buffer);
bool ahci_read_sectors(uint8_t port_num, uint64_t lba, uint32_t count,
                       void *buffer);
bool ahci_write_sectors(uint8_t port_num, uint64_t lba, uint32_t count,
                        const void *buffer);

// Utility functions
void ahci_list_devices(void);
void ahci_print_port_status(uint8_t port_num);
const char *ahci_get_device_type_name(uint32_t signature);

// Forward declaration
struct regs;

// IRQ handler
void ahci_irq_handler(struct regs *r);

#endif // AHCI_H