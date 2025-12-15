#include "apic.h"
#include "acpi.h"
#include "mmu.h"
#include "io.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"
#include "irq.h"
#include "idt.h"

// Variable global
apic_info_t apic_info = {0};

// Forward declarations
static bool apic_parse_madt(void);
static void apic_disable_pic(void);
static uint32_t ioapic_get_base(uint8_t io_apic_index);

// ========================================================================
// INICIALIZACIÓN
// ========================================================================

bool apic_check_support(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // ✅ Guardar/restaurar todos los registros
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "cpuid\n\t"
        "movl %%ebx, %%esi\n\t"
        "popl %%ebx"
        : "=a"(eax), "=S"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    
    // Verificar bit 9 de EDX (APIC present)
    if (!(edx & (1 << 9))) {
        return false;
    }
    
    // ✅ Verificar que APIC esté habilitado en MSR
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);
    if (!(apic_base & IA32_APIC_BASE_ENABLE)) {
        terminal_puts(&main_terminal, "APIC: APIC present but disabled in MSR\r\n");
    }
    
    return true;
}

bool apic_init(void) {
    terminal_puts(&main_terminal, "Initializing APIC subsystem...\r\n");
    
    memset(&apic_info, 0, sizeof(apic_info));
    
    // 1. Verificar soporte CPUID
    if (!apic_check_support()) {
        terminal_puts(&main_terminal, "APIC: CPU does not support APIC\r\n");
        return false;
    }
    
    // 2. Verificar que ACPI esté inicializado
    if (!acpi_is_supported()) {
        terminal_puts(&main_terminal, "APIC: ACPI not available, cannot initialize APIC\r\n");
        return false;
    }
    
    // 3. Parsear tabla MADT
    if (!apic_parse_madt()) {
        terminal_puts(&main_terminal, "APIC: Failed to parse MADT table\r\n");
        return false;
    }
    
    // 4. Verificar que tengamos al menos un I/O APIC
    if (apic_info.io_apic_count == 0) {
        terminal_puts(&main_terminal, "APIC: No I/O APIC found\r\n");
        return false;
    }
    
    // 5. Mapear Local APIC
    if (!mmu_ensure_physical_accessible(apic_info.lapic_base_phys, PAGE_SIZE, 
                                       &apic_info.lapic_base_virt)) {
        terminal_puts(&main_terminal, "APIC: Failed to map Local APIC\r\n");
        return false;
    }
    
    terminal_printf(&main_terminal, "APIC: Local APIC at phys=0x%08x, virt=0x%08x\r\n",
                   apic_info.lapic_base_phys, apic_info.lapic_base_virt);
    
    // 6. Mapear I/O APICs
    for (uint8_t i = 0; i < apic_info.io_apic_count; i++) {
        uint32_t virt_addr;
        if (!mmu_ensure_physical_accessible(apic_info.io_apics[i].base_address, 
                                           PAGE_SIZE, &virt_addr)) {
            terminal_printf(&main_terminal, "APIC: Failed to map I/O APIC %u\r\n", i);
            return false;
        }
        apic_info.io_apics[i].virtual_address = virt_addr;
        
        // Leer versión y número máximo de entradas
        uint32_t version = ioapic_read(i, IOAPIC_REG_VERSION);
        apic_info.io_apics[i].max_redirect_entries = ((version >> 16) & 0xFF) + 1;
        
        terminal_printf(&main_terminal, "APIC: I/O APIC %u at phys=0x%08x, virt=0x%08x, "
                       "GSI base=%u, max entries=%u\r\n",
                       i, apic_info.io_apics[i].base_address,
                       apic_info.io_apics[i].virtual_address,
                       apic_info.io_apics[i].gsi_base,
                       apic_info.io_apics[i].max_redirect_entries);
    }
    
    // 7. Habilitar Local APIC
    apic_enable();
    
    // 8. Leer ID y versión del Local APIC
    apic_info.lapic_id = lapic_get_id();
    apic_info.lapic_version = lapic_read(LAPIC_VERSION);
    
    terminal_printf(&main_terminal, "APIC: Local APIC ID=%u, version=0x%08x\r\n",
                   apic_info.lapic_id, apic_info.lapic_version);
    
    // 9. Deshabilitar PIC
    apic_disable_pic();
    
    apic_info.initialized = true;
    apic_info.using_apic = true;
    
    terminal_puts(&main_terminal, "APIC: Initialization complete\r\n");
    return true;
}

