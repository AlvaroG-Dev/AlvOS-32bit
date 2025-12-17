#include "irq.h"
#include "apic.h"
#include "idt.h"
#include "io.h"
#include "kernel.h"
#include "mouse.h"
#include "task.h"

volatile uint32_t ticks = 0;
volatile uint32_t ticks_since_boot = 0;
static volatile uint32_t irq_depth = 0;

void pic_send_eoi(uint8_t irq) {
  // ✅ Verificar estado del sistema primero
  if (!apic_info.initialized || !apic_info.using_apic) {
    // Usar PIC legacy
    if (irq >= 8) {
      outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
    return;
  }

  // ✅ Verificar que APIC esté habilitado y accesible
  if (apic_info.lapic_enabled && apic_info.lapic_base_virt) {
    // Enviar EOI al Local APIC
    lapic_write(LAPIC_EOI, 0);

    // Memory barrier
    __asm__ volatile("" ::: "memory");
  } else {
    // Fallback a PIC
    terminal_printf(&main_terminal, "APIC: Fallback to PIC EOI for IRQ %u\r\n",
                    irq);
    if (irq >= 8) {
      outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
  }
}

void irq_common_handler(struct regs *r) {
  uint8_t irq = r->int_no - 32;
  terminal_printf(&main_terminal, "IRQ %d received\n", irq);
  pic_send_eoi(irq);
}

void timer_irq_handler() {
  ticks++;
  ticks_since_boot++;

  // ✅ FIX: Scheduler tick ANTES de EOI
  // Esto evita race conditions
  if (scheduler.scheduler_enabled) {
    scheduler_tick();
  }

  // ✅ EOI al final
  pic_send_eoi(0);
}

void mouse_irq_handler() {
  mouse_handle_irq();
  pic_send_eoi(12);
}

void kernel_delay(uint32_t milliseconds) {
  if (milliseconds == 0)
    return;

  uint32_t start_ticks = ticks_since_boot;
  uint32_t target_ticks =
      start_ticks + (milliseconds / 10); // Assuming 100 Hz (10ms per tick)

  // Adjust for fractional milliseconds if needed
  if (milliseconds % 10) {
    target_ticks += 1; // Round up to next tick
  }

  while (ticks_since_boot < target_ticks) {
    __asm__ volatile("hlt"); // Halt CPU to reduce power usage while waiting
  }
}

// Canal 0, modo 3 (Square Wave Generator), acceso low/high, modo binario
#define PIT_CHANNEL0_PORT 0x40
#define PIT_COMMAND_PORT 0x43
#define PIT_BASE_FREQ 1193180 // Hz

void pit_init(uint32_t freq_hz) {
  // Si APIC está disponible, usar su timer
  if (apic_is_enabled()) {
    terminal_printf(&main_terminal, "PIT: Using APIC timer at %u Hz\r\n",
                    freq_hz);
    lapic_timer_init(freq_hz);
    return;
  }

  // Fallback a PIT legacy
  terminal_printf(&main_terminal, "PIT: Using legacy PIT at %u Hz\r\n",
                  freq_hz);

  if (freq_hz == 0) {
    return; // evitar división por 0
  }
  uint32_t divisor = PIT_BASE_FREQ / freq_hz;
  if (divisor == 0)
    divisor = 65536; // El PIT interpreta divisor=0 como 65536

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
  // ✅ Verificar si APIC está inicializado y funcional
  if (!apic_info.initialized) {
    terminal_puts(&main_terminal, "IRQ: APIC not initialized\r\n");
    return;
  }

  if (!apic_info.using_apic) {
    terminal_puts(&main_terminal, "IRQ: APIC not in use (PIC mode)\r\n");
    return;
  }

  // ✅ Verificar estado críticamente
  terminal_puts(&main_terminal,
                "IRQ: Verifying APIC state before configuration...\r\n");

  if (!apic_verify_state()) {
    terminal_puts(&main_terminal, "IRQ: APIC state verification FAILED\r\n");
    return;
  }

  // ✅ Verificar que tenemos I/O APICs
  if (apic_info.io_apic_count == 0) {
    terminal_puts(&main_terminal, "IRQ: No I/O APICs found\r\n");
    return;
  }

  terminal_puts(&main_terminal, "IRQ: Configuring APIC mode interrupts\r\n");

  // ✅ CRÍTICO: Deshabilitar interrupciones durante configuración
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  // ✅ 1. MÁSCARA TODAS las interrupciones en I/O APIC primero
  terminal_puts(&main_terminal, "  Masking all I/O APIC interrupts...\r\n");
  for (uint8_t irq = 0; irq < 24; irq++) {
    ioapic_mask_irq(irq);
  }

  // ✅ 2. Configurar Local APIC Spurious Interrupt Vector
  terminal_puts(&main_terminal,
                "  Configuring Local APIC spurious vector...\r\n");
  uint32_t svr = lapic_read(LAPIC_SVR);
  lapic_write(LAPIC_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);

  // ✅ 3. Limpiar cualquier interrupción pendiente en Local APIC
  lapic_write(LAPIC_EOI, 0);

  // ✅ 4. Configurar interrupciones específicas con vectores APROPRIADOS
  terminal_puts(&main_terminal, "  Setting up specific interrupts:\r\n");

  // ❗️ IMPORTANTE: Usar vectores DIFERENTES a los del PIC para evitar
  // conflictos PIC usa vectores 32-47, nosotros usaremos 48-63

  // Timer del sistema - IRQ 0
  uint32_t gsi_timer = apic_irq_to_gsi(0);
  if (apic_info.timer_frequency > 0) {
    // ✅ Si APIC timer está funcionando, NO configurar PIT en I/O APIC
    terminal_printf(
        &main_terminal,
        "    IRQ 0 -> GSI %u -> NOT mapped (using APIC timer directly)\r\n",
        gsi_timer);
    // Mantener masked en I/O APIC
    ioapic_mask_irq(0);
  } else {
    // ✅ Fallback: usar PIT a través de I/O APIC con vector 48
    ioapic_set_irq(gsi_timer, 48, false);
    terminal_printf(&main_terminal,
                    "    IRQ 0 -> GSI %u -> Vector 48 (PIT via I/O APIC)\r\n",
                    gsi_timer);

    // Configurar handler para vector 48 en IDT
    extern void irq0_entry(); // Necesitarás crear este stub en assembly
    idt_set_gate(48, (uintptr_t)irq0_entry, 0x08,
                 IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32);
  }

  // Teclado - IRQ 1
  uint32_t gsi_kbd = apic_irq_to_gsi(1);
  ioapic_set_irq(gsi_kbd, 49, false);
  terminal_printf(&main_terminal,
                  "    IRQ 1 -> GSI %u -> Vector 49 (Keyboard)\r\n", gsi_kbd);

  // Configurar handler para vector 49
  extern void irq1_entry();
  idt_set_gate(49, (uintptr_t)irq1_entry, 0x08,
               IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32);

  // Mouse - IRQ 12
  uint32_t gsi_mouse = apic_irq_to_gsi(12);
  ioapic_set_irq(gsi_mouse, 60, false);
  terminal_printf(&main_terminal,
                  "    IRQ 12 -> GSI %u -> Vector 60 (Mouse)\r\n", gsi_mouse);

  // Configurar handler para vector 60
  extern void irq12_entry();
  idt_set_gate(60, (uintptr_t)irq12_entry, 0x08,
               IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32);

  // IDE - IRQ 14 y 15 (masked por ahora)
  uint32_t gsi_ide1 = apic_irq_to_gsi(14);
  ioapic_set_irq(gsi_ide1, 62, true);
  terminal_printf(&main_terminal,
                  "    IRQ 14 -> GSI %u -> Vector 62 (IDE Primary, masked)\r\n",
                  gsi_ide1);

  uint32_t gsi_ide2 = apic_irq_to_gsi(15);
  ioapic_set_irq(gsi_ide2, 63, true);
  terminal_printf(
      &main_terminal,
      "    IRQ 15 -> GSI %u -> Vector 63 (IDE Secondary, masked)\r\n",
      gsi_ide2);

  // ✅ 5. Deshabilitar PIC si APIC está funcionando correctamente
  if (apic_info.lapic_enabled && apic_info.io_apic_count > 0) {
    terminal_puts(&main_terminal, "  Disabling legacy PIC...\r\n");
    apic_disable_pic();
  } else {
    terminal_puts(&main_terminal, "  Keeping PIC enabled as fallback\r\n");
  }

  // ✅ 6. Verificar configuración final
  terminal_puts(&main_terminal, "  Verifying final configuration...\r\n");

  // Verificar que Local APIC esté habilitado
  svr = lapic_read(LAPIC_SVR);
  if (!(svr & LAPIC_SVR_ENABLE)) {
    terminal_puts(&main_terminal,
                  "  ERROR: Local APIC not enabled in SVR!\r\n");
    lapic_write(LAPIC_SVR, svr | LAPIC_SVR_ENABLE);
  }

  // Verificar timer si está configurado
  if (apic_info.timer_frequency > 0) {
    uint32_t lvt_timer = lapic_read(LAPIC_LVT_TIMER);
    terminal_printf(&main_terminal, "  Timer LVT: 0x%08x (masked=%d)\r\n",
                    lvt_timer, (lvt_timer & LAPIC_LVT_MASKED) ? 1 : 0);
  }

  // ✅ 7. Restaurar interrupciones
  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  terminal_puts(&main_terminal, "IRQ: APIC configuration complete\r\n");

  // ✅ 8. Información de debug
  terminal_printf(&main_terminal, "  Local APIC ID: %u\r\n",
                  apic_info.lapic_id);
  terminal_printf(&main_terminal, "  I/O APIC count: %u\r\n",
                  apic_info.io_apic_count);
  terminal_printf(&main_terminal, "  APIC timer frequency: %u Hz\r\n",
                  apic_info.timer_frequency);
}