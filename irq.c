#include "irq.h"
#include "idt.h"
#include "io.h"
#include "kernel.h"
#include "task.h"
#include "mouse.h"
#include "apic.h"


volatile uint32_t ticks = 0;
volatile uint32_t ticks_since_boot = 0;
static volatile uint32_t irq_depth = 0;

void pic_send_eoi(uint8_t irq) {
    // ✅ FIX: Manejo consistente de APIC
    if (apic_is_enabled()) {
        // Con APIC, siempre hacer EOI del Local APIC
        // El parámetro irq se ignora porque APIC usa broadcast EOI
        lapic_eoi();
        return;
    }
    
    // Fallback a PIC legacy (solo si APIC no está disponible)
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);  // EOI al PIC esclavo primero
    }
    outb(PIC1_COMMAND, PIC_EOI);      // EOI al PIC maestro
}

void irq_common_handler(struct regs* r) {
    uint8_t irq = r->int_no - 32;
    terminal_printf(&main_terminal, "IRQ %d received\n", irq);
    pic_send_eoi(irq);
}


void timer_irq_handler() {
    ticks++;
    ticks_since_boot++;
    
    // Scheduler tick
    if (scheduler.scheduler_enabled) {
        scheduler_tick();
    }
    
    // ✅ FIX: EOI después de TODA la lógica
    // Esto asegura que no recibimos otra interrupción antes de terminar
    pic_send_eoi(0);
}


void mouse_irq_handler() {
    mouse_handle_irq();
    pic_send_eoi(12);
}

void kernel_delay(uint32_t milliseconds) {
    if (milliseconds == 0) return;

    uint32_t start_ticks = ticks_since_boot;
    uint32_t target_ticks = start_ticks + (milliseconds / 10);  // Assuming 100 Hz (10ms per tick)

    // Adjust for fractional milliseconds if needed
    if (milliseconds % 10) {
        target_ticks += 1;  // Round up to next tick
    }

    while (ticks_since_boot < target_ticks) {
        __asm__ volatile("hlt");  // Halt CPU to reduce power usage while waiting
    }
}

// Canal 0, modo 3 (Square Wave Generator), acceso low/high, modo binario
#define PIT_CHANNEL0_PORT 0x40
#define PIT_COMMAND_PORT  0x43
#define PIT_BASE_FREQ     1193180  // Hz

void pit_init(uint32_t freq_hz) {
    // Si APIC está disponible, usar su timer
    if (apic_is_enabled()) {
        terminal_printf(&main_terminal, "PIT: Using APIC timer at %u Hz\r\n", freq_hz);
        lapic_timer_init(freq_hz);
        return;
    }
    
    // Fallback a PIT legacy
    terminal_printf(&main_terminal, "PIT: Using legacy PIT at %u Hz\r\n", freq_hz);
    
    if (freq_hz == 0) {
        return;  // evitar división por 0
    }
    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor == 0) divisor = 65536;  // El PIT interpreta divisor=0 como 65536

    if (divisor > 65536) {
        // frecuencia demasiado baja para PIT, ajustamos al mínimo posible
        divisor = 65536;
    }

    // Configurar PIT: canal 0, modo 3 (square wave), acceso bajo/alto byte
    outb(PIT_COMMAND_PORT, 0x36);

    // Enviar divisor (low byte, luego high byte)
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
}

void irq_setup_apic(void) {
    if (!apic_is_enabled()) {
        terminal_puts(&main_terminal, "IRQ: APIC not enabled, using PIC\r\n");
        return;
    }
    
    terminal_puts(&main_terminal, "IRQ: Configuring APIC mode interrupts\r\n");
    
    // ✅ Configurar IRQs importantes
    
    // Timer del sistema (IRQ 0 -> Vector 32)
    // ✅ FIX: Si usamos APIC timer, mask el PIT
    if (apic_info.timer_frequency > 0) {
        ioapic_set_irq(0, 32, true);  // Masked porque usamos APIC timer
        terminal_puts(&main_terminal, "  IRQ 0 (PIT) -> Masked (using APIC timer)\r\n");
    } else {
        ioapic_set_irq(0, 32, false);  // Usar PIT como fallback
        terminal_puts(&main_terminal, "  IRQ 0 (PIT) -> Vector 32 (fallback)\r\n");
    }
    
    // Teclado (IRQ 1 -> Vector 33)
    ioapic_set_irq(1, 33, false);
    terminal_puts(&main_terminal, "  IRQ 1 (Keyboard) -> Vector 33\r\n");
    
    // Mouse (IRQ 12 -> Vector 44)
    ioapic_set_irq(12, 44, false);
    terminal_puts(&main_terminal, "  IRQ 12 (Mouse) -> Vector 44\r\n");
    
    // Disco primario (IRQ 14 -> Vector 46)
    ioapic_set_irq(14, 46, false);
    terminal_puts(&main_terminal, "  IRQ 14 (IDE Primary) -> Vector 46\r\n");
    
    // Disco secundario (IRQ 15 -> Vector 47)
    ioapic_set_irq(15, 47, false);
    terminal_puts(&main_terminal, "  IRQ 15 (IDE Secondary) -> Vector 47\r\n");
    
    terminal_puts(&main_terminal, "IRQ: APIC configuration complete\r\n");
}