static bool apic_parse_madt(void) {
    // Buscar tabla MADT en ACPI
    acpi_sdt_header_t* madt_header = (acpi_sdt_header_t*)acpi_find_table("APIC");
    if (!madt_header) {
        terminal_puts(&main_terminal, "APIC: MADT table not found\r\n");
        return false;
    }
    
    // Estructura completa de MADT
    struct {
        acpi_sdt_header_t header;
        uint32_t local_apic_address;
        uint32_t flags;
        uint8_t entries[];
    } __attribute__((packed)) *madt = (void*)madt_header;
    
    apic_info.lapic_base_phys = madt->local_apic_address;
    
    terminal_printf(&main_terminal, "APIC: MADT found, Local APIC at 0x%08x, flags=0x%08x\r\n",
                   madt->local_apic_address, madt->flags);
    
    // Parsear entradas
    uint8_t* entry_ptr = madt->entries;
    uint8_t* end_ptr = (uint8_t*)madt + madt->header.length;
    
    while (entry_ptr < end_ptr) {
        madt_entry_header_t* entry = (madt_entry_header_t*)entry_ptr;
        
        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
                if (apic_info.local_apic_count < 256) {
                    apic_info.local_apics[apic_info.local_apic_count].processor_id = 
                        lapic->acpi_processor_id;
                    apic_info.local_apics[apic_info.local_apic_count].apic_id = 
                        lapic->apic_id;
                    apic_info.local_apics[apic_info.local_apic_count].enabled = 
                        (lapic->flags & 1) != 0;
                    apic_info.local_apics[apic_info.local_apic_count].online_capable = 
                        (lapic->flags & 2) != 0;
                    apic_info.local_apic_count++;
                    
                    terminal_printf(&main_terminal, 
                        "APIC: Local APIC - Processor=%u, APIC ID=%u, Enabled=%u\r\n",
                        lapic->acpi_processor_id, lapic->apic_id, (lapic->flags & 1));
                }
                break;
            }
            
            case MADT_TYPE_IO_APIC: {
                madt_io_apic_t* ioapic = (madt_io_apic_t*)entry;
                if (apic_info.io_apic_count < 8) {
                    apic_info.io_apics[apic_info.io_apic_count].io_apic_id = 
                        ioapic->io_apic_id;
                    apic_info.io_apics[apic_info.io_apic_count].base_address = 
                        ioapic->io_apic_address;
                    apic_info.io_apics[apic_info.io_apic_count].gsi_base = 
                        ioapic->global_system_interrupt_base;
                    apic_info.io_apic_count++;
                    
                    terminal_printf(&main_terminal, 
                        "APIC: I/O APIC - ID=%u, Address=0x%08x, GSI Base=%u\r\n",
                        ioapic->io_apic_id, ioapic->io_apic_address, 
                        ioapic->global_system_interrupt_base);
                }
                break;
            }
            
            case MADT_TYPE_INTERRUPT_OVERRIDE: {
                madt_interrupt_override_t* override = (madt_interrupt_override_t*)entry;
                if (apic_info.override_count < 24) {
                    apic_info.overrides[apic_info.override_count].irq_source = 
                        override->irq_source;
                    apic_info.overrides[apic_info.override_count].gsi = 
                        override->global_system_interrupt;
                    apic_info.overrides[apic_info.override_count].flags = 
                        override->flags;
                    apic_info.overrides[apic_info.override_count].active_low = 
                        (override->flags & 0x3) == 0x3;
                    apic_info.overrides[apic_info.override_count].level_triggered = 
                        ((override->flags >> 2) & 0x3) == 0x3;
                    apic_info.override_count++;
                    
                    terminal_printf(&main_terminal, 
                        "APIC: IRQ Override - Source IRQ=%u -> GSI=%u, flags=0x%04x\r\n",
                        override->irq_source, override->global_system_interrupt, 
                        override->flags);
                }
                break;
            }
            
            default:
                // Ignorar otros tipos por ahora
                break;
        }
        
        entry_ptr += entry->length;
    }
    
    return true;
}

