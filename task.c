#include "task.h"
#include "gdt.h"
#include "io.h"
#include "irq.h"
#include "kernel.h"
#include "memory.h"
#include "memutils.h"
#include "mmu.h"
#include "string.h"
#include "task_utils.h"
#include "terminal.h"

// ============================================================================
// DECLARACIONES Y VARIABLES GLOBALES
// ============================================================================

task_scheduler_t scheduler = {0};

static void idle_task_func(void *arg);
static void task_wrapper(void);
static task_t *allocate_task(void);
static void deallocate_task(task_t *task);
static void add_task_to_list(task_t *task);
static void remove_task_from_list(task_t *task);

extern void task_switch_context(cpu_context_t *old_context,
                                cpu_context_t *new_context);
extern void task_start_first(cpu_context_t *context);
extern void task_switch_to_user(cpu_context_t *user_context);

// ========================================================================
// INICIALIZACIÃ“N DEL SISTEMA DE TAREAS
// ========================================================================

static void task_exit_wrapper(void) {
  // Esta funciÃ³n se llama cuando una tarea termina normalmente
  terminal_printf(&main_terminal, "[TASK_EXIT] Task %s finished normally\r\n",
                  scheduler.current_task ? scheduler.current_task->name
                                         : "unknown");
  task_exit(0);

  // Nunca deberÃ­a llegar aquÃ­
  while (1) {
    __asm__ volatile("hlt");
  }
}

static void task_entry_wrapper(void) {
  // ✅ FIX: Verificar contexto antes de ejecutar
  if (!scheduler.current_task) {
    terminal_puts(&main_terminal,
                  "ERROR: No current task in entry wrapper!\r\n");
    while (1)
      __asm__("hlt");
  }

  task_t *current = scheduler.current_task;
  void (*entry)(void *) = current->entry_point;
  void *arg = current->arg;

  // ✅ FIX: Verificar que entry sea válido
  if (!entry) {
    terminal_printf(&main_terminal,
                    "[ENTRY] ERROR: NULL entry point for %s\r\n",
                    current->name);
    task_exit(-1);
    while (1)
      __asm__("hlt"); // Nunca debería llegar aquí
  }

  terminal_printf(&main_terminal,
                  "[ENTRY] Starting %s (entry=0x%08x, arg=0x%08x)\r\n",
                  current->name, (uint32_t)entry, (uint32_t)arg);

  // ✅ Habilitar interrupciones ANTES de ejecutar
  __asm__ __volatile__("sti");

  // ✅ FIX: Ejecutar dentro de try/catch conceptual
  // (En C no hay try/catch, pero podemos usar setjmp/longjmp en futuro)
  entry(arg);

  // ✅ FIX: Si la función retorna, imprimir mensaje Y salir
  terminal_printf(&main_terminal, "[ENTRY] Task %s returned normally\r\n",
                  current->name);
  task_exit(0);

  // NUNCA debería llegar aquí
  while (1)
    __asm__("hlt");
}

static void perform_context_switch(task_t *from, task_t *to) {
  if (!from || !to)
    return;

  // CRÃTICO: Si es la misma tarea, no hacer nada
  if (from == to) {
    return;
  }

  // Guardar estado anterior SOLO si estaba RUNNING
  if (from->state == TASK_RUNNING) {
    from->state = TASK_READY;
  }

  // Nueva tarea pasa a RUNNING
  to->state = TASK_RUNNING;

  // Incrementar contadores
  from->switch_count++;
  to->switch_count++;
  scheduler.total_switches++;

  // Resetear quantum de ambas tareas
  from->time_slice = scheduler.quantum_ticks;
  to->time_slice = scheduler.quantum_ticks;

  // Actualizar tarea actual ANTES del cambio de contexto
  scheduler.current_task = to;

  // Debug cada 50 switches
  if (scheduler.total_switches % 50 == 0) {
    terminal_printf(&main_terminal, "[CTX #%u] %s -> %s\r\n",
                    scheduler.total_switches, from->name, to->name);
  }

  // CRÃTICO: Realizar cambio de contexto
  // Esta funciÃ³n debe preservar el estado del stack correctamente
  task_switch_context(&from->context, &to->context);

  // NOTA: DespuÃ©s de task_switch_context, estamos ejecutando en el contexto de
  // 'to' El cÃ³digo aquÃ­ se ejecuta cuando esta tarea vuelve a ser scheduled
}

