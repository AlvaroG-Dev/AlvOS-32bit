// syscalls.h
#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "isr.h"
#include "types.h"  // <-- AÑADIR ESTA LÍNEA

// Números de syscall
#define SYSCALL_EXIT     0x00
#define SYSCALL_WRITE    0x01
#define SYSCALL_READ     0x02
#define SYSCALL_FORK     0x03
#define SYSCALL_EXEC     0x04
#define SYSCALL_WAIT     0x05
#define SYSCALL_BRK      0x06
#define SYSCALL_GETPID   0x07
#define SYSCALL_YIELD    0x08
#define SYSCALL_SLEEP    0x09
#define SYSCALL_GETTIME  0x0A
#define SYSCALL_OPEN     0x0B
#define SYSCALL_CLOSE    0x0C
#define SYSCALL_SEEK     0x0D

// Estructura para argumentos de syscall
typedef struct {
    uint32_t eax;   // Número de syscall
    uint32_t ebx;   // arg1
    uint32_t ecx;   // arg2
    uint32_t edx;   // arg3
    uint32_t esi;   // arg4
    uint32_t edi;   // arg5
} syscall_args_t;

// Prototipos (usar tipos correctos)
void syscall_init(void);
void syscall_handler(struct regs* r);
uint32_t syscall_dispatch(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, 
                          uint32_t arg3, uint32_t arg4, uint32_t arg5);

// Funciones específicas de syscall
void syscall_exit(int status);
int32_t syscall_write(int32_t fd, const char* buf, uint32_t count);
int32_t syscall_read(int32_t fd, char* buf, uint32_t count);
int32_t syscall_fork(void);              // <-- Cambiado de pid_t a int32_t
int32_t syscall_exec(const char* path, char* const argv[]);
int32_t syscall_wait(int32_t* status);   // <-- Cambiado de pid_t a int32_t
void* syscall_brk(void* addr);
int32_t syscall_getpid(void);            // <-- Cambiado de pid_t a int32_t
void syscall_yield(void);
uint32_t syscall_sleep(uint32_t ms);
uint32_t syscall_gettime(void);
int32_t syscall_open(const char* path, int32_t flags);
int32_t syscall_close(int32_t fd);
int32_t syscall_seek(int32_t fd, int32_t offset, int32_t whence);

#endif // SYSCALLS_H