// ========================================================================
// LOCAL APIC
// ========================================================================

void lapic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t* addr = (volatile uint32_t*)(apic_info.lapic_base_virt + reg);
    *addr = value;
}

uint32_t lapic_read(uint32_t reg) {
    volatile uint32_t* addr = (volatile uint32_t*)(apic_info.lapic_base_virt + reg);
    return *addr;
}

void lapic_eoi(void) {
    if (!apic_info.initialized || !apic_info.using_apic) {
        return;
    }
    
    // Escribir 0 al registro EOI
    lapic_write(LAPIC_EOI, 0);
    
    // Memory barrier para asegurar que la escritura se complete
    __asm__ volatile("" ::: "memory");
    
}

uint8_t lapic_get_id(void) {
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void apic_enable(void) {
    // Habilitar APIC via MSR
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);
    apic_base |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_base);
    
    // Configurar Spurious Interrupt Vector Register
    // Vector 0xFF (spurious) + bit 8 (APIC enable)
    lapic_write(LAPIC_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);
    
    apic_info.lapic_enabled = true;
    terminal_puts(&main_terminal, "APIC: Local APIC enabled\r\n");
}

void apic_disable(void) {
    // Deshabilitar APIC via MSR
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);
    apic_base &= ~IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_base);
    
    apic_info.lapic_enabled = false;
    apic_info.using_apic = false;
}

// ========================================================================
// I/O APIC
// ========================================================================

static uint32_t ioapic_get_base(uint8_t io_apic_index) {
    if (io_apic_index >= apic_info.io_apic_count) {
        return 0;
    }
    return apic_info.io_apics[io_apic_index].virtual_address;
}

void ioapic_write(uint8_t io_apic_index, uint8_t reg, uint32_t value) {
    uint32_t base = ioapic_get_base(io_apic_index);
    if (!base) return;
    
    volatile uint32_t* regsel = (volatile uint32_t*)(base + IOAPIC_REGSEL);
    volatile uint32_t* regwin = (volatile uint32_t*)(base + IOAPIC_REGWIN);
    
    *regsel = reg;
    *regwin = value;
}

uint32_t ioapic_read(uint8_t io_apic_index, uint8_t reg) {
    uint32_t base = ioapic_get_base(io_apic_index);
    if (!base) return 0;
    
    volatile uint32_t* regsel = (volatile uint32_t*)(base + IOAPIC_REGSEL);
    volatile uint32_t* regwin = (volatile uint32_t*)(base + IOAPIC_REGWIN);
    
    *regsel = reg;
    return *regwin;
}

uint32_t apic_irq_to_gsi(uint8_t irq) {
    // Buscar override
    for (uint8_t i = 0; i < apic_info.override_count; i++) {
        if (apic_info.overrides[i].irq_source == irq) {
            return apic_info.overrides[i].gsi;
        }
    }
    
    // Sin override, IRQ = GSI (para el primer I/O APIC)
    return irq;
}

interrupt_override_t* apic_get_override(uint8_t irq) {
    for (uint8_t i = 0; i < apic_info.override_count; i++) {
        if (apic_info.overrides[i].irq_source == irq) {
            return &apic_info.overrides[i];
        }
    }
    return NULL;
}

