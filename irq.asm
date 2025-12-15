[BITS 32]
global irq0_entry
global irq1_entry
global irq2_entry
global irq3_entry
global irq4_entry
global irq5_entry
global irq6_entry
global irq7_entry
global irq8_entry
global irq9_entry
global irq10_entry
global irq11_entry
global irq12_entry
global irq13_entry
global irq14_entry
global irq15_entry

extern mouse_irq_handler
extern timer_irq_handler
extern keyboard_irq_handler
extern serial_irq_handler_line
extern irq_common_handler
extern ahci_irq_handler
extern mouse_irq_handler
section .text

irq0_entry:
    cli
    pusha
    push ds
    push es
    push fs
    push gs
    
    ; Set kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    call timer_irq_handler
    
    ; Restore segments
    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

irq1_entry:
    cli
    pusha
    call keyboard_irq_handler
    popa
    iretd

irq2_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq3_entry:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword 3              ; <-- pasa IRQ 3
    call serial_irq_handler_line
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iret

irq4_entry:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword 4              ; <-- pasa IRQ 4
    call serial_irq_handler_line
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iret

irq5_entry:
    pusha
    call irq_common_handler
    popa
    iretd


irq6_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq7_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq8_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq9_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq10_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq11_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq12_entry:
    cli
    pusha
    push ds
    push es
    push fs
    push gs
    
    ; Set kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call mouse_irq_handler

    ; Restore segments
    pop gs
    pop fs
    pop es
    pop ds
    popa
    sti
    iretd

irq13_entry:
    pusha
    call irq_common_handler
    popa
    iretd

irq14_entry:
    cli
    pusha
    push ds
    push es
    push fs
    push gs
    
    ; Set kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    call ahci_irq_handler
    
    ; Restore segments
    pop gs
    pop fs
    pop es
    pop ds
    popa
    sti
    iretd

irq15_entry:
    pusha
    call irq_common_handler
    popa
    iretd

