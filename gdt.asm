global gdt_flush
global tss_flush

gdt_flush:
    mov eax, [esp+4]   ; obtener puntero a gdt_ptr desde stack
    lgdt [eax]

    ; Far jump para actualizar CS
    jmp 0x08:flush
flush:
    mov ax, 0x10       ; Data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret


tss_flush:
    mov ax, 0x28          ; Selector de TSS (índice 5 = 0x28)
    ltr ax
    ret
