#include "isr.h"
#include "string.h"
#include "terminal.h"
#include "memutils.h"
#include "drawing.h"
#include "kernel.h"
#include "memory.h"
#include "task.h"

// Variables globales para el estado del sistema
SystemState system_state;
uint32_t last_fault_address = 0;
uint32_t last_error_code = 0;

// Tabla de mensajes de excepción extendida
const char* exception_messages[] = {
    /* 0 */  "Division By Zero",
    /* 1 */  "Debug",
    /* 2 */  "Non Maskable Interrupt",
    /* 3 */  "Breakpoint",
    /* 4 */  "Into Detected Overflow",
    /* 5 */  "Out of Bounds",
    /* 6 */  "Invalid Opcode",
    /* 7 */  "No Coprocessor",
    /* 8 */  "Double Fault",
    /* 9 */  "Coprocessor Segment Overrun",
    /* 10 */ "Bad TSS",
    /* 11 */ "Segment Not Present",
    /* 12 */ "Stack Fault",
    /* 13 */ "General Protection Fault",
    /* 14 */ "Page Fault",
    /* 15 */ "Unknown Interrupt",
    /* 16 */ "Coprocessor Fault",
    /* 17 */ "Alignment Check",
    /* 18 */ "Machine Check",
    /* 19 */ "Reserved",
    /* 20 */ "Reserved",
    /* 21 */ "Reserved",
    /* 22 */ "Reserved",
    /* 23 */ "Reserved",
    /* 24 */ "Reserved",
    /* 25 */ "Reserved",
    /* 26 */ "Reserved",
    /* 27 */ "Reserved",
    /* 28 */ "Reserved",
    /* 29 */ "Reserved",
    /* 30 */ "Reserved",
    /* 31 */ "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    /* 128 */ "System Call"
};

// Función para pantalla de panic (usando drawing.h)
void panic_screen(const char* exception_msg, struct regs* r) {
    // Limpiar pantalla con fondo azul (como BSOD)
    fill_rect(0, 0, g_fb.width, g_fb.height, 0x000000FF);  // Azul
    
    // Dibujar título
    set_colors(0xFFFFFFFF, 0x000000FF);  // Blanco sobre azul
    set_font(FONT_8x16_VGA);  // Fuente más grande para visibilidad
    draw_string(20, 20, "Kernel Panic - System Halted", 0xFFFFFFFF, 0x000000FF);
    
    // Dibujar mensaje de excepción
    draw_string(20, 60, exception_msg, 0xFFFFFFFF, 0x000000FF);
    
    // Dibujar info de registros (simple)
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "EIP: 0x%08x  Error: 0x%08x", r->eip, r->err_code);
    draw_string(20, 100, buffer, 0xFFFFFFFF, 0x000000FF);
    
    // Dibujar backtrace básico
    draw_string(20, 140, "Backtrace:", 0xFFFFFFFF, 0x000000FF);
    uint32_t* frame = (uint32_t*)r->ebp;
    int y = 160;
    int max_frames = 5;
    while (frame && max_frames--) {
        uint32_t eip = frame[1];
        snprintf(buffer, sizeof(buffer), "  0x%08x", eip);
        draw_string(40, y, buffer, 0xFFFFFFFF, 0x000000FF);
        y += 20;
        frame = (uint32_t*)frame[0];
    }
    
    // Halt
    while(1) __asm__("cli; hlt");
}

// Manejadores específicos para excepciones críticas
static void handle_page_fault(struct regs* r) {
    uint32_t fault_address;
    __asm__ __volatile__("mov %%cr2, %0" : "=r" (fault_address));
    
    last_fault_address = fault_address;
    last_error_code = r->err_code;
    
    // **DETECTAR SI ES UNA FALLA EN MODO USUARIO**
    bool user_mode = (r->err_code & 0x4) ? true : false;
    const char* mode = user_mode ? "User" : "Kernel";
    
    terminal_printf(&main_terminal, "\n*** PAGE FAULT in %s mode ***\r\n", mode);
    terminal_printf(&main_terminal, "  Fault address: 0x%08x\r\n", fault_address);
    terminal_printf(&main_terminal, "  Error code: 0x%08x\r\n", r->err_code);
    terminal_printf(&main_terminal, "  EIP: 0x%08x\r\n", r->eip);
    terminal_printf(&main_terminal, "  Current CR3: 0x%08x\r\n", mmu_get_current_cr3());
    
    // **SI ES EN MODO USUARIO, MATAR LA TAREA Y RESTAURAR KERNEL**
    if (user_mode && scheduler.current_task) {
        terminal_printf(&main_terminal, "  Terminating user task: %s\r\n", scheduler.current_task->name);
        
        // **RESTAURAR CR3 DEL KERNEL INMEDIATAMENTE**
        mmu_load_cr3(mmu_get_kernel_pd());
        terminal_printf(&main_terminal, "  Restored kernel CR3: 0x%08x\r\n", mmu_get_current_cr3());
        
        // Terminar la tarea
        task_destroy(scheduler.current_task);
        
        // Volver a la tarea idle
        scheduler.current_task = scheduler.idle_task;
        if (scheduler.current_task) {
            scheduler.current_task->state = TASK_RUNNING;
        }
        
        terminal_printf(&main_terminal, "  Returned to kernel mode\r\n");
        return; // NO hacer panic, solo terminar la tarea
    }
    
    // Para fallas en modo kernel, hacer panic normal
    char msg[256];
    snprintf(msg, sizeof(msg), 
             "Page Fault at 0x%08x\nMode: %s\nError: 0x%08x",
             fault_address, mode, r->err_code);
    panic_screen(msg, r);
}

