[BITS 32]

; --- GLOBALES ---
global irq0_entry, irq1_entry, irq2_entry, irq3_entry, irq4_entry, irq5_entry, irq6_entry, irq7_entry
global irq8_entry, irq9_entry, irq10_entry, irq11_entry, irq12_entry, irq13_entry, irq14_entry, irq15_entry
global irq48_entry, irq49_entry, irq50_entry, irq51_entry, irq52_entry, irq60_entry, irq62_entry, irq63_entry

; --- EXTERNOS ---
extern timer_irq_handler
extern keyboard_irq_handler
extern mouse_irq_handler
extern ahci_irq_handler
extern serial_irq_handler_line
extern irq_common_handler

section .text

; --- MACRO PARA IRQs ---
%macro IRQ 2
%1:
    cli
    push dword 0        ; dummy error code
    push dword %2       ; int_no
    jmp irq_common_stub
%endmacro

; --- STUB COMÚN PARA IRQs ---
irq_common_stub:
    pusha
    
    push ds
    push es
    push fs
    push gs
    
    ; Cargar segmentos del kernel
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Puntero a regs para el handler en C
    push esp
    
    ; Obtener int_no del stack (esta en ESP + 48)
    ; Regs=4, GS=4, FS=4, ES=4, DS=4, Pusha=32 -> 52 offset
    ; Pero ESP apunta a regs, asi que [esp+52] es int_no
    mov eax, [esp + 52]
    
    ; Dispatcher manual para handlers especificos
    cmp eax, 32         ; IRQ 0 (Timer)
    je .call_timer
    cmp eax, 48         ; IRQ 0 (Timer APIC)
    je .call_timer
    
    cmp eax, 33         ; IRQ 1 (Keyboard)
    je .call_kbd
    cmp eax, 49         ; IRQ 1 (Keyboard APIC)
    je .call_kbd
    
    cmp eax, 44         ; IRQ 12 (Mouse)
    je .call_mouse
    cmp eax, 60         ; IRQ 12 (Mouse APIC)
    je .call_mouse
    
    cmp eax, 46         ; IRQ 14 (IDE/AHCI)
    je .call_ahci
    cmp eax, 62         ; IRQ 14 (IDE APIC)
    je .call_ahci
    
    cmp eax, 35         ; IRQ 3 (Serial 2)
    je .call_serial3
    cmp eax, 51         ; IRQ 3 APIC
    je .call_serial3
    
    cmp eax, 36         ; IRQ 4 (Serial 1)
    je .call_serial4
    cmp eax, 52         ; IRQ 4 APIC
    je .call_serial4

    ; Default: handler común
    call irq_common_handler
    jmp .done

.call_timer:
    call timer_irq_handler
    jmp .done

.call_kbd:
    call keyboard_irq_handler
    jmp .done

.call_mouse:
    call mouse_irq_handler
    jmp .done

.call_ahci:
    call ahci_irq_handler
    jmp .done

.call_serial3:
    push dword 3
    call serial_irq_handler_line
    add esp, 4
    jmp .done

.call_serial4:
    push dword 4
    call serial_irq_handler_line
    add esp, 4
    jmp .done

.done:
    add esp, 4          ; Limpiar puntero esp
    
    pop gs
    pop fs
    pop es
    pop ds
    
    popa
    add esp, 8          ; Limpiar int_no y error_code
    iretd

; --- DEFINICIONES DE ENTRADAS ---
; PIC Vectors (32-47)
IRQ irq0_entry, 32
IRQ irq1_entry, 33
IRQ irq2_entry, 34
IRQ irq3_entry, 35
IRQ irq4_entry, 36
IRQ irq5_entry, 37
IRQ irq6_entry, 38
IRQ irq7_entry, 39
IRQ irq8_entry, 40
IRQ irq9_entry, 41
IRQ irq10_entry, 42
IRQ irq11_entry, 43
IRQ irq12_entry, 44
IRQ irq13_entry, 45
IRQ irq14_entry, 46
IRQ irq15_entry, 47

; APIC Vectors (48+)
IRQ irq48_entry, 48     ; Timer
IRQ irq49_entry, 49     ; Kbd
IRQ irq50_entry, 50     ; 
IRQ irq51_entry, 51     ; Serial 2
IRQ irq52_entry, 52     ; Serial 1
IRQ irq60_entry, 60     ; Mouse
IRQ irq62_entry, 62     ; IDE/AHCI
IRQ irq63_entry, 63     ; IDE 2


