global idt_load

idt_load:
    mov eax, [esp+4] ; si se llama con "call idt_load(ptr)"
    lidt [eax]
    ret
