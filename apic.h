#ifndef APIC_H
#define APIC_H

#include <stdbool.h>
#include <stdint.h>

// ========================================================================
// ESTRUCTURAS MADT (Multiple APIC Description Table)
// ========================================================================

// Entry types en MADT
#define MADT_TYPE_LOCAL_APIC 0
#define MADT_TYPE_IO_APIC 1
#define MADT_TYPE_INTERRUPT_OVERRIDE 2
#define MADT_TYPE_NMI 4
#define MADT_TYPE_LOCAL_APIC_OVERRIDE 5

// Flags de MADT
#define MADT_FLAG_PCAT_COMPAT (1 << 0) // Sistema tiene PIC legacy

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint8_t length;
} madt_entry_header_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t acpi_processor_id;
  uint8_t apic_id;
  uint32_t flags; // Bit 0: Processor Enabled, Bit 1: Online Capable
} madt_local_apic_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t io_apic_id;
  uint8_t reserved;
  uint32_t io_apic_address;
  uint32_t global_system_interrupt_base;
} madt_io_apic_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t bus_source;               // 0 = ISA
  uint8_t irq_source;               // IRQ en el bus source
  uint32_t global_system_interrupt; // GSI que debe usar
  uint16_t flags;                   // Polarity y trigger mode
} madt_interrupt_override_t;

// ========================================================================
// LOCAL APIC
// ========================================================================

// Registros del Local APIC (offsets desde base address)
#define LAPIC_ID 0x020          // Local APIC ID
#define LAPIC_VERSION 0x030     // Local APIC Version
#define LAPIC_TPR 0x080         // Task Priority Register
#define LAPIC_APR 0x090         // Arbitration Priority Register
#define LAPIC_PPR 0x0A0         // Processor Priority Register
#define LAPIC_EOI 0x0B0         // End Of Interrupt
#define LAPIC_RRD 0x0C0         // Remote Read Register
#define LAPIC_LDR 0x0D0         // Logical Destination Register
#define LAPIC_DFR 0x0E0         // Destination Format Register
#define LAPIC_SVR 0x0F0         // Spurious Interrupt Vector Register
#define LAPIC_ISR 0x100         // In-Service Register (0x100-0x170)
#define LAPIC_TMR 0x180         // Trigger Mode Register (0x180-0x1F0)
#define LAPIC_IRR 0x200         // Interrupt Request Register (0x200-0x270)
#define LAPIC_ESR 0x280         // Error Status Register
#define LAPIC_CMCI 0x2F0        // LVT Corrected Machine Check Interrupt
#define LAPIC_ICR_LOW 0x300     // Interrupt Command Register (bits 0-31)
#define LAPIC_ICR_HIGH 0x310    // Interrupt Command Register (bits 32-63)
#define LAPIC_LVT_TIMER 0x320   // LVT Timer Register
#define LAPIC_LVT_THERMAL 0x330 // LVT Thermal Sensor Register
#define LAPIC_LVT_PMC 0x340     // LVT Performance Monitoring Counter
#define LAPIC_LVT_LINT0 0x350   // LVT LINT0 Register
#define LAPIC_LVT_LINT1 0x360   // LVT LINT1 Register
#define LAPIC_LVT_ERROR 0x370   // LVT Error Register
#define LAPIC_TIMER_ICR 0x380   // Timer Initial Count Register
#define LAPIC_TIMER_CCR 0x390   // Timer Current Count Register
#define LAPIC_TIMER_DCR 0x3E0   // Timer Divide Configuration Register

// Flags para Spurious Interrupt Vector Register
#define LAPIC_SVR_ENABLE (1 << 8)
#define LAPIC_SPURIOUS_VECTOR 0xFF

// Flags para LVT
#define LAPIC_LVT_MASKED (1 << 16)
#define LAPIC_LVT_TIMER_PERIODIC (1 << 17)
#define LAPIC_LVT_TIMER_TSC_DEADLINE (1 << 18)

// Timer divide values
#define LAPIC_TIMER_DIV_1 0x0B
#define LAPIC_TIMER_DIV_2 0x00
#define LAPIC_TIMER_DIV_4 0x01
#define LAPIC_TIMER_DIV_8 0x02
#define LAPIC_TIMER_DIV_16 0x03
#define LAPIC_TIMER_DIV_32 0x08
#define LAPIC_TIMER_DIV_64 0x09
#define LAPIC_TIMER_DIV_128 0x0A

