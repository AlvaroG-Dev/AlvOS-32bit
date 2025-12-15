#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "memutils.h"
#include "string.h"
#include "kernel.h"
#include "apic.h"

// ========================================
// ESTRUCTURAS
// ========================================
idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idt_ptr;
extern void irq0_entry();
extern void irq1_entry();
extern void irq2_entry();
extern void irq3_entry();
extern void irq4_entry();
extern void irq5_entry();
extern void irq6_entry();
extern void irq7_entry();
extern void irq8_entry();
extern void irq9_entry();
extern void irq10_entry();
extern void irq11_entry();
extern void irq12_entry();
extern void irq13_entry();
extern void irq14_entry();
extern void irq15_entry();
// ========================================
// FUNCIONES EXTERNAS (de ASM)
// ========================================
extern void idt_load(uintptr_t);

// ========================================
// FUNCIONES INTERNAS
// ========================================

void idt_set_gate(uint8_t num, uintptr_t base, uint16_t selector, uint8_t flags) {
    if (num >= IDT_ENTRIES) return;
    
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = selector;
    idt[num].zero = 0;
    idt[num].type_attr = flags;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt_entry_t) * IDT_ENTRIES - 1;
    idt_ptr.base = (uintptr_t)&idt;
    
    memset(&idt, 0, sizeof(idt_entry_t) * IDT_ENTRIES);
    
    // Configurar excepciones (sin cambios)
    for (int i = 0; i < 32; i++) {
        if (i == 8) {
            idt_set_gate(8, 0, 0x28, 0x85);
        } else {
            idt_set_gate(i, isr_stub_table[i], 0x08,
                        IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32);
        }
    }
    
    // Configurar IRQs (32-47)
    uintptr_t irq_entries[] = {
        (uintptr_t)irq0_entry, (uintptr_t)irq1_entry, (uintptr_t)irq2_entry,
        (uintptr_t)irq3_entry, (uintptr_t)irq4_entry, (uintptr_t)irq5_entry,
        (uintptr_t)irq6_entry, (uintptr_t)irq7_entry, (uintptr_t)irq8_entry,
        (uintptr_t)irq9_entry, (uintptr_t)irq10_entry, (uintptr_t)irq11_entry,
        (uintptr_t)irq12_entry, (uintptr_t)irq13_entry, (uintptr_t)irq14_entry,
        (uintptr_t)irq15_entry
    };
    
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_entries[i], 0x08,
                    IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32);
    }
    
    
    // Remap PICs (siempre necesario, incluso si luego usamos APIC)
    pic_remap(32, 40);
    
    // Si vamos a usar APIC, las máscaras del PIC se deshabilitarán después
    // Por ahora, dejamos las interrupciones básicas habilitadas
    outb(PIC1_DATA, 0xE8);  // Habilitar IRQ0, IRQ1, IRQ2, IRQ12
    outb(PIC2_DATA, 0xEF);  // Habilitar IRQ12 (bit 4)

    // Cargar IDT
    asm volatile("cli");
    idt_load((uint32_t)&idt_ptr);
    
    terminal_puts(&main_terminal, "IDT: Initialized (ready for PIC or APIC)\r\n");
}

// ========================================
// CONTROLADOR DEL PIC
// ========================================
void pic_remap(int offset1, int offset2) {
    // Guardar máscaras
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    
    // Inicialización
    outb(PIC1_COMMAND, 0x11); // ICW1
    outb(PIC2_COMMAND, 0x11);
    
    // ICW2 - offsets
    outb(PIC1_DATA, offset1);  // IRQ0-7 -> INT 32-39
    outb(PIC2_DATA, offset2);  // IRQ8-15 -> INT 40-47
    
    // ICW3 - cascada
    outb(PIC1_DATA, 0x04); // Slave en IRQ2
    outb(PIC2_DATA, 0x02); // Slave ID
    
    // ICW4 - modo 8086
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    // Restaurar máscaras
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}