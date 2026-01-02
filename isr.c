#include "isr.h"
#include "drawing.h"
#include "kernel.h"
#include "memory.h"
#include "memutils.h"
#include "string.h"
#include "task.h"
#include "terminal.h"

// Variables globales para el estado del sistema
SystemState system_state;
uint32_t last_fault_address = 0;
uint32_t last_error_code = 0;
extern void task_switch_context(cpu_context_t *old_context,
                                cpu_context_t *new_context);

// Tabla de mensajes de excepción extendida
const char *exception_messages[] = {
    /* 0 */ "Division By Zero",
    /* 1 */ "Debug",
    /* 2 */ "Non Maskable Interrupt",
    /* 3 */ "Breakpoint",
    /* 4 */ "Into Detected Overflow",
    /* 5 */ "Out of Bounds",
    /* 6 */ "Invalid Opcode",
    /* 7 */ "No Coprocessor",
    /* 8 */ "Double Fault",
    /* 9 */ "Coprocessor Segment Overrun",
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
    /* 128 */ "System Call"};

// Función para pantalla de panic (usando drawing.h)
void panic_screen(const char *exception_msg, struct regs *r) {
  // Limpiar pantalla con fondo azul (como BSOD)
  fill_rect(0, 0, g_fb.width, g_fb.height, 0x000000FF); // Azul

  // Dibujar título
  set_colors(0xFFFFFFFF, 0x000000FF); // Blanco sobre azul
  set_font(FONT_8x16_VGA);            // Fuente más grande para visibilidad
  draw_string(20, 20, "Kernel Panic - System Halted", 0xFFFFFFFF, 0x000000FF);

  // Dibujar mensaje de excepción
  draw_string(20, 60, exception_msg, 0xFFFFFFFF, 0x000000FF);

  // Dibujar info de registros (simple)
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "EIP: 0x%08x  Error: 0x%08x", r->eip,
           r->err_code);
  draw_string(20, 100, buffer, 0xFFFFFFFF, 0x000000FF);

  // Dibujar backtrace básico
  draw_string(20, 140, "Backtrace:", 0xFFFFFFFF, 0x000000FF);
  uint32_t *frame = (uint32_t *)r->ebp;
  int y = 160;
  int max_frames = 5;
  while (frame && max_frames--) {
    uint32_t eip = frame[1];
    snprintf(buffer, sizeof(buffer), "  0x%08x", eip);
    draw_string(40, y, buffer, 0xFFFFFFFF, 0x000000FF);
    y += 20;
    frame = (uint32_t *)frame[0];
  }

  // Halt
  while (1)
    __asm__("cli; hlt");
}

// Manejadores específicos para excepciones críticas
static void handle_page_fault(struct regs *r) {
  uint32_t fault_address;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_address));

  last_fault_address = fault_address;
  last_error_code = r->err_code;

  // **DETECTAR SI ES UNA FALLA EN MODO USUARIO**
  bool user_mode = (r->cs & 0x03) == 0x03; // Usar CS, no err_code
  const char *mode = user_mode ? "User" : "Kernel";

  terminal_printf(&main_terminal, "\n*** PAGE FAULT in %s mode ***\r\n", mode);
  terminal_printf(&main_terminal, "  Fault address: 0x%08x\r\n", fault_address);
  terminal_printf(&main_terminal, "  Error code: 0x%08x\r\n", r->err_code);
  terminal_printf(&main_terminal, "  EIP: 0x%08x\r\n", r->eip);
  terminal_printf(&main_terminal, "  CS: 0x%04x (Ring %d)\r\n", r->cs,
                  r->cs & 0x03);
  terminal_printf(&main_terminal, "  Current CR3: 0x%08x\r\n",
                  mmu_get_current_cr3());

  // **SI ES EN MODO USUARIO, MATAR LA TAREA Y HACER CONTEXT SWITCH**
  if (user_mode && scheduler.current_task) {
    terminal_printf(&main_terminal, "  Terminating user task: %s\r\n",
                    scheduler.current_task->name);

    task_t *faulting_task = scheduler.current_task;

    // **1. Restaurar CR3 del kernel ANTES de modificar estructuras**
    mmu_load_cr3(mmu_get_kernel_pd());

    // **2. Marcar la tarea como muerta pero NO destruirla aún**
    faulting_task->state = TASK_ZOMBIE;

    // **3. Buscar siguiente tarea para ejecutar (no puede ser la misma)**
    task_t *next_task = scheduler.idle_task;

    if (scheduler.task_list) {
      task_t *t = scheduler.task_list;
      do {
        if (t != faulting_task && t->state == TASK_READY) {
          next_task = t;
          break;
        }
        t = t->next;
      } while (t != scheduler.task_list);
    }

    // **4. Cambiar al scheduler current_task ANTES de cualquier retorno**
    scheduler.current_task = next_task;
    next_task->state = TASK_RUNNING;

    terminal_printf(&main_terminal, "  Switching to task: %s\r\n",
                    next_task->name);

    // **5. Forzar un cambio de contexto inmediato**
    // En lugar de retornar de la interrupción, saltamos al scheduler
    task_switch_context(&faulting_task->context, &next_task->context);

    // **NUNCA LLEGAMOS AQUÍ - task_switch_to no retorna**
    __builtin_unreachable();
  }

  // Para fallas en modo kernel, hacer panic normal
  char msg[256];
  snprintf(msg, sizeof(msg),
           "Page Fault at 0x%08x\nMode: %s\nError: 0x%08x\nEIP: 0x%08x",
           fault_address, mode, r->err_code, r->eip);
  panic_screen(msg, r);
}