void ioapic_set_irq(uint8_t irq, uint8_t vector, bool masked) {
    uint32_t gsi = apic_irq_to_gsi(irq);
    
    // Encontrar I/O APIC responsable de este GSI
    uint8_t io_apic_index = 0;
    uint8_t redirect_entry = gsi;
    
    for (uint8_t i = 0; i < apic_info.io_apic_count; i++) {
        if (gsi >= apic_info.io_apics[i].gsi_base &&
            gsi < apic_info.io_apics[i].gsi_base + 
                  apic_info.io_apics[i].max_redirect_entries) {
            io_apic_index = i;
            redirect_entry = gsi - apic_info.io_apics[i].gsi_base;
            break;
        }
    }
    
    // Configurar entrada de redirección
    interrupt_override_t* override = apic_get_override(irq);
    
    uint64_t entry = vector;
    
    // Delivery mode: Fixed
    entry |= IOAPIC_DELIVERY_FIXED;
    
    // Destination mode: Physical
    entry |= IOAPIC_DEST_PHYSICAL;
    
    // Polarity
    if (override && override->active_low) {
        entry |= IOAPIC_POLARITY_LOW;
    } else {
        entry |= IOAPIC_POLARITY_HIGH;
    }
    
    // Trigger mode
    if (override && override->level_triggered) {
        entry |= IOAPIC_TRIGGER_LEVEL;
    } else {
        entry |= IOAPIC_TRIGGER_EDGE;
    }
    
    // Masked
    if (masked) {
        entry |= IOAPIC_MASKED;
    }
    
    // Destination (Local APIC ID)
    entry |= ((uint64_t)apic_info.lapic_id) << 56;
    
    // Escribir entrada (low 32 bits, high 32 bits)
    uint8_t reg_low = IOAPIC_REG_REDTBL_BASE + (redirect_entry * 2);
    uint8_t reg_high = reg_low + 1;
    
    ioapic_write(io_apic_index, reg_low, (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(io_apic_index, reg_high, (uint32_t)(entry >> 32));
}

void ioapic_mask_irq(uint8_t irq) {
    uint32_t gsi = apic_irq_to_gsi(irq);
    
    // Encontrar I/O APIC
    uint8_t io_apic_index = 0;
    uint8_t redirect_entry = gsi;
    
    for (uint8_t i = 0; i < apic_info.io_apic_count; i++) {
        if (gsi >= apic_info.io_apics[i].gsi_base &&
            gsi < apic_info.io_apics[i].gsi_base + 
                  apic_info.io_apics[i].max_redirect_entries) {
            io_apic_index = i;
            redirect_entry = gsi - apic_info.io_apics[i].gsi_base;
            break;
        }
    }
    
    uint8_t reg_low = IOAPIC_REG_REDTBL_BASE + (redirect_entry * 2);
    uint32_t entry = ioapic_read(io_apic_index, reg_low);
    entry |= IOAPIC_MASKED;
    ioapic_write(io_apic_index, reg_low, entry);
}

void ioapic_unmask_irq(uint8_t irq) {
    uint32_t gsi = apic_irq_to_gsi(irq);
    
    // Encontrar I/O APIC
    uint8_t io_apic_index = 0;
    uint8_t redirect_entry = gsi;
    
    for (uint8_t i = 0; i < apic_info.io_apic_count; i++) {
        if (gsi >= apic_info.io_apics[i].gsi_base &&
            gsi < apic_info.io_apics[i].gsi_base + 
                  apic_info.io_apics[i].max_redirect_entries) {
            io_apic_index = i;
            redirect_entry = gsi - apic_info.io_apics[i].gsi_base;
            break;
        }
    }
    
    uint8_t reg_low = IOAPIC_REG_REDTBL_BASE + (redirect_entry * 2);
    uint32_t entry = ioapic_read(io_apic_index, reg_low);
    entry &= ~IOAPIC_MASKED;
    ioapic_write(io_apic_index, reg_low, entry);
}

// ========================================================================
// TIMER DEL LOCAL APIC
// ========================================================================

uint32_t lapic_timer_calibrate(void) {
    terminal_puts(&main_terminal, "APIC: Calibrating Local APIC timer...\r\n");
    
    // ✅ Usar PIT Channel 0 (más fiable)
    // Detener interrupciones durante calibración
    __asm__ volatile("cli");
    
    // Configurar PIT Channel 0 para one-shot a 100Hz
    outb(0x43, 0x30);  // Channel 0, lo/hi byte, mode 0
    uint16_t pit_count = 11932;  // ~10ms @ 1.193182 MHz
    outb(0x40, pit_count & 0xFF);
    outb(0x40, (pit_count >> 8) & 0xFF);
    
    // Configurar APIC timer
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);  // Masked durante calibración
    
    // Leer valor inicial del PIT
    outb(0x43, 0x00);  // Latch command
    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);
    uint16_t pit_start = (high << 8) | low;
    
    // Iniciar APIC timer con valor máximo
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    
    // Esperar ~10ms
    for (volatile int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    
    // Leer APIC timer
    uint32_t apic_count = lapic_read(LAPIC_TIMER_CCR);
    uint32_t apic_ticks = 0xFFFFFFFF - apic_count;
    
    // Detener timer
    lapic_write(LAPIC_TIMER_ICR, 0);
    
    // Calcular ticks per ms (con divisor 16)
    uint32_t ticks_per_ms = (apic_ticks * 16) / 10;
    
    // ✅ Validación más estricta
    if (ticks_per_ms < 100 || ticks_per_ms > 10000000) {
        terminal_printf(&main_terminal, 
            "APIC: Calibration suspicious: %u ticks/ms, using default\r\n",
            ticks_per_ms);
        ticks_per_ms = 100000;  // Default conservador
    }
    
    apic_info.timer_ticks_per_ms = ticks_per_ms;
    
    __asm__ volatile("sti");
    
    terminal_printf(&main_terminal, 
        "APIC: Timer calibrated: %u ticks per ms\r\n", ticks_per_ms);
    
    return ticks_per_ms;
}

void lapic_timer_init(uint32_t frequency_hz) {
    terminal_printf(&main_terminal, "APIC: Initializing timer at %u Hz...\r\n", frequency_hz);
    
    // ✅ Calibrar SIEMPRE
    __asm__ volatile("cli");
    apic_info.timer_ticks_per_ms = 0;
    lapic_timer_calibrate();
    __asm__ volatile("sti");
    
    if (apic_info.timer_ticks_per_ms == 0) {
        terminal_puts(&main_terminal, "APIC: Calibration failed, disabling APIC timer\r\n");
        apic_info.using_apic = false;
        return;
    }
    
    // ✅ FIX: Cálculo correcto para cualquier frecuencia
    uint32_t initial_count;
    
    if (frequency_hz >= 1000) {
        // Alta frecuencia: calcular ticks directamente
        // ticks_per_second = ticks_per_ms * 1000
        // ticks_per_interrupt = ticks_per_second / frequency_hz
        uint32_t ticks_per_second = apic_info.timer_ticks_per_ms * 1000;
        initial_count = ticks_per_second / frequency_hz;
        
        // Aplicar divisor (estamos usando DIV_16)
        initial_count /= 16;
    } else {
        // Baja frecuencia: usar milisegundos
        uint32_t ms_per_interrupt = 1000 / frequency_hz;
        initial_count = (apic_info.timer_ticks_per_ms * ms_per_interrupt) / 16;
    }
    
    // ✅ FIX: Validación más estricta
    if (initial_count == 0) {
        terminal_puts(&main_terminal, "APIC: Initial count too low, using minimum\r\n");
        initial_count = 100;  // Valor mínimo seguro
    }
    
    if (initial_count > 0xFFFFFF00) {
        terminal_puts(&main_terminal, "APIC: Initial count too high, capping\r\n");
        initial_count = 0xFFFFFF00;
    }
    
    terminal_printf(&main_terminal, 
        "APIC: Timer config: ticks/ms=%u, freq=%uHz, initial_count=%u\r\n",
        apic_info.timer_ticks_per_ms, frequency_hz, initial_count);
    
    // Configurar divisor
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    
    // ✅ Vector 32 (IRQ 0)
    uint32_t vector = 32;
    
    // Verificar que Local APIC esté habilitado
    uint32_t svr = lapic_read(LAPIC_SVR);
    if (!(svr & LAPIC_SVR_ENABLE)) {
        terminal_puts(&main_terminal, "APIC: WARNING - Local APIC not enabled, enabling now\r\n");
        apic_enable();
    }
    
    // ✅ Configurar LVT Timer: vector + periódico + NO masked
    uint32_t lvt_value = vector | LAPIC_LVT_TIMER_PERIODIC;
    lapic_write(LAPIC_LVT_TIMER, lvt_value);
    
    // Configurar initial count
    lapic_write(LAPIC_TIMER_ICR, initial_count);
    
    // Verificar configuración
    uint32_t lvt_check = lapic_read(LAPIC_LVT_TIMER);
    uint32_t icr_check = lapic_read(LAPIC_TIMER_ICR);
    
    terminal_printf(&main_terminal, 
        "APIC: Timer verification - LVT=0x%08x (masked=%d), ICR=%u\r\n",
        lvt_check, (lvt_check & LAPIC_LVT_MASKED) ? 1 : 0, icr_check);
    
    if (lvt_check & LAPIC_LVT_MASKED) {
        terminal_puts(&main_terminal, "ERROR: Timer still masked after config!\r\n");
        return;
    }
    
    // Verificar que el timer esté corriendo
    uint32_t ccr_initial = lapic_read(LAPIC_TIMER_CCR);
    for (volatile int i = 0; i < 10000; i++);
    uint32_t ccr_after = lapic_read(LAPIC_TIMER_CCR);
    
    if (ccr_initial == ccr_after) {
        terminal_puts(&main_terminal, "ERROR: Timer not counting!\r\n");
        terminal_printf(&main_terminal, "  CCR initial: %u, after delay: %u\r\n", 
                       ccr_initial, ccr_after);
        return;
    }
    
    apic_info.timer_frequency = frequency_hz;
    terminal_puts(&main_terminal, "APIC: Timer initialized and verified\r\n");
}

bool apic_verify_state(void) {
    if (!apic_info.initialized || !apic_info.using_apic) {
        return false;
    }
    
    // Verificar que el Local APIC esté habilitado vía MSR
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);
    if (!(apic_base & IA32_APIC_BASE_ENABLE)) {
        terminal_puts(&main_terminal, "APIC: ERROR - APIC disabled in MSR!\r\n");
        return false;
    }
    
    // Verificar SVR
    uint32_t svr = lapic_read(LAPIC_SVR);
    if (!(svr & LAPIC_SVR_ENABLE)) {
        terminal_puts(&main_terminal, "APIC: ERROR - APIC not enabled in SVR!\r\n");
        return false;
    }
    
    // Verificar que la dirección virtual sea válida
    if (!apic_info.lapic_base_virt) {
        terminal_puts(&main_terminal, "APIC: ERROR - Invalid virtual address!\r\n");
        return false;
    }
    
    // Leer ID para verificar acceso
    uint8_t id = lapic_get_id();
    if (id != apic_info.lapic_id) {
        terminal_printf(&main_terminal, "APIC: WARNING - ID mismatch (expected %u, got %u)\r\n",
                       apic_info.lapic_id, id);
    }
    
    terminal_puts(&main_terminal, "APIC: State verification passed\r\n");
    return true;
}