// Delivery Mode para ICR
#define LAPIC_DELIVERY_FIXED 0x0
#define LAPIC_DELIVERY_LOWEST 0x1
#define LAPIC_DELIVERY_SMI 0x2
#define LAPIC_DELIVERY_NMI 0x4
#define LAPIC_DELIVERY_INIT 0x5
#define LAPIC_DELIVERY_STARTUP 0x6

// ========================================================================
// I/O APIC
// ========================================================================

// Registros del I/O APIC
#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_ARB 0x02
#define IOAPIC_REG_REDTBL_BASE 0x10

// Offsets dentro de la estructura MMIO del I/O APIC
#define IOAPIC_REGSEL 0x00
#define IOAPIC_REGWIN 0x10

// Flags para entradas de redirección del I/O APIC
#define IOAPIC_DELIVERY_FIXED 0x000
#define IOAPIC_DELIVERY_LOWEST 0x100
#define IOAPIC_DELIVERY_SMI 0x200
#define IOAPIC_DELIVERY_NMI 0x400
#define IOAPIC_DELIVERY_INIT 0x500
#define IOAPIC_DELIVERY_EXTINT 0x700

#define IOAPIC_DEST_PHYSICAL 0x000
#define IOAPIC_DEST_LOGICAL 0x800

#define IOAPIC_POLARITY_HIGH 0x0000
#define IOAPIC_POLARITY_LOW 0x2000

#define IOAPIC_TRIGGER_EDGE 0x0000
#define IOAPIC_TRIGGER_LEVEL 0x8000

#define IOAPIC_MASKED 0x10000

// ========================================================================
// MSR (Model Specific Registers) para APIC
// ========================================================================

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_BSP (1 << 8)     // Bootstrap Processor
#define IA32_APIC_BASE_ENABLE (1 << 11) // APIC Enable
#define IA32_APIC_BASE_X2APIC (1 << 10) // x2APIC mode

// ========================================================================
// ESTRUCTURAS DE DATOS
// ========================================================================

typedef struct {
  uint8_t io_apic_id;
  uint32_t base_address;
  uint32_t virtual_address;
  uint32_t gsi_base;
  uint8_t max_redirect_entries;
} io_apic_info_t;

typedef struct {
  uint8_t processor_id;
  uint8_t apic_id;
  bool enabled;
  bool online_capable;
} local_apic_info_t;

typedef struct {
  uint8_t irq_source;
  uint32_t gsi;
  uint16_t flags;
  bool active_low;
  bool level_triggered;
} interrupt_override_t;

typedef struct {
  // Local APIC
  uint32_t lapic_base_phys;
  uint32_t lapic_base_virt;
  bool lapic_enabled;
  uint8_t lapic_id;
  uint32_t lapic_version;

  // I/O APIC(s)
  io_apic_info_t io_apics[8];
  uint8_t io_apic_count;

  // Local APICs (para multi-core en el futuro)
  local_apic_info_t local_apics[256];
  uint8_t local_apic_count;

  // Interrupt overrides
  interrupt_override_t overrides[24];
  uint8_t override_count;

  // Estado
  bool initialized;
  bool using_apic; // true si usamos APIC, false si PIC
  bool pic_disabled;

  // Timer
  uint32_t timer_frequency;
  uint32_t timer_ticks_per_ms;
} apic_info_t;

// Variable global
extern apic_info_t apic_info;

// ========================================================================
// FUNCIONES PRINCIPALES
// ========================================================================

// Inicialización
bool apic_init(void);
bool apic_check_support(void);
void apic_enable(void);
void apic_disable(void);
bool apic_verify_state(void);

// Local APIC
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_eoi(void);
uint8_t lapic_get_id(void);

// Timer del Local APIC
void lapic_timer_init(uint32_t frequency_hz);
void lapic_timer_oneshot(uint32_t initial_count);
void lapic_timer_periodic(uint32_t initial_count);
void lapic_timer_stop(void);
uint32_t lapic_timer_calibrate(void);

// I/O APIC
void ioapic_write(uint8_t io_apic_index, uint8_t reg, uint32_t value);
uint32_t ioapic_read(uint8_t io_apic_index, uint8_t reg);
void ioapic_set_irq(uint8_t irq, uint8_t vector, bool masked);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

// Mapeo de IRQ a GSI (Global System Interrupt)
uint32_t apic_irq_to_gsi(uint8_t irq);
interrupt_override_t *apic_get_override(uint8_t irq);
void irq_setup_apic(void);

// Utilidades
void apic_print_info(void);
bool apic_is_enabled(void);
void apic_disable_pic(void);

// MSR helpers (inline)
static inline void wrmsr(uint32_t msr, uint64_t value) {
  uint32_t low = value & 0xFFFFFFFF;
  uint32_t high = value >> 32;
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

#endif // APIC