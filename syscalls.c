// syscalls.c
#include "syscalls.h"
#include "idt.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"
#include "string.h"
#include "kernel.h"
#include "irq.h"  // Para ticks_since_boot

extern void syscall_entry(void);

void syscall_init(void) {
    // Configurar INT 0x80 como puerta de syscall
    // DPL=3 para permitir llamadas desde Ring 3
    idt_set_gate(0x80, (uintptr_t)syscall_entry, 0x08,
                IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_FLAG_INTERRUPT32);
    
    terminal_puts(&main_terminal, "Syscalls initialized (INT 0x80)\r\n");
}

void syscall_handler(struct regs* r) {
    // r->eax contiene el número de syscall
    uint32_t syscall_num = r->eax;
    uint32_t result = 0;
    
    // Despachar syscall
    result = syscall_dispatch(syscall_num, 
                             r->ebx, r->ecx, r->edx, r->esi, r->edi);
    
    // El resultado se devuelve en EAX
    r->eax = result;
}

uint32_t syscall_dispatch(uint32_t syscall_num, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    switch(syscall_num) {
        case SYSCALL_EXIT:
            syscall_exit((int)arg1);
            return 0;
            
        case SYSCALL_WRITE:
            return syscall_write((int32_t)arg1, (const char*)arg2, (uint32_t)arg3);
            
        case SYSCALL_READ:
            return syscall_read((int32_t)arg1, (char*)arg2, (uint32_t)arg3);
            
        case SYSCALL_FORK:
            return syscall_fork();
            
        case SYSCALL_EXEC:
            // Implementación simplificada
            return (uint32_t)-1; // Not implemented yet
            
        case SYSCALL_WAIT:
            return syscall_wait((int32_t*)arg1);
            
        case SYSCALL_BRK:
            return (uint32_t)syscall_brk((void*)arg1);
            
        case SYSCALL_GETPID:
            return syscall_getpid();
            
        case SYSCALL_YIELD:
            syscall_yield();
            return 0;
            
        case SYSCALL_SLEEP:
            return syscall_sleep(arg1);
            
        case SYSCALL_GETTIME:
            return syscall_gettime();
            
        case SYSCALL_OPEN:
            return syscall_open((const char*)arg1, (int32_t)arg2);
            
        case SYSCALL_CLOSE:
            return syscall_close((int32_t)arg1);
            
        case SYSCALL_SEEK:
            return syscall_seek((int32_t)arg1, (int32_t)arg2, (int32_t)arg3);
            
        default:
            terminal_printf(&main_terminal, "Unknown syscall: %u\r\n", syscall_num);
            return (uint32_t)-1;
    }
}

// Implementaciones básicas
void syscall_exit(int status) {
    terminal_printf(&main_terminal, "Process %u exited with code %d\r\n",
                   scheduler.current_task ? scheduler.current_task->task_id : 0, 
                   status);
    task_exit(status);
}

int32_t syscall_write(int32_t fd, const char* buf, uint32_t count) {
    if (fd == 1 || fd == 2) { // stdout/stderr
        for (uint32_t i = 0; i < count; i++) {
            terminal_putchar(&main_terminal, buf[i]);
        }
        return count;
    }
    return -1; // Error
}

int32_t syscall_read(int32_t fd, char* buf, uint32_t count) {
    // TODO: Implementar lectura desde teclado para stdin (fd=0)
    if (fd == 0) {
        // stdin - podrías implementar lectura de teclado aquí
        return 0; // Por ahora, retornar 0 bytes
    }
    return -1; // Error
}

int32_t syscall_fork(void) {
    // TODO: Implementar fork
    return -1; // Not implemented
}

int32_t syscall_wait(int32_t* status) {
    // TODO: Implementar wait
    return -1; // Not implemented
}

void* syscall_brk(void* addr) {
    // TODO: Implementar brk para gestión de heap de usuario
    return (void*)-1; // Not implemented
}

int32_t syscall_getpid(void) {
    return scheduler.current_task ? scheduler.current_task->task_id : 0;
}

void syscall_yield(void) {
    task_yield();
}

uint32_t syscall_sleep(uint32_t ms) {
    task_sleep(ms);
    return 0;
}

uint32_t syscall_gettime(void) {
    return ticks_since_boot; // Ticks desde el boot
}

int32_t syscall_open(const char* path, int32_t flags) {
    // TODO: Implementar usando VFS
    return -1; // Not implemented
}

int32_t syscall_close(int32_t fd) {
    // TODO: Implementar usando VFS
    return -1; // Not implemented
}

int32_t syscall_seek(int32_t fd, int32_t offset, int32_t whence) {
    // TODO: Implementar seek
    return -1; // Not implemented
}