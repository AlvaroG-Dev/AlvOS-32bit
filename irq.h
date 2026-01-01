#ifndef IRQ_H
#define IRQ_H

#include "isr.h"
#include <stddef.h>
#include <stdint.h>

// Definiciones para los IRQs
#define IRQ0  32
#define IRQ1  33
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

// Tabla de punteros a los stubs de IRQ
extern uintptr_t irq_stub_table[];
extern volatile uint32_t ticks;
extern volatile uint32_t ticks_since_boot;
// Tipo para manejadores de IRQ
typedef void (*irq_handler_t)(struct regs*);

// Funciones públicas
void timer_irq_handler(void);             // <-- Debe estar visible
void irq_common_handler(struct regs* r);
void pic_send_eoi(uint8_t irq);
void pit_init(uint32_t frequency);
void irq_setup_apic(void);

// Funciones del delay
void kernel_delay_init(uint32_t freq_hz);
void kernel_delay(uint32_t milliseconds);
void kernel_delay_us(uint32_t microseconds);
void kernel_safe_delay(uint32_t milliseconds);
void kernel_active_delay(uint32_t milliseconds);
bool kernel_delay_condition(uint32_t milliseconds, bool (*condition)(void));
void kernel_calibrate_delay(void);

#endif