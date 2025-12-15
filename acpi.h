#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ACPI Signatures
#define ACPI_RSDP_SIGNATURE     "RSD PTR "
#define ACPI_RSDT_SIGNATURE     "RSDT"
#define ACPI_XSDT_SIGNATURE     "XSDT"
#define ACPI_FADT_SIGNATURE     "FACP"
#define ACPI_DSDT_SIGNATURE     "DSDT"
#define ACPI_SSDT_SIGNATURE     "SSDT"
#define ACPI_MADT_SIGNATURE     "APIC"
#define ACPI_MCFG_SIGNATURE     "MCFG"
#define ACPI_HPET_SIGNATURE     "HPET"

// ACPI Power Management 1 Control Register bits
#define ACPI_PM1_CNT_SCI_EN     (1 << 0)   // SCI interrupt enable
#define ACPI_PM1_CNT_BM_RLD     (1 << 1)   // Bus master reload
#define ACPI_PM1_CNT_GBL_RLS    (1 << 2)   // Global release
#define ACPI_PM1_CNT_SLP_TYP    (7 << 10)  // Sleep type mask
#define ACPI_PM1_CNT_SLP_EN     (1 << 13)  // Sleep enable

// Sleep states
#define ACPI_S0_SLEEP_TYPE      0x00        // Working state
#define ACPI_S1_SLEEP_TYPE      0x01        // Sleep with CPU and RAM on
#define ACPI_S3_SLEEP_TYPE      0x05        // Suspend to RAM
#define ACPI_S4_SLEEP_TYPE      0x06        // Suspend to disk
#define ACPI_S5_SLEEP_TYPE      0x07        // Soft power off

// Maximum number of ACPI tables to handle
#define MAX_ACPI_TABLES 32

// Root System Description Pointer (RSDP)
typedef struct __attribute__((packed)) {
    char signature[8];          // "RSD PTR "
    uint8_t checksum;           // Checksum of first 20 bytes
    char oem_id[6];            // OEM ID
    uint8_t revision;          // ACPI revision (0 = v1.0, 2 = v2.0+)
    uint32_t rsdt_address;     // Physical address of RSDT
    
    // ACPI 2.0+ fields
    uint32_t length;           // Length of RSDP structure
    uint64_t xsdt_address;     // Physical address of XSDT (64-bit)
    uint8_t extended_checksum; // Checksum of entire structure
    uint8_t reserved[3];       // Reserved fields
} acpi_rsdp_t;

// System Description Table Header (common to all tables)
typedef struct __attribute__((packed)) {
    char signature[4];         // Table signature
    uint32_t length;          // Length of table including header
    uint8_t revision;         // Table revision
    uint8_t checksum;         // Checksum of entire table
    char oem_id[6];          // OEM ID
    char oem_table_id[8];    // OEM Table ID
    uint32_t oem_revision;   // OEM Revision
    uint32_t creator_id;     // Creator ID
    uint32_t creator_revision; // Creator revision
} acpi_sdt_header_t;

// Root System Description Table (RSDT) - ACPI 1.0
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t sdt_pointers[]; // Variable length array of SDT pointers
} acpi_rsdt_t;

// Extended System Description Table (XSDT) - ACPI 2.0+
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t sdt_pointers[]; // Variable length array of 64-bit SDT pointers
} acpi_xsdt_t;

typedef struct __attribute__((packed)) {
    uint8_t address_space_id;    // 0: System Memory, 1: System I/O, 2: PCI Config, etc.
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;         // 0: Undefined, 1: Byte, 2: Word, 3: DWord, 4: QWord
    uint64_t address;
} generic_address_t;