void task_init(void) {
  memset(&scheduler, 0, sizeof(scheduler));
  scheduler.next_task_id = 1;
  scheduler.quantum_ticks = 10;
  scheduler.scheduler_enabled = false;

  terminal_printf(&main_terminal, "Task system initialized\r\n");

  // Crear tarea idle
  scheduler.idle_task =
      task_create("idle", idle_task_func, NULL, TASK_PRIORITY_HIGH);
  if (!scheduler.idle_task) {
    terminal_puts(&main_terminal, "FATAL: Failed to create idle task\r\n");
    return;
  }

  // ✅ FIX: NO asignar current_task aquí
  // El scheduler decidirá qué tarea ejecutar primero
  scheduler.current_task = NULL;

  // ✅ FIX: Idle queda en READY, esperando su turno
  scheduler.idle_task->state = TASK_READY;

  terminal_puts(&main_terminal, "Idle task created successfully\r\n");
}

// ========================================================================
// MODIFICACIÃ“N EN task_yield PARA MANEJAR MODO USUARIO
// ========================================================================

void task_yield(void) {
  if (!scheduler.scheduler_enabled || !scheduler.current_task) {
    return;
  }

  // ✅ Deshabilitar interrupciones durante el cambio
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  task_t *next = scheduler_next_task();

  // ✅ FIX: Verificar que next sea diferente de current
  if (!next || next == scheduler.current_task) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return;
  }

  task_t *from = scheduler.current_task;

  // ✅ FIX: Solo cambiar a READY si actualmente estamos RUNNING
  if (from->state == TASK_RUNNING && from != scheduler.idle_task) {
    from->state = TASK_READY;
  }

  next->state = TASK_RUNNING;
  next->time_slice = scheduler.quantum_ticks;

  from->switch_count++;
  next->switch_count++;
  scheduler.total_switches++;

  scheduler.current_task = next;

  // ✅ FIX: Switch de contexto con interrupciones deshabilitadas
  task_switch_context(&from->context, &next->context);

  // ✅ FIX: Restaurar interrupciones DESPUÉS del switch
  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}

// ========================================================================
// GESTIÃ“N DE TAREAS
// ========================================================================

task_t *task_create(const char *name, void (*entry_point)(void *), void *arg,
                    task_priority_t priority) {
  terminal_printf(&main_terminal, "[TASK_CREATE] Creating task: %s\r\n",
                  name ? name : "null");

  if (!entry_point || scheduler.task_count >= MAX_TASKS) {
    terminal_printf(&main_terminal,
                    "[TASK_CREATE] FAILED: entry=%s, count=%u\r\n",
                    entry_point ? "ok" : "NULL", scheduler.task_count);
    return NULL;
  }

  // Deshabilitar interrupciones durante la creaciÃ³n
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  task_t *task = allocate_task();
  if (!task) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return NULL;
  }

  // INICIALIZACIÃ“N COMPLETA DE TODOS LOS CAMPOS
  memset(task, 0,
         sizeof(task_t)); // Esto asegura que todos los campos estÃ©n en 0

  task->task_id = scheduler.next_task_id++;
  strncpy(task->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
  task->name[TASK_NAME_MAX - 1] = '\0';
  task->state = TASK_CREATED;
  task->priority = priority;
  task->entry_point = entry_point;
  task->arg = arg;

  // Asignar stack
  task->stack_size = TASK_STACK_SIZE;
  task->stack_base = kernel_malloc(task->stack_size);
  if (!task->stack_base) {
    deallocate_task(task);
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return NULL;
  }

  task->stack_top = (void *)((uint8_t *)task->stack_base + task->stack_size);

  // Configurar el stack y contexto inicial
  task_setup_stack(task, entry_point, arg);

  // INICIALIZAR TODOS LOS CAMPOS DE TIEMPO Y ESTADÃSTICAS
  task->time_slice = scheduler.quantum_ticks;
  task->total_runtime = 0;
  task->switch_count = 0;
  task->exit_code = 0;
  task->sleep_until = 0;
  task->wake_time = 0;

  // Inicializar punteros de lista
  task->next = NULL;
  task->prev = NULL;

  // AÃ±adir a la lista de tareas
  add_task_to_list(task);
  scheduler.task_count++;

  // La tarea estÃ¡ lista para ejecutar
  task->state = TASK_READY;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  message_queue_create(task->task_id);

  terminal_printf(&main_terminal, "Task created: %s (ID: %u)\r\n", task->name,
                  task->task_id);
  return task;
}

void task_destroy(task_t *task) {
  if (!task || task == scheduler.idle_task) {
    return; // No destruir la tarea idle
  }

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  // Si es la tarea actual, cambiar a otra
  if (task == scheduler.current_task) {
    task_yield();
  }

  // Marcar como zombie y remover de la lista
  task->state = TASK_ZOMBIE;
  remove_task_from_list(task);

  // Liberar recursos
  if (task->stack_base) {
    kernel_free(task->stack_base);
  }

  scheduler.task_count--;
  deallocate_task(task);

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}

