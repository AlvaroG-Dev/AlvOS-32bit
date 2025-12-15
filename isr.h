#ifndef ISR_H
#define ISR_H

#include <stddef.h>
#include <stdint.h>
#include "terminal.h"

// Estructura para guardar el estado del sistema
typedef struct {
    uint32_t last_exception;
    uint32_t exception_count;
    uint32_t task_count;
    uint8_t critical_error;
} SystemState;

struct regs {
    // Orden inverso a los pushs, desde el último al primero
    uint32_t gs, fs, es, ds;                        // pushed manually
    uint32_t edi, esi, ebp, esp_fake, ebx, edx, ecx, eax; // pusha
    uint32_t int_no, err_code;                     // manual push or CPU
    uint32_t eip, cs, eflags;                      // pushed by CPU
};

// Funciones públicas
void isr_handler(struct regs* r);
void print_registers(Terminal* term, struct regs* r);
void print_backtrace(uint32_t ebp);
void print_system_state(Terminal* term);
void print_task_info(Terminal* term);
void panic_screen(const char* exception_msg, struct regs* r);

// Variables globales
extern const char* exception_messages[];
extern SystemState system_state;
extern uint32_t last_fault_address;
extern uint32_t last_error_code;

// CORRECTA declaración como array de direcciones, no punteros a funciones
extern uintptr_t isr_stub_table[];

#endif