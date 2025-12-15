#ifndef IDT_H
#define IDT_H

#include <stddef.h>
#include <stdint.h>


#define IDT_ENTRIES         256
#define KERNEL_CODE_SELECTOR 0x08

#define IDT_FLAG_PRESENT     0x80
#define IDT_FLAG_RING0       0x00
#define IDT_FLAG_RING3       0x60
#define IDT_FLAG_INTERRUPT32 0x0E

#define IDT_ENTRY_FLAGS      (IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INTERRUPT32)

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20


typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

void idt_init(void);
void idt_set_gate(uint8_t num, uintptr_t base, uint16_t selector, uint8_t flags);
void pic_remap(int offset1, int offset2);

#endif