static void handle_general_protection_fault(struct regs* r) {
    char msg[256];
    snprintf(msg, sizeof(msg), 
             "General Protection Fault\nError Code: 0x%08x\nEIP: 0x%08x",
             r->err_code, r->eip);
    panic_screen(msg, r);
}

static void handle_double_fault(struct regs* r) {
    char msg[256];
    snprintf(msg, sizeof(msg), 
             "Double Fault\nError Code: 0x%08x\nPrevious: %s",
             r->err_code, exception_messages[system_state.last_exception]);
    panic_screen(msg, r);
}

// Función para imprimir información de registros
void print_registers(Terminal* term, struct regs* r) {
    char buffer[256];
    
    snprintf(buffer, sizeof(buffer),
             "\nRegister Dump:\n"
             "EAX: 0x%08x EBX: 0x%08x ECX: 0x%08x EDX: 0x%08x\n"
             "ESI: 0x%08x EDI: 0x%08x EBP: 0x%08x ESP: 0x%08x\n"
             "EIP: 0x%08x EFLAGS: 0x%08x\n"
             "CS: 0x%04x DS: 0x%04x ES: 0x%04x FS: 0x%04x GS: 0x%04x SS: 0x%04x\n",
             r->eax, r->ebx, r->ecx, r->edx,
             r->esi, r->edi, r->ebp, r->esp_fake,
             r->eip, r->eflags,
             r->cs, r->ds, r->es, r->fs, r->gs, 0x10); // SS siempre es 0x10 en kernel
    
    terminal_puts(term, buffer);
}

// Función para imprimir backtrace
void print_backtrace(uint32_t ebp) {
    terminal_puts(&main_terminal, "Backtrace:\n");
    
    uint32_t* frame = (uint32_t*)ebp;
    int max_frames = 10;
    
    while (frame && max_frames--) {
        uint32_t eip = frame[1];
        if (eip < 0x100000) break; // Direcciones bajas probablemente inválidas
        
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "  0x%08x\n", eip);
        terminal_puts(&main_terminal, buffer);
        
        frame = (uint32_t*)frame[0];
        if (!frame || (uint32_t)frame < 0x100000) break;
    }
}

// Función para imprimir información del sistema
void print_system_state(Terminal* term) {
    char buffer[128];
    heap_info_t stats = heap_stats();
    snprintf(buffer, sizeof(buffer),
             "\nSystem State:\n"
             "Last Fault Address: 0x%08x\n"
             "Last Error Code: 0x%08x\n"
             "Memory Used: %d KB\n"
             "Tasks Running: %d\n",
             last_fault_address, last_error_code,
             (unsigned int)stats.used,
             system_state.task_count);
    
    terminal_puts(term, buffer);
}

// Función para imprimir información de la tarea actual
void print_task_info(Terminal* term) {
    // En un sistema más completo, aquí mostraríamos información de la tarea actual
    terminal_puts(term, "\nCurrent Task: Kernel\n");
}

// Manejador principal de ISRs
void isr_handler(struct regs* r) {
    // Actualizar estado del sistema
    system_state.last_exception = r->int_no;
    system_state.exception_count++;

    int is_critical = (r->int_no == 8 || r->int_no == 10 || r->int_no == 11 || 
                       r->int_no == 12 || r->int_no == 13 || r->int_no == 14);
    
    if (is_critical) {
        // Manejar críticas con handlers específicos y panic screen
        switch (r->int_no) {
            case 14: handle_page_fault(r); break;
            case 13: handle_general_protection_fault(r); break;
            case 8: handle_double_fault(r); break;
            default: {
                char msg[128];
                snprintf(msg, sizeof(msg), "%s\nError Code: 0x%08x", 
                         exception_messages[r->int_no], r->err_code);
                panic_screen(msg, r);
            }
        }
    } else {
        // Para no críticas: Log en terminal e intentar recuperación
        char buffer[128];
        snprintf(buffer, sizeof(buffer),
                 "\n\nException 0x%02x (%s) occurred\n"
                 "Error Code: 0x%08x\n"
                 "EIP: 0x%08x\n",
                 r->int_no, 
                 r->int_no < 32 ? exception_messages[r->int_no] : "Unknown",
                 r->err_code,
                 r->eip);
        terminal_puts(&main_terminal, buffer);
        
        print_registers(&main_terminal, r);
        
        // Intentar recuperación: Por ejemplo, para divide by zero (0), setear eax=0 y ajustar eip para skip
        if (r->int_no == 0) {  // Divide by zero
            r->eax = 0;  // Setear resultado a 0
            r->eip += 2;  // Skip la instrucción (asumiendo div simple, ajusta según código)
            terminal_puts(&main_terminal, "Recovered from Divide by Zero by setting result to 0\n");
            return;  // Continuar ejecución
        }
        
        // Para otras, mostrar y continuar (puede causar loops)
        terminal_puts(&main_terminal, "\nAttempting to continue...\n");
    }
}