void task_sleep(uint32_t ms) {
  if (!scheduler.current_task ||
      scheduler.current_task == scheduler.idle_task) {
    return;
  }

  uint32_t wake_tick =
      ticks_since_boot + (ms / 10); // Convertir ms a ticks (100Hz)
  scheduler.current_task->sleep_until = wake_tick;
  scheduler.current_task->state = TASK_SLEEPING;

  // terminal_printf(&main_terminal, "[SLEEP] Task %s sleeping for %u ms (until
  // tick %u)\r\n",
  //                scheduler.current_task->name, ms, wake_tick);

  // Ceder el CPU inmediatamente
  task_yield();
}

void task_exit(int exit_code) {
  if (!scheduler.current_task ||
      scheduler.current_task == scheduler.idle_task) {
    // Idle task nunca debe salir
    terminal_puts(&main_terminal, "[TASK_EXIT] Cannot exit idle task\r\n");
    return;
  }

  // ✅ FIX: Deshabilitar interrupciones para operación atómica
  __asm__ __volatile__("cli");

  // ✅ FIX: Verificar estado de manera atómica
  if (scheduler.current_task->state == TASK_FINISHED ||
      scheduler.current_task->state == TASK_ZOMBIE) {
    terminal_printf(&main_terminal,
                    "[TASK_EXIT] WARNING: %s already exited, halting\r\n",
                    scheduler.current_task->name);
    // NO retornar, en su lugar hacer halt infinito
    while (1)
      __asm__("hlt");
  }

  // Marcar como terminada
  scheduler.current_task->exit_code = exit_code;
  scheduler.current_task->state = TASK_FINISHED;

  terminal_printf(&main_terminal, "Task %s exited with code %d\r\n",
                  scheduler.current_task->name, exit_code);

  // ✅ FIX: Forzar cambio de contexto inmediato
  // NO volver a habilitar interrupciones aquí
  task_yield();

  // ✅ CRÍTICO: Si por alguna razón volvemos aquí (BUG), halt infinito
  terminal_printf(&main_terminal, "FATAL: task_exit returned for %s!\r\n",
                  scheduler.current_task->name);
  while (1)
    __asm__ __volatile__("cli; hlt");
}

// ========================================================================
// CONTROL DEL PLANIFICADOR
// ========================================================================

void scheduler_start(void) {
  if (scheduler.task_count == 0) {
    terminal_puts(&main_terminal, "No tasks to schedule\r\n");
    return;
  }

  terminal_puts(&main_terminal, "Scheduler starting...\r\n");

  // ✅ FIX: Buscar primera tarea NO-idle, o usar idle si no hay otra
  task_t *first_task = NULL;
  task_t *current = scheduler.task_list;

  if (current) {
    do {
      // Preferir cualquier tarea que NO sea idle
      if (current != scheduler.idle_task && current->state == TASK_READY) {
        first_task = current;
        break;
      }
      current = current->next;
    } while (current != scheduler.task_list);
  }

  // Si no hay otras tareas, usar idle
  if (!first_task) {
    first_task = scheduler.idle_task;
  }

  // ✅ FIX: Asegurar que todas las demás tareas estén en READY
  if (scheduler.task_list) {
    task_t *t = scheduler.task_list;
    do {
      if (t != first_task && t->state == TASK_CREATED) {
        t->state = TASK_READY;
      }
      t = t->next;
    } while (t != scheduler.task_list);
  }

  // Configurar primera tarea
  first_task->state = TASK_RUNNING;
  first_task->time_slice = scheduler.quantum_ticks;
  scheduler.current_task = first_task;
  scheduler.scheduler_enabled = true;

  terminal_printf(&main_terminal, "First task: %s (ID: %u)\r\n",
                  first_task->name, first_task->task_id);

  // Habilitar interrupciones
  __asm__ __volatile__("sti");
}

void scheduler_stop(void) { scheduler.scheduler_enabled = false; }

