// Definir la función main
void main(void) {
    // Mensaje embebido en el código (posición conocida)
    const char msg[] = "HELLO FROM SYSCALL!\n";
    
    // Syscall write(1, msg, 20)
    // EAX = 1 (SYS_WRITE)
    // EBX = 1 (stdout)
    // ECX = puntero al mensaje
    // EDX = tamaño
    __asm__ volatile(
        "movl $1, %%eax\n"          // SYS_WRITE
        "movl $1, %%ebx\n"          // fd = stdout
        "leal %0, %%ecx\n"          // ECX = dirección del mensaje
        "movl $20, %%edx\n"         // size
        "int $0x80\n"               // syscall
        :
        : "m"(msg[0])
        : "eax", "ebx", "ecx", "edx"
    );
    
    // Syscall exit(0)
    __asm__ volatile(
        "movl $0, %%eax\n"          // SYS_EXIT
        "movl $0, %%ebx\n"          // exit code = 0
        "int $0x80\n"
        :
        :
        : "eax", "ebx"
    );
    
    // No debería llegar aquí
    while(1);
}