static void handle_general_protection_fault(struct regs *r) {
  // **DETECTAR SI ES UNA GPF EN MODO USUARIO**
  bool user_mode = (r->cs & 0x03) == 0x03;
  const char *mode = user_mode ? "User" : "Kernel";

  last_error_code = r->err_code;

  terminal_printf(&main_terminal,
                  "\n*** GENERAL PROTECTION FAULT in %s mode ***\r\n", mode);
  terminal_printf(&main_terminal, "  Error code: 0x%08x\r\n", r->err_code);
  terminal_printf(&main_terminal, "  EIP: 0x%08x\r\n", r->eip);
  terminal_printf(&main_terminal, "  CS: 0x%04x (Ring %d)\r\n", r->cs,
                  r->cs & 0x03);

  // **SI ES EN MODO USUARIO, MATAR LA TAREA Y HACER CONTEXT SWITCH**
  if (user_mode && scheduler.current_task) {
    terminal_printf(&main_terminal, "  Terminating user task: %s\r\n",
                    scheduler.current_task->name);

    task_t *faulting_task = scheduler.current_task;

    // **1. Restaurar CR3 del kernel (si es necesario)**
    mmu_load_cr3(mmu_get_kernel_pd());

    // **2. Marcar la tarea como muerta**
    faulting_task->state = TASK_ZOMBIE;

    // **3. Buscar siguiente tarea**
    task_t *next_task = scheduler.idle_task;

    if (scheduler.task_list) {
      task_t *t = scheduler.task_list;
      do {
        if (t != faulting_task && t->state == TASK_READY) {
          next_task = t;
          break;
        }
        t = t->next;
      } while (t != scheduler.task_list);
    }

    // **4. Cambiar current_task**
    scheduler.current_task = next_task;
    next_task->state = TASK_RUNNING;

    terminal_printf(&main_terminal, "  Switching to task: %s\r\n",
                    next_task->name);

    // **5. Forzar cambio de contexto**
    task_switch_context(&faulting_task->context, &next_task->context);

    // **NUNCA LLEGAMOS AQUÍ**
    __builtin_unreachable();
  }

  // **SI ES EN MODO KERNEL, HACER PANIC**
  char msg[256];
  snprintf(msg, sizeof(msg),
           "General Protection Fault\nMode: %s\nError: 0x%08x\nEIP: 0x%08x",
           mode, r->err_code, r->eip);
  panic_screen(msg, r);
}

static void handle_double_fault(struct regs *r) {
  char msg[256];
  snprintf(msg, sizeof(msg), "Double Fault\nError Code: 0x%08x\nPrevious: %s",
           r->err_code, exception_messages[system_state.last_exception]);
  panic_screen(msg, r);
}

// Función para imprimir información de registros
void print_registers(Terminal *term, struct regs *r) {
  char buffer[256];

  snprintf(
      buffer, sizeof(buffer),
      "\nRegister Dump:\n"
      "EAX: 0x%08x EBX: 0x%08x ECX: 0x%08x EDX: 0x%08x\n"
      "ESI: 0x%08x EDI: 0x%08x EBP: 0x%08x ESP: 0x%08x\n"
      "EIP: 0x%08x EFLAGS: 0x%08x\n"
      "CS: 0x%04x DS: 0x%04x ES: 0x%04x FS: 0x%04x GS: 0x%04x SS: 0x%04x\n",
      r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r->esp_fake,
      r->eip, r->eflags, r->cs, r->ds, r->es, r->fs, r->gs,
      0x10); // SS siempre es 0x10 en kernel

  terminal_puts(term, buffer);
}

