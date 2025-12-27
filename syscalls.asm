; syscalls.asm
[bits 32]

global syscall_entry
extern syscall_handler

SYSCALL_VECTOR equ 0x80

section .text

; Interrupt handler para syscall
syscall_entry:
    cli
    
    ; Si venimos de Ring 3, el procesador ya hizo el switch a Ring 0
    ; y pusheo SS, ESP, EFLAGS, CS, EIP, ErrorCode
    
    ; Guardar registros adicionales
    pusha
    
    ; Guardar segmentos de datos
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
    
    ; Preparar argumentos para el handler en C
    push esp        ; Puntero al struct regs
    
    ; Llamar al handler en C
    call syscall_handler
    
    ; Limpiar stack
    add esp, 4
    
    ; Restaurar segmentos
    pop gs
    pop fs
    pop es
    pop ds
    
    ; Restaurar registros
    popa
    
    ; Retornar a Ring 3
    iret