#include "task.h"
#include "exec.h"
#include "irq.h"
#include "kernel.h"
#include "memory.h"
#include "mmu.h"
#include "pmm.h"
#include "serial.h"
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
  if (scheduler.current_task &&
      !(scheduler.current_task->flags & TASK_FLAG_QUIET)) {
    serial_printf(COM1_BASE, "[TASK_EXIT] Task %s finished normally\r\n",
                  scheduler.current_task->name);
  }
  task_exit(0);

  // Nunca deberÃ­a llegar aquÃ­
  while (1) {
    __asm__ volatile("hlt");
  }
}

static void task_entry_wrapper(void) {
  // ✅ FIX: Verificar contexto antes de ejecutar
  if (!scheduler.current_task) {
    serial_printf(COM1_BASE, "ERROR: No current task in entry wrapper!\r\n");
    while (1)
      __asm__("hlt");
  }

  task_t *current = scheduler.current_task;
  void (*entry)(void *) = current->entry_point;
  void *arg = current->arg;

  // ✅ FIX: Verificar que entry sea válido
  if (!(current->flags & TASK_FLAG_QUIET)) {
    if (!entry) {
      serial_printf(COM1_BASE, "[ENTRY] ERROR: NULL entry point for %s\r\n",
                    current->name);
      task_exit(-1);
      while (1)
        __asm__("hlt"); // Nunca debería llegar aquí
    }

    serial_printf(COM1_BASE,
                  "[ENTRY] Starting %s (entry=0x%08x, arg=0x%08x)\r\n",
                  current->name, (uint32_t)entry, (uint32_t)arg);
  }

  // ✅ Habilitar interrupciones ANTES de ejecutar
  __asm__ __volatile__("sti");

  // ✅ FIX: Ejecutar dentro de try/catch conceptual
  // (En C no hay try/catch, pero podemos usar setjmp/longjmp en futuro)
  entry(arg);

  // ✅ FIX: Si la función retorna, imprimir mensaje Y salir
  if (!(current->flags & TASK_FLAG_QUIET)) {
    serial_printf(COM1_BASE, "[ENTRY] Task %s returned normally\r\n",
                  current->name);
  }
  task_exit(0);

  // NUNCA debería llegar aquí
  while (1)
    __asm__("hlt");
}

static void perform_context_switch(task_t *from, task_t *to) {
  if (!from || !to || from == to)
    return;

  // 1. Guardar estado transitorio
  if (from->state == TASK_RUNNING) {
    from->state = TASK_READY;
  }
  to->state = TASK_RUNNING;

  // 2. Actualizar metadatos
  from->switch_count++;
  to->switch_count++;
  scheduler.total_switches++;
  scheduler.current_task = to;
  to->time_slice = scheduler.quantum_ticks;

  // 3. CAMBIO DE ESPACIO DE DIRECCIONES (CR3)
  if (to->address_space && to->address_space->page_directory) {
    // SINCRONIZAR mapeos del kernel al PD de usuario.
    // Si el kernel creó nuevas page tables después de que este proceso
    // fue creado, deben ser visibles para el proceso ahora.
    uint32_t user_pd_phys = to->address_space->page_directory;
    uint32_t *user_pd = (uint32_t *)(KERNEL_VIRTUAL_BASE + user_pd_phys);
    uint32_t *kernel_pd = page_directory;

    for (int i = 0; i < 768; i++) {
      if ((kernel_pd[i] & PAGE_PRESENT) && !(user_pd[i] & PAGE_PRESENT)) {
        user_pd[i] = kernel_pd[i] & ~PAGE_USER; // Supervisor-only
      }
    }
    for (int i = 768; i < 1024; i++) {
      if (kernel_pd[i] != user_pd[i]) {
        user_pd[i] = kernel_pd[i];
      }
    }

    mmu_load_cr3(user_pd_phys);
  } else {
    mmu_load_cr3(mmu_get_kernel_pd());
  }

  // 4. Salto final al nuevo contexto
  task_switch_context(&from->context, &to->context);
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


  // Guardar flags y deshabilitar interrupciones
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  task_t *from = scheduler.current_task;
  task_t *next = scheduler_next_task();

  if (next && next != from) {
    perform_context_switch(from, next);
  }

  // Restaurar interrupciones
  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}