// Función para imprimir backtrace
void print_backtrace(uint32_t ebp) {
  terminal_puts(&main_terminal, "Backtrace:\n");

  uint32_t *frame = (uint32_t *)ebp;
  int max_frames = 10;

  while (frame && max_frames--) {
    uint32_t eip = frame[1];
    if (eip < 0x100000)
      break; // Direcciones bajas probablemente inválidas

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "  0x%08x\n", eip);
    terminal_puts(&main_terminal, buffer);

    frame = (uint32_t *)frame[0];
    if (!frame || (uint32_t)frame < 0x100000)
      break;
  }
}

// Función para imprimir información del sistema
void print_system_state(Terminal *term) {
  char buffer[128];
  heap_info_t stats = heap_stats();
  snprintf(buffer, sizeof(buffer),
           "\nSystem State:\n"
           "Last Fault Address: 0x%08x\n"
           "Last Error Code: 0x%08x\n"
           "Memory Used: %d KB\n"
           "Tasks Running: %d\n",
           last_fault_address, last_error_code, (unsigned int)stats.used,
           system_state.task_count);

  terminal_puts(term, buffer);
}

// Función para imprimir información de la tarea actual
void print_task_info(Terminal *term) {
  // En un sistema más completo, aquí mostraríamos información de la tarea
  // actual
  terminal_puts(term, "\nCurrent Task: Kernel\n");
}

// Manejador principal de ISRs - ACTUALIZADO
void isr_handler(struct regs *r) {
  // Actualizar estado del sistema
  system_state.last_exception = r->int_no;
  system_state.exception_count++;

  // DETECTAR MODO ACTUAL (kernel o usuario)
  bool user_mode = (r->cs & 0x03) == 0x03;

  // Manejar excepciones críticas
  switch (r->int_no) {
  case 14: // Page Fault
    handle_page_fault(r);
    break;

  case 13: // General Protection Fault
    handle_general_protection_fault(r);
    break;

  case 8: // Double Fault
    handle_double_fault(r);
    break;

  case 10: // Invalid TSS
  case 11: // Segment Not Present
  case 12: // Stack Fault
    // Estas también pueden ocurrir en modo usuario
    if (user_mode && scheduler.current_task) {
      // Para modo usuario, terminar la tarea
      terminal_printf(&main_terminal,
                      "Exception %d in user mode, terminating task %s\r\n",
                      r->int_no, scheduler.current_task->name);

      // Restaurar kernel CR3
      mmu_load_cr3(mmu_get_kernel_pd());

      // Terminar tarea
      task_destroy(scheduler.current_task);
      scheduler.current_task = scheduler.idle_task;
      if (scheduler.current_task) {
        scheduler.current_task->state = TASK_RUNNING;
      }
      return;
    } else {
      // Para modo kernel, panic
      char msg[128];
      snprintf(msg, sizeof(msg), "%s\nError Code: 0x%08x",
               exception_messages[r->int_no], r->err_code);
      panic_screen(msg, r);
    }
    break;

  default:
    // Para otras excepciones: Log en terminal
    char buffer[128];
    snprintf(buffer, sizeof(buffer),
             "\nException 0x%02x (%s) in %s mode\n"
             "Error Code: 0x%08x\nEIP: 0x%08x\n",
             r->int_no,
             r->int_no < 32 ? exception_messages[r->int_no] : "Unknown",
             user_mode ? "User" : "Kernel", r->err_code, r->eip);
    terminal_puts(&main_terminal, buffer);

    // Intentar recuperación para algunas excepciones no críticas
    if (user_mode) {
      // En modo usuario, excepciones no críticas pueden terminar la tarea
      if (scheduler.current_task) {
        terminal_printf(&main_terminal, "Terminating user task: %s\r\n",
                        scheduler.current_task->name);

        mmu_load_cr3(mmu_get_kernel_pd());
        task_destroy(scheduler.current_task);
        scheduler.current_task = scheduler.idle_task;
        if (scheduler.current_task) {
          scheduler.current_task->state = TASK_RUNNING;
        }
      }
    } else if (r->int_no == 0) { // Divide by zero en modo kernel
      // Recuperación para divide by zero
      r->eax = 0;
      r->eip += 2; // Skip instrucción DIV/IDIV (2 bytes típicos)
      terminal_puts(&main_terminal,
                    "Recovered from Divide by Zero in kernel mode\n");
    } else {
      // Otras excepciones en modo kernel: continuar con cuidado
      terminal_puts(&main_terminal,
                    "Attempting to continue in kernel mode...\n");
    }
    break;
  }
}