void lapic_timer_oneshot(uint32_t initial_count) {
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_1);
    lapic_write(LAPIC_LVT_TIMER, 32);  // Vector 32, one-shot
    lapic_write(LAPIC_TIMER_ICR, initial_count);
}

void lapic_timer_periodic(uint32_t initial_count) {
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_1);
    lapic_write(LAPIC_LVT_TIMER, 32 | LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, initial_count);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0);
}

// ========================================================================
// PIC DISABLE
// ========================================================================

static void apic_disable_pic(void) {
    terminal_puts(&main_terminal, "APIC: Disabling legacy PIC...\r\n");
    
    // Enmascarar todas las interrupciones del PIC
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    
    apic_info.pic_disabled = true;
    
    terminal_puts(&main_terminal, "APIC: PIC disabled\r\n");
}

// ========================================================================
// UTILIDADES
// ========================================================================

void apic_print_info(void) {
    terminal_puts(&main_terminal, "\r\n=== APIC Information ===\r\n");
    
    if (!apic_info.initialized) {
        terminal_puts(&main_terminal, "APIC not initialized\r\n");
        return;
    }
    
    terminal_printf(&main_terminal, "Using APIC: %s\r\n", 
                   apic_info.using_apic ? "Yes" : "No (PIC fallback)");
    terminal_printf(&main_terminal, "PIC Disabled: %s\r\n",
                   apic_info.pic_disabled ? "Yes" : "No");
    
    terminal_puts(&main_terminal, "\r\n--- Local APIC ---\r\n");
    terminal_printf(&main_terminal, "Base Address: Phys=0x%08x, Virt=0x%08x\r\n",
                   apic_info.lapic_base_phys, apic_info.lapic_base_virt);
    terminal_printf(&main_terminal, "ID: %u\r\n", apic_info.lapic_id);
    terminal_printf(&main_terminal, "Version: 0x%08x\r\n", apic_info.lapic_version);
    terminal_printf(&main_terminal, "Enabled: %s\r\n",
                   apic_info.lapic_enabled ? "Yes" : "No");
    
    if (apic_info.timer_ticks_per_ms > 0) {
        terminal_printf(&main_terminal, "Timer: %u Hz (%u ticks/ms)\r\n",
                       apic_info.timer_frequency, apic_info.timer_ticks_per_ms);
    }
    
    terminal_puts(&main_terminal, "\r\n--- I/O APICs ---\r\n");
    terminal_printf(&main_terminal, "Count: %u\r\n", apic_info.io_apic_count);
    
    for (uint8_t i = 0; i < apic_info.io_apic_count; i++) {
        terminal_printf(&main_terminal, "  [%u] ID=%u, Base=0x%08x, GSI Base=%u, "
                       "Max Entries=%u\r\n",
                       i,
                       apic_info.io_apics[i].io_apic_id,
                       apic_info.io_apics[i].base_address,
                       apic_info.io_apics[i].gsi_base,
                       apic_info.io_apics[i].max_redirect_entries);
    }
    
    terminal_puts(&main_terminal, "\r\n--- Local APICs (Processors) ---\r\n");
    terminal_printf(&main_terminal, "Count: %u\r\n", apic_info.local_apic_count);
    
    for (uint8_t i = 0; i < apic_info.local_apic_count; i++) {
        terminal_printf(&main_terminal, "  [%u] Processor=%u, APIC ID=%u, Enabled=%s\r\n",
                       i,
                       apic_info.local_apics[i].processor_id,
                       apic_info.local_apics[i].apic_id,
                       apic_info.local_apics[i].enabled ? "Yes" : "No");
    }
    
    if (apic_info.override_count > 0) {
        terminal_puts(&main_terminal, "\r\n--- IRQ Overrides ---\r\n");
        for (uint8_t i = 0; i < apic_info.override_count; i++) {
            terminal_printf(&main_terminal, 
                "  IRQ %u -> GSI %u (Active %s, %s-triggered)\r\n",
                apic_info.overrides[i].irq_source,
                apic_info.overrides[i].gsi,
                apic_info.overrides[i].active_low ? "Low" : "High",
                apic_info.overrides[i].level_triggered ? "Level" : "Edge");
        }
    }
    
    terminal_puts(&main_terminal, "\r\n");
}

bool apic_is_enabled(void) {
    return apic_info.initialized && apic_info.using_apic;
}