task_t *task_create(const char *name, void (*entry_point)(void *), void *arg,
                    task_priority_t priority) {

  if (!entry_point || scheduler.task_count >= MAX_TASKS) {
    if (exec_verbose) {
      serial_printf(COM1_BASE, "[TASK_CREATE] FAILED: entry=%s, count=%u\r\n",
                    entry_point ? "ok" : "NULL", scheduler.task_count);
    }
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

  if (!exec_verbose) {
    task->flags |= TASK_FLAG_QUIET;
  }

  if (!(task->flags & TASK_FLAG_QUIET)) {
    serial_printf(COM1_BASE, "[TASK_CREATE] Creating task: %s\r\n",
                  name ? name : "null");
  }

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

  // Inicializar CWD (heredar del padre si existe)
  if (scheduler.current_task) {
    strncpy(task->cwd, scheduler.current_task->cwd, 255);
    task->cwd[255] = '\0';
  } else {
    strcpy(task->cwd, "/home");
  }
  task->wait_for_pid = 0;
  task->has_waited = 0;
  task->parent = scheduler.current_task;

  // La tarea estÃ¡ lista para ejecutar
  task->state = TASK_READY;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  message_queue_create(task->task_id);

  if (exec_verbose) {
    serial_printf(COM1_BASE, "Task created: %s (ID: %u)\r\n", task->name,
                  task->task_id);
  }
  return task;
}

void task_destroy(task_t *task) {
  if (!task || task == scheduler.idle_task) {
    return; // No destruir la tarea idle
  }

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  // Si es la tarea actual, debemos manejar esto con cuidado
  if (task == scheduler.current_task) {
    // CAMBIO CRITICO: No podemos liberar nuestra propia memoria mientras
    // corremos en ella Marcar como ZOMBIE y ceder CPU para siempre. El
    // recolector (idle/cleanup) nos limpiará.
    task->state = TASK_ZOMBIE;

    // Deshabilitar interrupciones para el yield final
    __asm__ __volatile__("cli");
    task_yield();

    // Código inalcanzable si el scheduler funciona bien
    while (1)
      __asm__("hlt");
  }

  // Marcar como zombie y remover de la lista
  task->state = TASK_ZOMBIE;
  remove_task_from_list(task);

  // Liberar recursos
  if (task->stack_base) {
    kernel_free(task->stack_base);
  }

  // Liberar recursos de usuario (aislamiento)
  if (task->user_stack_phys) {
    pmm_free_pages(task->user_stack_phys, task->user_stack_size / PAGE_SIZE);
    task->user_stack_phys = NULL;
  }

  // Si la tarea tiene un espacio de direcciones privado (aislado), destruirlo
  // El VMM se encargará de liberar el directorio de páginas y las regiones.
  if (task->address_space && task->address_space != &kernel_address_space) {
    vmm_destroy_address_space(task->address_space);
    task->address_space = NULL;
  }

  // Liberar descriptores de archivo abiertos
  for (int i = 0; i < VFS_MAX_FDS; i++) {
    if (task->fd_table[i] != NULL) {
      // vfs_close se encarga de liberar la estructura socket o file
      // Nota: Necesitamos asegurarnos de que vfs_close pueda manejar esto sin
      // el contexto de la tarea actual si estamos destruyendo OTRA tarea.
      // vfs_close usa current_task->fd_table para buscar, pero aquí tenemos el
      // puntero directo. Podríamos llamar directamente a file->ops->close(file)
      // y luego liberar file.

      vfs_file_t *file = task->fd_table[i];
      if (file && (uint32_t)file != 0x1) {
        // Si tenemos VFS_NODE_SOCKET, vfs_close (si lo actualizamos) lo
        // manejará Pero como vfs_close toma un INT fd y busca en
        // current_task... Necesitamos una función interna o hacerlo manualmente
        // aquí.

        // Por seguridad, llamamos a vfs_file_close_internal(file) si existiera,
        // o replicamos la lógica:

        if (file->ops && file->ops->close) {
          file->ops->close(file);
        }

        // Decrementar refcount del nodo si existe
        if (file->node) {
          file->node->refcount--;
          if (file->node->refcount == 0 && file->node->ops &&
              file->node->ops->release) {
            file->node->ops->release(file->node);
          }
        }

        kernel_free(file);
        task->fd_table[i] = NULL;
      }
    }
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

  // ✅ FIX: Asegurar al menos 1 tick de sueño y redondear hacia arriba
  uint32_t ticks_to_sleep = (ms + 9) / 10;
  if (ticks_to_sleep == 0)
    ticks_to_sleep = 1;

  uint32_t wake_tick = ticks_since_boot + ticks_to_sleep;
  scheduler.current_task->sleep_until = wake_tick;
  scheduler.current_task->state = TASK_SLEEPING;

  // Ceder el CPU inmediatamente
  task_yield();
}

void task_terminate(task_t *task, int exit_code) {
  if (!task || task == scheduler.idle_task)
    return;

  unsigned int f = vfs_lock_disable_irq();

  if (task->state == TASK_FINISHED || task->state == TASK_ZOMBIE) {
    vfs_unlock_restore_irq(f);
    return;
  }

  task->exit_code = exit_code;
  task->state = TASK_FINISHED;

  // Despertar al padre si estaba esperando
  if (task->parent) {
    task_t *p = task->parent;
    if (p->state == TASK_WAITING && p->wait_for_pid == task->task_id) {
      p->state = TASK_READY;
      p->wait_for_pid = 0;
    }
  }

  if (!(task->flags & TASK_FLAG_QUIET)) {
    serial_printf(COM1_BASE, "Task %s terminated with code %d\r\n", task->name,
                  exit_code);
  }

  vfs_unlock_restore_irq(f);
}

void task_exit(int exit_code) {
  if (!scheduler.current_task ||
      scheduler.current_task == scheduler.idle_task) {
    terminal_puts(&main_terminal, "[TASK_EXIT] Cannot exit idle task\r\n");
    return;
  }

  task_terminate(scheduler.current_task, exit_code);

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

  // 5. Realizar switch unificado (maneja CR3 y estados)
  task_t *from = scheduler.current_task;
  perform_context_switch(from, next);
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
  if (!(current->flags & TASK_FLAG_QUIET)) {
    serial_printf(COM1_BASE,
                  "[USER_WRAPPER] Preparing transition to Ring 3:\r\n"
                  "  Task: %s (ID: %u)\r\n"
                  "  User code: 0x%08x\r\n"
                  "  User stack: 0x%08x\r\n",
                  current->name, current->task_id,
                  (uint32_t)current->user_entry_point,
                  (uint32_t)current->user_stack_top);
  }

  // **CRÍTICO**: Verificar mapeo de la página de código
  uint32_t code_page = (uint32_t)current->user_entry_point & ~0xFFF;

  if (!mmu_is_mapped(code_page)) {
    serial_printf(COM1_BASE,
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

  // Verificar y asegurar permisos de usuario para la página de código
  uint32_t current_flags = mmu_get_page_flags(code_page);
  if (!(current_flags & PAGE_USER)) {
    if (!(current->flags & TASK_FLAG_QUIET)) {
      serial_printf(COM1_BASE,
                    "[USER_WRAPPER] Adding PAGE_USER flag to 0x%08x\r\n",
                    code_page);
    }
    if (!mmu_set_page_user(code_page)) {
      if (!(current->flags & TASK_FLAG_QUIET)) {
        serial_printf(COM1_BASE,
                      "[USER_WRAPPER] ERROR: Failed to set PAGE_USER!\r\n");
      }
      task_exit(-1);
      return;
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
  if (!(current->flags & TASK_FLAG_QUIET)) {
    terminal_printf(&main_terminal,
                    "[USER_WRAPPER] Ready for switch:\r\n"
                    "  CS:EIP = 0x%04x:0x%08x\r\n"
                    "  SS:ESP = 0x%04x:0x%08x\r\n"
                    "  DS:ES:FS:GS = 0x%04x:0x%04x:0x%04x:0x%04x\r\n"
                    "  EFLAGS = 0x%08x\r\n",
                    user_ctx.cs, user_ctx.eip, user_ctx.ss, user_ctx.esp,
                    user_ctx.ds, user_ctx.es, user_ctx.fs, user_ctx.gs,
                    user_ctx.eflags);
  }

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

task_t *task_create_user(const char *name, void *user_code_addr, int argc,
                         char **argv, uint32_t code_size,
                         task_priority_t priority, address_space_t *as) {
  if (exec_verbose) {
    terminal_printf(&main_terminal,
                    "[USER_CREATE] Creating user task: %s at 0x%08x\r\n", name,
                    (uint32_t)user_code_addr);
  }

  // 1. Validar o crear el espacio de direcciones
  if (!as) {
    as = vmm_create_address_space();
    if (!as) {
      terminal_puts(&main_terminal,
                    "[USER_CREATE] ERROR: Failed to create address space\r\n");
      return NULL;
    }
  }

  // 2. Cambiar temporalmente al nuevo espacio de direcciones para la
  // configuración
  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  // 3. Asignar stack con GUARD PAGE en el nuevo espacio
  size_t aligned_stack_size = (USER_STACK_SIZE + 0xFFF) & ~0xFFF;
  size_t total_alloc_size = aligned_stack_size + PAGE_SIZE;

  // El kernel aloca memoria física para el stack
  void *phys_stack = pmm_alloc_pages(total_alloc_size / PAGE_SIZE);
  if (!phys_stack) {
    mmu_load_cr3(old_cr3);
    terminal_puts(&main_terminal,
                  "[USER_CREATE] ERROR: Out of physical memory for stack\r\n");
    return NULL;
  }

  // Mapear el stack en una dirección fija de usuario (p.ej. 0xBFFFF000)
  uint32_t user_stack_virt = 0xC0000000 - total_alloc_size;
  for (uint32_t i = PAGE_SIZE; i < total_alloc_size; i += PAGE_SIZE) {
    mmu_map_page(user_stack_virt + i, (uint32_t)phys_stack + i,
                 PAGE_PRESENT | PAGE_RW | PAGE_USER);
  }
  // La primera página es de guarda (no mapeada o sin permisos)

  // 4. Preparar stack top y ARGC/ARGV (accedemos vía mapeo kernel directo)
  uint32_t stack_top_virt = (0xC0000000 - 32) & ~0xF;

  // Para ESCRIBIR en el stack desde el kernel, usamos la dirección virtual
  // mapeada PERO como acabamos de cambiar CR3, si intentamos acceder a
  // user_stack_virt, el MMU usará la tabla que acabamos de crear. ¡Perfecto!

  if (argc > 0 && argv != NULL) {
    uint32_t arg_ptrs[argc];
    // 1. Copiar las cadenas de texto al stack
    for (int i = argc - 1; i >= 0; i--) {
      size_t len = strlen(argv[i]) + 1;
      stack_top_virt -= len;
      memcpy((void *)stack_top_virt, argv[i], len);
      arg_ptrs[i] = stack_top_virt;
    }

    // Alinear stack a 16 bytes (importante para ABI de System V)
    stack_top_virt &= ~0xF;

    // 2. Crear el array argv[] (punteros a las cadenas) en el stack
    // Primero, un terminador NULL para las listas (envp, luego argv)
    stack_top_virt -= 4;
    *(uint32_t *)stack_top_virt = 0;

    for (int i = argc - 1; i >= 0; i--) {
      stack_top_virt -= 4;
      *(uint32_t *)stack_top_virt = arg_ptrs[i];
    }

    // 3. El formato que espera crt0.asm en Ring 3 es el estándar de Linux:
    // [esp]     = argc
    // [esp+4]   = argv[0]
    // [esp+8]   = argv[1]
    // ...
    // Por tanto, solo tenemos que añadir argc encima de los punteros
    stack_top_virt -= 4;
    *(uint32_t *)stack_top_virt = (uint32_t)argc;
  }


  // 5. Restaurar CR3 anterior
  mmu_load_cr3(old_cr3);

  // 6. Crear estructura de tarea
  task_t *task = task_create(name, user_mode_entry_wrapper, NULL, priority);
  if (!task) {
    pmm_free_pages(phys_stack, total_alloc_size / PAGE_SIZE);
    return NULL;
  }

  // 7. Asociar espacio de direcciones
  task->address_space = as;

  // Configurar metadatos de usuario
  task->user_stack_base = (void *)user_stack_virt;
  task->user_stack_top = (void *)stack_top_virt;
  task->user_stack_phys = phys_stack;
  task->user_stack_size = total_alloc_size;
  task->user_entry_point = user_code_addr;
  task->user_code_base = user_code_addr;
  task->user_code_size = code_size;
  task->flags |= TASK_FLAG_USER_MODE;

  if (!exec_verbose) {
    task->flags |= TASK_FLAG_QUIET;
  }

  // Inicializar tabla de descriptores
  for (int i = 0; i < VFS_MAX_FDS; i++)
    task->fd_table[i] = NULL;

  task->fd_table[0] = (void *)1;
  task->fd_table[1] = (void *)1;
  task->fd_table[2] = (void *)1;

  // ✅ Limpieza obligatoria al 1er READ de stdin (compatibilidad AlvOS)
  task->stdin_buffer_dirty = true;

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

  if (!(task->flags & TASK_FLAG_QUIET)) {
    serial_printf(
        COM1_BASE, "[STACK] %s: ESP=0x%08x (aligned=%s) EIP=0x%08x\r\n",
        task->name, task->context.esp,
        ((uint32_t)stack_ptr & 0xF) == 0 ? "YES" : "NO", task->context.eip);
  }
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
    // HLT para ahorrar energía
    __asm__ volatile(
        "sti; hlt"); // Asegurar interrupciones habilitadas para despertar

    // Limpiar zombies en cada ciclo idle
    task_cleanup_zombies();

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

    // CAMBIO: 200ms en lugar de 1 segundo
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