// Fixed ACPI Description Table (FADT)
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;    // Physical address of FACS
    uint32_t dsdt_address;     // Physical address of DSDT
    uint8_t reserved1;         // Reserved
    uint8_t preferred_pm_profile; // Power management profile
    uint16_t sci_interrupt;    // SCI interrupt number
    uint32_t smi_command_port; // SMI command port
    uint8_t acpi_enable;       // Value to write to enable ACPI
    uint8_t acpi_disable;      // Value to write to disable ACPI
    uint8_t s4bios_req;        // Value for S4BIOS request
    uint8_t pstate_control;    // P-state control
    uint32_t pm1a_event_block; // PM1a Event Register Block
    uint32_t pm1b_event_block; // PM1b Event Register Block
    uint32_t pm1a_control_block; // PM1a Control Register Block
    uint32_t pm1b_control_block; // PM1b Control Register Block
    uint32_t pm2_control_block;  // PM2 Control Register Block
    uint32_t pm_timer_block;     // PM Timer Control Register Block
    uint32_t gpe0_block;         // GPE0 Register Block
    uint32_t gpe1_block;         // GPE1 Register Block
    uint8_t pm1_event_length;    // Length of PM1 Event Register Block
    uint8_t pm1_control_length;  // Length of PM1 Control Register Block
    uint8_t pm2_control_length;  // Length of PM2 Control Register Block
    uint8_t pm_timer_length;     // Length of PM Timer Register Block
    uint8_t gpe0_length;         // Length of GPE0 Register Block
    uint8_t gpe1_length;         // Length of GPE1 Register Block
    uint8_t gpe1_base;           // GPE1 base offset
    uint8_t cst_control;         // C-state control
    uint16_t worst_c2_latency;   // Worst case C2 latency
    uint16_t worst_c3_latency;   // Worst case C3 latency
    uint16_t flush_size;         // Cache flush size
    uint16_t flush_stride;       // Cache flush stride
    uint8_t duty_offset;         // Duty cycle offset
    uint8_t duty_width;          // Duty cycle width
    uint8_t day_alarm;           // RTC day alarm index
    uint8_t month_alarm;         // RTC month alarm index
    uint8_t century;             // RTC century index
    uint16_t boot_arch_flags;    // IA-PC Boot Architecture Flags
    uint8_t reserved2;           // Reserved
    uint32_t flags;              // Fixed feature flags
    generic_address_t reset_register;
    uint8_t reset_value;
    uint8_t reserved3[3];
    // ACPI 2.0+ fields would follow, but we'll keep it simple for now
} acpi_fadt_t;

// ACPI Power Management structures
typedef struct {
    bool acpi_enabled;
    bool sci_enabled;
    uint16_t pm1a_control_port;
    uint16_t pm1b_control_port;
    uint16_t pm1a_status_port;
    uint16_t pm1b_status_port;
    uint16_t pm2_control_port;
    uint16_t smi_command_port;
    uint8_t acpi_enable_value;
    uint8_t acpi_disable_value;
    uint8_t s5_sleep_type_a;
    uint8_t s5_sleep_type_b;
    generic_address_t reset_reg;
    uint8_t reset_value;
} acpi_pm_info_t;

// ACPI system information
typedef struct {
    acpi_rsdp_t* rsdp;
    acpi_rsdt_t* rsdt;
    acpi_xsdt_t* xsdt;
    acpi_fadt_t* fadt;
    acpi_pm_info_t pm_info;
    uint32_t table_count;
    void* tables[MAX_ACPI_TABLES];
    bool initialized;
    uint8_t acpi_version; // 1 for ACPI 1.0, 2 for ACPI 2.0+
} acpi_info_t;

// Global ACPI information
extern acpi_info_t acpi_info;

// Main ACPI functions
void acpi_init(void);
acpi_rsdp_t* acpi_find_rsdp(void);
bool acpi_validate_checksum(void* table, size_t length);
void* acpi_find_table(const char* signature);
void acpi_parse_tables(void);

// Power management functions
bool acpi_enable(void);
bool acpi_disable(void);
void acpi_power_off(void);
void acpi_reboot(void);
void acpi_suspend(void);

// Utility functions
void acpi_list_tables(void);
const char* acpi_get_table_name(const char* signature);
bool acpi_is_supported(void);
uint8_t acpi_get_version(void);

// Helper functions for implementation
acpi_rsdp_t* acpi_search_rsdp_in_range(void* start, size_t length);
void acpi_parse_rsdt(void);
void acpi_parse_xsdt(void);

#endif // ACPI_H