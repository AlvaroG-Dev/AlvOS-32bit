[bits 32]
section .init
global _start
extern main_entry

_start:
    ; Entry point limpio sin prologue
    mov eax, [esp]     ; argc
    lea ebx, [esp + 4] ; argv
    
    push ebx
    push eax
    
    call main_entry
    
    ; Exit syscall (0 is exit)
    mov ebx, eax
    mov eax, 0
    int 0x80
    hlt
