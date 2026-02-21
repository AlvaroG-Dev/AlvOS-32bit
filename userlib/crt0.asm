[bits 32]

section .text
global _start
extern main
extern _exit

_start:
    ; El kernel pone en el stack de usuario:
    ; [esp]     = argc
    ; [esp + 4] = argv[0]
    ; [esp + 8] = argv[1]
    ; ...
    
    ; Establecer EBP base en 0 para indicar el final de un stack trace
    xor ebp, ebp
    
    ; Obtener argc y la dirección base del array argv desde el stack
    mov eax, [esp]          ; eax = argc
    lea ebx, [esp + 4]      ; ebx = pointer to argv[0] (char **)
    
    ; Preparar los argumentos para main(argc, argv, envp)
    ; envp no está implementado plenamente, así que pasamos NULL
    push 0                  ; envp = NULL
    push ebx                ; argv
    push eax                ; argc
    
    call main

    ; El resultado de main está en EAX. Pasarlo a _exit.
    push eax
    call _exit

    ; Si por alguna razón _exit retorna, detener la CPU
.halt:
    hlt
    jmp .halt