void scheduler_tick(void) {
  if (!scheduler.scheduler_enabled || !scheduler.current_task) {
    return;
  }

  // 1. Actualizar tareas durmientes
  task_update_sleep_states();

  // 2. Incrementar runtime de la tarea actual (si está RUNNING)
  if (scheduler.current_task->state == TASK_RUNNING) {
    scheduler.current_task->total_runtime++;
  }

  // 3. Decidir si hacer switch
  bool should_switch = false;

  // ✅ FIX: Lógica clara de cuándo hacer switch
  if (scheduler.current_task->state != TASK_RUNNING) {
    // Tarea actual no puede continuar
    should_switch = true;
  } else if (scheduler.current_task != scheduler.idle_task) {
    // Decrementar quantum de tareas normales
    if (scheduler.current_task->time_slice > 0) {
      scheduler.current_task->time_slice--;
    }

    if (scheduler.current_task->time_slice == 0) {
      // Quantum expirado
      should_switch = true;
    }
  } else {
    // Estamos en idle, verificar si hay otras tareas
    task_t *t = scheduler.task_list;
    if (t) {
      do {
        if (t != scheduler.idle_task && t->state == TASK_READY) {
          should_switch = true;
          break;
        }
        t = t->next;
      } while (t != scheduler.task_list);
    }
  }

  if (!should_switch) {
    return;
  }

  // 4. Buscar siguiente tarea
  task_t *next = scheduler_next_task();
  if (!next || next == scheduler.current_task) {
    return;
  }

  // 5. Realizar switch
  task_t *from = scheduler.current_task;

  if (from->state == TASK_RUNNING) {
    from->state = TASK_READY;
  }
  next->state = TASK_RUNNING;

  from->switch_count++;
  next->switch_count++;
  scheduler.total_switches++;

  next->time_slice = scheduler.quantum_ticks;
  scheduler.current_task = next;

  task_switch_context(&from->context, &next->context);
}

task_t *scheduler_next_task(void) {
  if (!scheduler.task_list || scheduler.task_count == 0) {
    return scheduler.idle_task;
  }

  // Empezar desde la SIGUIENTE tarea (round-robin)
  task_t *start = scheduler.current_task ? scheduler.current_task->next
                                         : scheduler.task_list;
  task_t *current = start;
  task_t *best = NULL;
  int best_priority = 99;

  // Una vuelta completa buscando la mejor tarea READY
  do {
    // Solo tareas en estado READY
    if (current->state == TASK_READY) {
      // Preferir non-idle tasks
      if (current != scheduler.idle_task) {
        // Encontramos una tarea real lista
        if (current->priority < best_priority) {
          best = current;
          best_priority = current->priority;
        }
      } else if (!best) {
        // Idle solo si no hay nada mÃ¡s
        best = current;
      }
    }

    current = current->next;
  } while (current != start);

  // Si no encontramos nada, usar idle
  if (!best) {
    return scheduler.idle_task;
  }

  return best;
}

// ========================================================================
// MODO USUARIO (RING 3)
// ========================================================================

// Este wrapper se ejecuta en Ring 0 y luego hace la transición a Ring 3
static void user_mode_entry_wrapper(void *arg) {
  (void)arg; // No usamos el argumento directamente

  task_t *current = scheduler.current_task;

  if (!current || !(current->flags & TASK_FLAG_USER_MODE)) {
    terminal_puts(&main_terminal, "[USER_WRAPPER] ERROR: Not a user task!\r\n");
    task_exit(-1);
    return;
  }

  // Debug: mostrar estado antes del switch
  terminal_printf(&main_terminal,
                  "[USER_WRAPPER] Preparing transition to Ring 3:\r\n"
                  "  Task: %s (ID: %u)\r\n"
                  "  User code: 0x%08x\r\n"
                  "  User stack: 0x%08x\r\n",
                  current->name, current->task_id,
                  (uint32_t)current->user_entry_point,
                  (uint32_t)current->user_stack_top);

  // **CRÍTICO**: Verificar mapeo de la página de código
  uint32_t code_page = (uint32_t)current->user_entry_point & ~0xFFF;

  if (!mmu_is_mapped(code_page)) {
    terminal_printf(&main_terminal,
                    "[USER_WRAPPER] ERROR: Code page not mapped at 0x%08x!\r\n",
                    code_page);

    // Intentar mapear automáticamente
    terminal_printf(&main_terminal, "  Attempting to map page 0x%08x...\r\n",
                    code_page);

    if (!mmu_map_page(code_page, code_page,
                      PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
      terminal_puts(&main_terminal, "  Mapping failed!\r\n");
      task_exit(-1);
      return;
    }

    terminal_puts(&main_terminal, "  Page mapped successfully.\r\n");

    // Flushear TLB
    __asm__ volatile("invlpg (%0)" : : "r"(code_page));
  }

  // Verificar permisos de página
  uint32_t pd_index = code_page >> 22;
  uint32_t pt_index = (code_page >> 12) & 0x3FF;

  if (page_directory[pd_index] & PAGE_PRESENT) {
    uint32_t flags = page_tables[pd_index][pt_index] & 0xFFF;
    if (!(flags & PAGE_USER)) {
      terminal_puts(&main_terminal,
                    "  Adding PAGE_USER flag to code page...\r\n");
      page_tables[pd_index][pt_index] |= PAGE_USER;
      __asm__ volatile("invlpg (%0)" : : "r"(code_page));
    }
  }

  // **CRÍTICO**: Crear contexto de usuario en tiempo de ejecución
  cpu_context_t user_ctx = {0};

  // 1. Segmentos de usuario (Ring 3) - ¡IMPORTANTE!
  user_ctx.cs = 0x1B; // User CS (Ring 3)
  user_ctx.ds = 0x23; // User DS (Ring 3) - ¡NO DEBE SER 0!
  user_ctx.es = 0x23; // ¡NO DEBE SER 0!
  user_ctx.fs = 0x23; // ¡NO DEBE SER 0!
  user_ctx.gs = 0x23; // ¡NO DEBE SER 0!
  user_ctx.ss = 0x23; // User SS (Ring 3)

  // 2. Stack y entry point
  user_ctx.esp = (uint32_t)current->user_stack_top;
  user_ctx.ebp = (uint32_t)current->user_stack_top;
  user_ctx.eip = (uint32_t)current->user_entry_point;

  // 3. EFLAGS con IF=1
  user_ctx.eflags = 0x202;

  // 4. Registros en 0
  user_ctx.eax = 0;
  user_ctx.ebx = 0;
  user_ctx.ecx = 0;
  user_ctx.edx = 0;
  user_ctx.esi = 0;
  user_ctx.edi = 0;

  // Verificar que los segmentos sean de Ring 3
  if ((user_ctx.cs & 3) != 3) {
    terminal_printf(
        &main_terminal,
        "[USER_WRAPPER] ERROR: CS=0x%04x (RPL=%u), expected RPL=3\r\n",
        user_ctx.cs, user_ctx.cs & 3);
    task_exit(-1);
    return;
  }

  if ((user_ctx.ss & 3) != 3) {
    terminal_printf(
        &main_terminal,
        "[USER_WRAPPER] ERROR: SS=0x%04x (RPL=%u), expected RPL=3\r\n",
        user_ctx.ss, user_ctx.ss & 3);
    task_exit(-1);
    return;
  }

  // Debug final con más información
  terminal_printf(&main_terminal,
                  "[USER_WRAPPER] Ready for switch:\r\n"
                  "  CS:EIP = 0x%04x:0x%08x\r\n"
                  "  SS:ESP = 0x%04x:0x%08x\r\n"
                  "  DS:ES:FS:GS = 0x%04x:0x%04x:0x%04x:0x%04x\r\n"
                  "  EFLAGS = 0x%08x\r\n",
                  user_ctx.cs, user_ctx.eip, user_ctx.ss, user_ctx.esp,
                  user_ctx.ds, user_ctx.es, user_ctx.fs, user_ctx.gs,
                  user_ctx.eflags);

  // Deshabilitar interrupciones antes del switch
  __asm__ volatile("cli");

  // Hacer la transición a Ring 3
  // Esta función NUNCA retorna
  task_switch_to_user(&user_ctx);

  // Si por algún milagro retorna, hacer halt
  terminal_puts(&main_terminal,
                "[USER_WRAPPER] FATAL: Returned from Ring 3!\r\n");
  while (1) {
    __asm__ volatile("cli; hlt");
  }
}
// ============================================================================
// FUNCIÓN PARA CREAR TAREAS DE USUARIO (COMPLETAMENTE CORREGIDA)
// ============================================================================

task_t *task_create_user(const char *name, void *user_code_addr, void *arg,
                         task_priority_t priority) {
  terminal_printf(&main_terminal,
                  "[USER_CREATE] Creating user task: %s at 0x%08x\r\n", name,
                  (uint32_t)user_code_addr);

  // 1. Validar dirección de código
  if ((uint32_t)user_code_addr < 0x200000 ||
      (uint32_t)user_code_addr >= 0xC0000000) {
    terminal_printf(&main_terminal,
                    "[USER_CREATE] ERROR: Invalid code address 0x%08x\r\n",
                    (uint32_t)user_code_addr);
    return NULL;
  }

  // 2. Verificar que el código esté mapeado con PAGE_USER
  uint32_t code_page = (uint32_t)user_code_addr & ~0xFFF;
  uint32_t code_flags = mmu_get_page_flags(code_page);

  if (!(code_flags & PAGE_PRESENT)) {
    terminal_puts(&main_terminal,
                  "[USER_CREATE] ERROR: Code page not mapped\r\n");
    return NULL;
  }

  if (!(code_flags & PAGE_USER)) {
    terminal_printf(
        &main_terminal,
        "[USER_CREATE] WARNING: Code page missing PAGE_USER (flags=0x%03x)\r\n",
        code_flags);

    // Intentar corregir
    if (!mmu_set_page_user(code_page)) {
      terminal_puts(
          &main_terminal,
          "[USER_CREATE] ERROR: Cannot set PAGE_USER on code page\r\n");
      return NULL;
    }

    terminal_puts(&main_terminal,
                  "[USER_CREATE] PAGE_USER added to code page\r\n");
  }

  // 3. Asignar stack de usuario
  void *user_stack = kernel_malloc(USER_STACK_SIZE);
  if (!user_stack) {
    terminal_puts(&main_terminal,
                  "[USER_CREATE] ERROR: Cannot allocate user stack\r\n");
    return NULL;
  }
  memset(user_stack, 0, USER_STACK_SIZE);

  // 4. **CRÍTICO**: Mapear CADA PÁGINA del stack con PAGE_USER
  uint32_t stack_start = (uint32_t)user_stack;
  uint32_t stack_end = stack_start + USER_STACK_SIZE;

  terminal_printf(&main_terminal,
                  "[USER_CREATE] Mapping user stack: 0x%08x - 0x%08x\r\n",
                  stack_start, stack_end);

  for (uint32_t page = stack_start; page < stack_end; page += 0x1000) {
    uint32_t phys = mmu_virtual_to_physical(page);

    if (!phys) {
      // No está mapeada, mapear ahora
      if (!mmu_map_page(page, page, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
        terminal_printf(
            &main_terminal,
            "[USER_CREATE] ERROR: Failed to map stack page 0x%08x\r\n", page);
        kernel_free(user_stack);
        return NULL;
      }
    } else {
      // Ya está mapeada, asegurar PAGE_USER
      if (!mmu_set_page_user(page)) {
        terminal_printf(&main_terminal,
                        "[USER_CREATE] ERROR: Failed to set PAGE_USER on stack "
                        "page 0x%08x\r\n",
                        page);
        kernel_free(user_stack);
        return NULL;
      }
    }
  }

  // 5. Calcular top del stack (alineado a 16 bytes)
  uint32_t stack_top = stack_end & ~0xF;

  // 6. Crear tarea usando el wrapper de kernel
  task_t *task = task_create(name, user_mode_entry_wrapper, arg, priority);
  if (!task) {
    terminal_puts(&main_terminal,
                  "[USER_CREATE] ERROR: task_create() failed\r\n");
    kernel_free(user_stack);
    return NULL;
  }

  // 7. Configurar información de usuario
  task->user_stack_base = user_stack;
  task->user_stack_top = (void *)stack_top;
  task->user_stack_size = USER_STACK_SIZE;
  task->user_entry_point = user_code_addr;
  task->user_code_base = user_code_addr;
  task->user_code_size = 4096;
  task->flags |= TASK_FLAG_USER_MODE;

  // 8. Verificar que el contexto de kernel esté correcto
  if (task->context.eip != (uint32_t)user_mode_entry_wrapper) {
    terminal_printf(&main_terminal,
                    "[USER_CREATE] WARNING: Fixing EIP: 0x%08x -> 0x%08x\r\n",
                    task->context.eip, (uint32_t)user_mode_entry_wrapper);
    task->context.eip = (uint32_t)user_mode_entry_wrapper;
  }

  terminal_printf(&main_terminal,
                  "[USER_CREATE] User task created successfully!\r\n"
                  "  Task ID: %u\r\n"
                  "  Kernel entry (wrapper): 0x%08x\r\n"
                  "  User entry point: 0x%08x\r\n"
                  "  Kernel stack: 0x%08x -> 0x%08x\r\n"
                  "  User stack: 0x%08x -> 0x%08x\r\n"
                  "  Kernel segments: CS=0x%04x, DS=0x%04x, SS=0x%04x\r\n"
                  "  User segments: CS=0x%04x, DS=0x%04x, SS=0x%04x\r\n",
                  task->task_id, (uint32_t)user_mode_entry_wrapper,
                  (uint32_t)user_code_addr, (uint32_t)task->stack_base,
                  (uint32_t)task->stack_top, (uint32_t)user_stack, stack_top,
                  task->context.cs, task->context.ds, task->context.ss, 0x1B,
                  0x23, 0x23);

  return task;
}

// ========================================================================
// FUNCIONES DE INFORMACIÃ“N
// ========================================================================

task_t *task_current(void) { return scheduler.current_task; }

task_t *task_find_by_id(uint32_t task_id) {
  if (!scheduler.task_list)
    return NULL;

  task_t *current = scheduler.task_list;
  do {
    if (current->task_id == task_id) {
      return current;
    }
    current = current->next;
  } while (current != scheduler.task_list);

  return NULL;
}

task_t *task_find_by_name(const char *name) {
  if (!scheduler.task_list || !name)
    return NULL;

  task_t *current = scheduler.task_list;
  do {
    if (strncmp(current->name, name, TASK_NAME_MAX) == 0) {
      return current;
    }
    current = current->next;
  } while (current != scheduler.task_list);

  return NULL;
}

void task_list_all(void) {
  terminal_puts(&main_terminal, "\r\n=== Task List ===\r\n");
  terminal_printf(&main_terminal, "Current: %s (ID: %u)\r\n",
                  scheduler.current_task ? scheduler.current_task->name
                                         : "none",
                  scheduler.current_task ? scheduler.current_task->task_id : 0);
  terminal_printf(&main_terminal, "Total tasks: %u\r\n", scheduler.task_count);
  terminal_printf(&main_terminal, "Total switches: %u\r\n\r\n",
                  scheduler.total_switches);

  if (!scheduler.task_list) {
    terminal_puts(&main_terminal, "No tasks\r\n");
    return;
  }

  task_t *current = scheduler.task_list;
  do {
    const char *state_names[] = {"CREATED", "RUNNING",  "READY", "SLEEPING",
                                 "WAITING", "FINISHED", "ZOMBIE"};

    terminal_printf(
        &main_terminal,
        "ID: %2u | %-12s | %-9s | Pri: %u | Switches: %4u | Runtime: %6u\r\n",
        current->task_id, current->name, state_names[current->state],
        current->priority, current->switch_count, current->total_runtime);

    current = current->next;
  } while (current != scheduler.task_list);

  terminal_puts(&main_terminal, "\r\n");
}

// ========================================================================
// FUNCIONES AUXILIARES
// ========================================================================

void task_setup_stack(task_t *task, void (*entry_point)(void *), void *arg) {
  memset(task->stack_base, 0xAA, task->stack_size);

  uint8_t *stack_end = (uint8_t *)task->stack_base + task->stack_size;
  uint32_t *stack_ptr = (uint32_t *)stack_end;

  // ✅ FIX: Alineación estricta a 16 bytes
  stack_ptr = (uint32_t *)((uintptr_t)stack_ptr & ~0xF);

  // ✅ FIX: Verificar alineación
  if ((uintptr_t)stack_ptr & 0xF) {
    terminal_printf(&main_terminal,
                    "[STACK] ERROR: Stack not aligned for %s\r\n", task->name);
    return;
  }

  // Canary
  *(--stack_ptr) = 0xDEADBEEF;

  // ✅ FIX: Contexto apuntando al wrapper
  task->context.eip = (uint32_t)task_entry_wrapper;
  task->context.esp = (uint32_t)stack_ptr;
  task->context.ebp = (uint32_t)stack_ptr;

  // Registros en cero
  task->context.eax = 0;
  task->context.ebx = 0;
  task->context.ecx = 0;
  task->context.edx = 0;
  task->context.esi = 0;
  task->context.edi = 0;

  // Segmentos de kernel
  task->context.cs = 0x08;
  task->context.ds = 0x10;
  task->context.es = 0x10;
  task->context.fs = 0x10;
  task->context.gs = 0x10;
  task->context.ss = 0x10;

  // ✅ FIX: EFLAGS con IF=0 (se habilitará con IRET)
  // Esto evita race conditions durante la inicialización
  task->context.eflags = 0x200;

  terminal_printf(
      &main_terminal, "[STACK] %s: ESP=0x%08x (aligned=%s) EIP=0x%08x\r\n",
      task->name, task->context.esp,
      ((uint32_t)stack_ptr & 0xF) == 0 ? "YES" : "NO", task->context.eip);
}

bool task_is_ready(task_t *task) {
  if (!task)
    return false;

  switch (task->state) {
  case TASK_READY:
    return true;

  case TASK_RUNNING:
    // RUNNING no estÃ¡ "lista", estÃ¡ ejecutÃ¡ndose
    return false;

  case TASK_SLEEPING:
    // Verificar si ya es hora de despertar
    if (ticks_since_boot >= task->sleep_until) {
      task->state = TASK_READY;
      return true;
    }
    return false;

  case TASK_CREATED:
  case TASK_WAITING:
  case TASK_FINISHED:
  case TASK_ZOMBIE:
  default:
    return false;
  }
}

void task_update_sleep_states(void) {
  if (!scheduler.task_list)
    return;

  task_t *current = scheduler.task_list;
  do {
    if (current->state == TASK_SLEEPING &&
        ticks_since_boot >= current->sleep_until) {
      current->state = TASK_READY;
      // terminal_printf(&main_terminal, "[SLEEP] Task %s woke up\r\n",
      // current->name);
    }
    current = current->next;
  } while (current != scheduler.task_list);
}

// ========================================================================
// FUNCIONES AUXILIARES INTERNAS
// ========================================================================

static task_t *allocate_task(void) {
  task_t *task = (task_t *)kernel_malloc(sizeof(task_t));
  if (task) {
    memset(task, 0, sizeof(task_t));
  }
  return task;
}

static void deallocate_task(task_t *task) {
  if (task) {
    kernel_free(task);
  }
}

static void add_task_to_list(task_t *task) {
  if (!task)
    return;

  if (!scheduler.task_list) {
    // Primera tarea
    scheduler.task_list = task;
    task->next = task;
    task->prev = task;
  } else {
    // Insertar al final de la lista circular
    task_t *last = scheduler.task_list->prev;

    task->next = scheduler.task_list;
    task->prev = last;
    last->next = task;
    scheduler.task_list->prev = task;
  }
}

static void remove_task_from_list(task_t *task) {
  if (!task || !scheduler.task_list)
    return;

  if (task->next == task) {
    // Ãšnica tarea en la lista
    scheduler.task_list = NULL;
  } else {
    // Remover de la lista circular
    task->prev->next = task->next;
    task->next->prev = task->prev;

    // Si era el head, mover el head
    if (scheduler.task_list == task) {
      scheduler.task_list = task->next;
    }
  }

  task->next = NULL;
  task->prev = NULL;
}

static void task_wrapper(void) {
  // Esta funciÃ³n se llama cuando una tarea kernel termina normalmente
  if (scheduler.current_task) {
    terminal_printf(&main_terminal, "Kernel task %s finished normally\r\n",
                    scheduler.current_task->name);
    task_exit(0);
  }
}

static void idle_task_func(void *arg) {
  (void)arg;

  terminal_printf(&main_terminal, "[IDLE] Task started\r\n");

  uint32_t last_yield = 0;

  while (1) {
    // HLT para ahorrar energÃ­a
    __asm__ volatile("hlt");

    // Ceder el CPU cada 10 ticks si no hay otras tareas
    if (ticks_since_boot - last_yield > 10) {
      last_yield = ticks_since_boot;

      // Verificar si hay otras tareas listas
      task_t *current = scheduler.task_list;
      bool other_tasks_ready = false;

      if (current) {
        do {
          if (current != scheduler.idle_task && task_is_ready(current)) {
            other_tasks_ready = true;
            break;
          }
          current = current->next;
        } while (current != scheduler.task_list);
      }

      // Si hay otras tareas listas, ceder el CPU
      if (other_tasks_ready) {
        task_yield();
      }
    }
  }
}

// FunciÃ³n para mostrar estadÃ­sticas detalladas del sistema
void show_system_stats(void) {
  terminal_puts(&main_terminal, "\r\n=== System Statistics ===\r\n");

  // EstadÃ­sticas del planificador
  terminal_printf(&main_terminal, "Scheduler enabled: %s\r\n",
                  scheduler.scheduler_enabled ? "YES" : "NO");
  terminal_printf(&main_terminal, "Total tasks: %u (max: %u)\r\n",
                  scheduler.task_count, MAX_TASKS);
  terminal_printf(&main_terminal, "Total context switches: %u\r\n",
                  scheduler.total_switches);
  terminal_printf(&main_terminal, "Current task: %s (ID: %u)\r\n",
                  scheduler.current_task ? scheduler.current_task->name
                                         : "none",
                  scheduler.current_task ? scheduler.current_task->task_id : 0);

  // EstadÃ­sticas de memoria
  heap_info_t heap_info = heap_stats();
  terminal_printf(&main_terminal, "Heap used: %u bytes\r\n", heap_info.used);
  terminal_printf(&main_terminal, "Heap free: %u bytes\r\n", heap_info.free);
  terminal_printf(&main_terminal, "Largest free block: %u bytes\r\n",
                  heap_info.largest_free_block);

  // Tiempo del sistema
  terminal_printf(&main_terminal, "System uptime: %u ticks (%u seconds)\r\n",
                  ticks_since_boot, ticks_since_boot / 100);

  terminal_puts(&main_terminal, "\r\n");
}

void cleanup_task(void *arg) {
  (void)arg;

  while (1) {
    task_cleanup_zombies();

    // âœ… CAMBIO: 200ms en lugar de 1 segundo
    task_sleep(200);

    // Verificar heap periÃ³dicamente
    static uint32_t cleanup_count = 0;
    if (++cleanup_count % 50 == 0) { // Cada 10 segundos
      heap_info_t info = heap_stats();
      if (info.used > (STATIC_HEAP_SIZE * 0.8)) {
        terminal_printf(&main_terminal, "[CLEANUP] High memory usage: %u%%\r\n",
                        (info.used * 100) / STATIC_HEAP_SIZE);
      }
    }

    task_yield();
  }
}