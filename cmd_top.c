#include "keyboard.h"
#include "memory.h"
#include "string.h"
#include "task.h"
#include "terminal.h"

// External from kernel
extern volatile uint32_t ticks_since_boot;

// Colores para el comando top
#define TOP_COLOR_HEADER 0x00A8E6   // Cyan brillante
#define TOP_COLOR_RUNNING 0x00FF00  // Verde brillante
#define TOP_COLOR_READY 0xFFFF00    // Amarillo
#define TOP_COLOR_SLEEPING 0x808080 // Gris
#define TOP_COLOR_ZOMBIE 0xFF0000   // Rojo
#define TOP_COLOR_HIGH_MEM 0xFF6B35 // Naranja
#define TOP_COLOR_TEXT 0xFFFFFF     // Blanco
#define TOP_COLOR_ACCENT 0x00D9FF   // Cyan claro

// Variable global para controlar la tarea top
static volatile bool top_task_running = false;
static task_t *top_task_handle = NULL;

// Estructura para estadísticas de tareas
typedef struct {
  uint32_t task_id;
  char name[TASK_NAME_MAX];
  task_state_t state;
  task_priority_t priority;
  uint32_t total_runtime;
  uint32_t switch_count;
  size_t stack_used;
  size_t stack_total;
  uint8_t cpu_percent;
} task_stats_t;

// Obtener nombre del estado como string
static const char *get_state_name(task_state_t state) {
  switch (state) {
  case TASK_CREATED:
    return "CREATED";
  case TASK_RUNNING:
    return "RUNNING";
  case TASK_READY:
    return "READY  ";
  case TASK_SLEEPING:
    return "SLEEP  ";
  case TASK_WAITING:
    return "WAITING";
  case TASK_FINISHED:
    return "FINISH ";
  case TASK_ZOMBIE:
    return "ZOMBIE ";
  default:
    return "UNKNOWN";
  }
}

// Obtener color según el estado
static uint32_t get_state_color(task_state_t state) {
  switch (state) {
  case TASK_RUNNING:
    return TOP_COLOR_RUNNING;
  case TASK_READY:
    return TOP_COLOR_READY;
  case TASK_SLEEPING:
  case TASK_WAITING:
    return TOP_COLOR_SLEEPING;
  case TASK_FINISHED:
  case TASK_ZOMBIE:
    return TOP_COLOR_ZOMBIE;
  default:
    return TOP_COLOR_TEXT;
  }
}

// ✅ FUNCIÓN MEJORADA CON DEBUG: Calcular uso del stack
static size_t calculate_stack_usage(task_t *task) {
  if (!task || !task->stack_base || !task->stack_size) {
    return 0;
  }

  // ⚠️ IMPORTANTE: Según tu código en task.c, necesitamos verificar cómo
  // se interpreta stack_base y stack_top

  // En task_setup_stack() tienes:
  // uint8_t *stack_end = (uint8_t *)task->stack_base + task->stack_size;
  // Esto significa que:
  //   - stack_base es la dirección BAJA (inicio del buffer)
  //   - stack_top (task->stack_top) es la dirección ALTA (stack_base + size)

  uintptr_t stack_bottom = (uintptr_t)task->stack_base;
  uintptr_t stack_top = (uintptr_t)task->stack_top;
  uintptr_t current_esp = (uintptr_t)task->context.esp;

  // DEBUG: Validar que stack_top sea coherente
  if (stack_top == 0 || stack_top <= stack_bottom) {
    // stack_top no está configurado, calcularlo
    stack_top = stack_bottom + task->stack_size;
  }

  // Validación de rango
  if (current_esp < stack_bottom || current_esp > stack_top) {
    // ESP fuera de rango - retornar 100% como advertencia
    return task->stack_size;
  }

  // Si ESP == stack_top, el stack está vacío (recién inicializado)
  if (current_esp >= stack_top - 16) {
    // Stack prácticamente vacío (menos de 16 bytes usados)
    // Esto es normal en tareas recién creadas
    return 0;
  }

  // Cálculo: stack usado = desde stack_top hasta ESP
  size_t used = stack_top - current_esp;

  // Limitar al tamaño total
  if (used > task->stack_size) {
    return task->stack_size;
  }

  return used;
}

// ✅ FUNCIÓN MEJORADA: Recolectar estadísticas de todas las tareas
static int collect_task_stats(task_stats_t *stats, int max_tasks,
                              task_t *current_running_task) {
  if (!scheduler.task_list) {
    return 0;
  }

  int count = 0;
  uint32_t total_runtime = 0;

  // Primera pasada: calcular tiempo total
  task_t *current = scheduler.task_list;
  do {
    total_runtime += current->total_runtime;
    current = current->next;
  } while (current != scheduler.task_list && count < 100);

  // Segunda pasada: recolectar estadísticas
  current = scheduler.task_list;
  count = 0;
  do {
    if (count >= max_tasks)
      break;

    stats[count].task_id = current->task_id;
    strncpy(stats[count].name, current->name, TASK_NAME_MAX - 1);
    stats[count].name[TASK_NAME_MAX - 1] = '\0';

    // ✅ CORRECCIÓN CRÍTICA: Mostrar el estado REAL de la tarea
    // Si es la tarea actualmente en ejecución (no la de top), mostrar RUNNING
    if (current == current_running_task && current != top_task_handle) {
      stats[count].state = TASK_RUNNING;
    } else {
      stats[count].state = current->state;
    }

    stats[count].priority = current->priority;
    stats[count].total_runtime = current->total_runtime;
    stats[count].switch_count = current->switch_count;
    stats[count].stack_total = current->stack_size;

    // ✅ Calcular uso de stack mejorado
    stats[count].stack_used = calculate_stack_usage(current);

    // Calcular porcentaje de CPU
    if (total_runtime > 0) {
      stats[count].cpu_percent = (current->total_runtime * 100) / total_runtime;
      // Limitar a 100%
      if (stats[count].cpu_percent > 100) {
        stats[count].cpu_percent = 100;
      }
    } else {
      stats[count].cpu_percent = 0;
    }

    count++;
    current = current->next;
  } while (current != scheduler.task_list);

  return count;
}

// Función para mostrar el top una vez (llamada por la tarea)
static void display_top_screen(Terminal *term) {
  if (!term)
    return;

  // ✅ Guardar la tarea que estaba corriendo antes de top
  task_t *previous_running = NULL;
  if (scheduler.task_list) {
    task_t *t = scheduler.task_list;
    do {
      // Buscar la tarea que tiene más runtime reciente (excluyendo top)
      if (t != top_task_handle && t->state == TASK_RUNNING) {
        previous_running = t;
        break;
      }
      t = t->next;
    } while (t != scheduler.task_list);
  }

  // Guardar colores actuales
  uint32_t old_fg = term->current_attrs.fg_color;
  uint32_t old_bg = term->current_attrs.bg_color;

  // === HEADER (ASCII ONLY) ===
  term->current_attrs.fg_color = TOP_COLOR_HEADER;
  terminal_puts(term, "\r\n");
  terminal_puts(term, "+-------------------------------------------------------"
                      "-------------------+\r\n");
  terminal_puts(term, "|                        ");
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_puts(term, "* ALVOS SYSTEM MONITOR *");
  term->current_attrs.fg_color = TOP_COLOR_HEADER;
  terminal_puts(term, "                        |\r\n");
  terminal_puts(term, "+-------------------------------------------------------"
                      "-------------------+\r\n");

  // === SYSTEM INFO ===
  term->current_attrs.fg_color = TOP_COLOR_TEXT;

  // Uptime
  uint32_t uptime_seconds = ticks_since_boot / 100;
  uint32_t hours = uptime_seconds / 3600;
  uint32_t minutes = (uptime_seconds % 3600) / 60;
  uint32_t seconds = uptime_seconds % 60;

  terminal_printf(term, "  ");
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_printf(term, "Uptime:");
  term->current_attrs.fg_color = TOP_COLOR_TEXT;
  terminal_printf(term, " %02u:%02u:%02u", hours, minutes, seconds);

  terminal_printf(term, "  |  ");
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_printf(term, "Tasks:");
  term->current_attrs.fg_color = TOP_COLOR_TEXT;
  terminal_printf(term, " %u", scheduler.task_count);

  terminal_printf(term, "  |  ");
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_printf(term, "Switches:");
  term->current_attrs.fg_color = TOP_COLOR_TEXT;
  terminal_printf(term, " %u\r\n", scheduler.total_switches);

  // Memoria
  heap_info_t heap = heap_stats_fast();
  uint32_t mem_total_kb = (heap.used + heap.free) / 1024;
  uint32_t mem_used_kb = heap.used / 1024;
  uint8_t mem_percent =
      (mem_total_kb > 0) ? ((mem_used_kb * 100) / mem_total_kb) : 0;

  terminal_printf(term, "  ");
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_printf(term, "Memory:");
  term->current_attrs.fg_color = TOP_COLOR_TEXT;
  terminal_printf(term, " %u KB / %u KB", mem_used_kb, mem_total_kb);

  // Barra de progreso de memoria (ASCII)
  terminal_printf(term, "  [");
  uint8_t bar_length = 20;
  uint8_t filled = (mem_percent * bar_length) / 100;

  for (uint8_t i = 0; i < bar_length; i++) {
    if (i < filled) {
      if (mem_percent > 80) {
        term->current_attrs.fg_color = TOP_COLOR_ZOMBIE;
      } else if (mem_percent > 60) {
        term->current_attrs.fg_color = TOP_COLOR_HIGH_MEM;
      } else {
        term->current_attrs.fg_color = TOP_COLOR_RUNNING;
      }
      terminal_puts(term, "#");
    } else {
      term->current_attrs.fg_color = TOP_COLOR_SLEEPING;
      terminal_puts(term, ".");
    }
  }

  term->current_attrs.fg_color = TOP_COLOR_TEXT;
  terminal_printf(term, "] %u%%\r\n", mem_percent);

  // Separador
  term->current_attrs.fg_color = TOP_COLOR_HEADER;
  terminal_puts(term, "  "
                      "--------------------------------------------------------"
                      "------------------\r\n");

  // === TABLE HEADER ===
  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_puts(term, "  PID  NAME             STATE    PRI  CPU%  SWITCHES  "
                      "STACK      RUNTIME\r\n");
  term->current_attrs.fg_color = TOP_COLOR_HEADER;
  terminal_puts(term, "  "
                      "--------------------------------------------------------"
                      "------------------\r\n");

  // === TASK LIST ===
  task_stats_t stats[MAX_TASKS];
  int task_count = collect_task_stats(stats, MAX_TASKS, previous_running);

  // Limitar a 12 tareas para que quepan en pantalla
  int display_count = (task_count < 12) ? task_count : 12;

  // ✅ Ordenar por CPU% (de mayor a menor) para mostrar las más activas primero
  for (int i = 0; i < task_count - 1; i++) {
    for (int j = i + 1; j < task_count; j++) {
      // Ordenar por: 1) CPU%, 2) Runtime si CPU% es igual
      if (stats[j].cpu_percent > stats[i].cpu_percent ||
          (stats[j].cpu_percent == stats[i].cpu_percent &&
           stats[j].total_runtime > stats[i].total_runtime)) {
        // Swap
        task_stats_t temp = stats[i];
        stats[i] = stats[j];
        stats[j] = temp;
      }
    }
  }

  for (int i = 0; i < display_count; i++) {
    term->current_attrs.fg_color = TOP_COLOR_TEXT;

    // PID
    terminal_printf(term, "  %-4u ", stats[i].task_id);

    // Name (con indicador si es la tarea actual)
    char name_padded[17];
    if (stats[i].state == TASK_RUNNING &&
        strncmp(stats[i].name, "top", 3) != 0) {
      // Marcar tarea RUNNING con asterisco
      snprintf(name_padded, sizeof(name_padded), "%-15s*", stats[i].name);
    } else {
      snprintf(name_padded, sizeof(name_padded), "%-16s", stats[i].name);
    }
    terminal_printf(term, "%s ", name_padded);

    // State (colorido)
    term->current_attrs.fg_color = get_state_color(stats[i].state);
    terminal_printf(term, "%-8s", get_state_name(stats[i].state));

    term->current_attrs.fg_color = TOP_COLOR_TEXT;

    // Priority
    terminal_printf(term, " %-3u  ", stats[i].priority);

    // CPU%
    if (stats[i].cpu_percent > 50) {
      term->current_attrs.fg_color = TOP_COLOR_RUNNING;
    } else if (stats[i].cpu_percent > 20) {
      term->current_attrs.fg_color = TOP_COLOR_HIGH_MEM;
    }
    terminal_printf(term, "%-4u%%", stats[i].cpu_percent);
    term->current_attrs.fg_color = TOP_COLOR_TEXT;

    // Switches
    terminal_printf(term, " %-9u ", stats[i].switch_count);

    // ✅ Stack usage MEJORADO con cálculo de porcentaje preciso
    uint32_t stack_percent = 0;
    if (stats[i].stack_total > 0 && stats[i].stack_used > 0) {
      // Multiplicar primero, dividir después para evitar truncamiento
      uint64_t percent64 =
          ((uint64_t)stats[i].stack_used * 100) / stats[i].stack_total;
      stack_percent = (uint32_t)percent64;

      // Si el resultado es 0 pero hay stack usado, mostrar al menos 1%
      if (stack_percent == 0 && stats[i].stack_used > 0) {
        stack_percent = 1;
      }
    }

    // Colorear según uso del stack
    if (stack_percent > 90) {
      term->current_attrs.fg_color = TOP_COLOR_ZOMBIE;
    } else if (stack_percent > 75) {
      term->current_attrs.fg_color = TOP_COLOR_HIGH_MEM;
    } else if (stack_percent > 50) {
      term->current_attrs.fg_color = TOP_COLOR_READY;
    } else if (stack_percent > 0) {
      term->current_attrs.fg_color = TOP_COLOR_TEXT;
    } else {
      // 0% legítimo (stack vacío)
      term->current_attrs.fg_color = TOP_COLOR_SLEEPING;
    }

    // Mostrar porcentaje O bytes si es muy pequeño
    if (stats[i].stack_total > 0) {
      if (stack_percent > 0) {
        terminal_printf(term, "%-3u%%", stack_percent);
      } else if (stats[i].stack_used > 0) {
        // Mostrar bytes directamente si el porcentaje redondea a 0
        terminal_printf(term, "<%3uB",
                        stats[i].stack_used > 999 ? 999 : stats[i].stack_used);
      } else {
        terminal_printf(term, "  0%%");
      }
    } else {
      terminal_printf(term, " N/A");
    }

    term->current_attrs.fg_color = TOP_COLOR_TEXT;
    terminal_printf(term, "    ");

    // Runtime
    terminal_printf(term, " %u\r\n", stats[i].total_runtime);
  }

  // Footer
  term->current_attrs.fg_color = TOP_COLOR_HEADER;
  terminal_puts(term, "  "
                      "--------------------------------------------------------"
                      "------------------\r\n");

  term->current_attrs.fg_color = TOP_COLOR_SLEEPING;
  terminal_printf(term, "  Total: %d task(s)", task_count);

  if (task_count > display_count) {
    terminal_printf(term, " (showing top %d by CPU)", display_count);
  }

  terminal_printf(term, "  |  Fragmentation: %.1f%%", heap.fragmentation);

  if (heap.fragmentation > 30.0f) {
    term->current_attrs.fg_color = TOP_COLOR_HIGH_MEM;
    terminal_puts(term, "  [!]");
  }

  terminal_puts(term, "\r\n");

  // ✅ Mostrar información adicional de debug si hay problemas
  term->current_attrs.fg_color = TOP_COLOR_SLEEPING;
  terminal_printf(term, "  Scheduler: %s  |  Quantum: %u ticks\r\n",
                  scheduler.scheduler_enabled ? "ON " : "OFF",
                  scheduler.quantum_ticks);

  term->current_attrs.fg_color = TOP_COLOR_ACCENT;
  terminal_puts(term,
                "  Press 'q' or Ctrl+C to quit  |  * = Currently running\r\n");

  // Restaurar colores
  term->current_attrs.fg_color = old_fg;
  term->current_attrs.bg_color = old_bg;
}

// Tarea que muestra top continuamente
static void top_task_func(void *arg) {
  Terminal *term = (Terminal *)arg;

  top_task_running = true;

  // ✅ Dar tiempo al sistema para estabilizarse
  task_sleep(100);

  while (top_task_running) {
    // Limpiar pantalla
    terminal_clear(term);

    // Mostrar top
    display_top_screen(term);

    // ✅ Esperar 1 segundo antes de actualizar (100 ticks a 100Hz)
    task_sleep(200);

    // Verificar si hay tecla presionada
    int key = keyboard_getkey_nonblock();
    if (key == 'q' || key == 'Q' || key == 3) { // 'q', 'Q', o Ctrl+C (ASCII 3)
      break;
    }
  }

  top_task_running = false;
  top_task_handle = NULL;

  terminal_clear(term);
  terminal_puts(term, "Top monitor stopped.\r\n");

  task_exit(0);
}

// Comando top que inicia la tarea
void cmd_top(Terminal *term) {
  if (!term)
    return;

  // Si ya hay una tarea top corriendo, notificar
  if (top_task_running) {
    terminal_puts(term,
                  "Top is already running. Press 'q' to quit it first.\r\n");
    return;
  }

  // Crear tarea top con prioridad baja para no interferir
  top_task_handle = task_create("top", top_task_func, term, TASK_PRIORITY_LOW);

  if (!top_task_handle) {
    terminal_puts(term, "Failed to create top task.\r\n");
    return;
  }

  terminal_puts(term, "Top monitor started. Press 'q' or Ctrl+C to quit.\r\n");
}

// ✅ FUNCIÓN ADICIONAL: Mostrar información detallada de una tarea específica
void cmd_task_info(Terminal *term, uint32_t task_id) {
  task_t *task = task_find_by_id(task_id);

  if (!task) {
    terminal_printf(term, "Task ID %u not found.\r\n", task_id);
    return;
  }

  terminal_printf(term, "\r\n=== Task Information: %s (ID: %u) ===\r\n",
                  task->name, task->task_id);
  terminal_printf(term, "State:        %s\r\n", get_state_name(task->state));
  terminal_printf(term, "Priority:     %u\r\n", task->priority);
  terminal_printf(term, "Runtime:      %u ticks\r\n", task->total_runtime);
  terminal_printf(term, "Switches:     %u\r\n", task->switch_count);

  // Información del stack CON DEBUG DETALLADO
  uintptr_t stack_bottom = (uintptr_t)task->stack_base;
  uintptr_t stack_top_calc = stack_bottom + task->stack_size;
  uintptr_t stack_top_stored = (uintptr_t)task->stack_top;
  uintptr_t current_esp = (uintptr_t)task->context.esp;

  size_t stack_used = calculate_stack_usage(task);
  uint32_t stack_percent = 0;
  if (task->stack_size > 0 && stack_used > 0) {
    // Usar 64 bits para evitar overflow en la multiplicación
    uint64_t percent64 = ((uint64_t)stack_used * 100) / task->stack_size;
    stack_percent = (uint32_t)percent64;

    // Mostrar al menos 1% si hay uso
    if (stack_percent == 0 && stack_used > 0) {
      stack_percent = 1;
    }
  }

  terminal_printf(term, "\r\n--- Stack Information ---\r\n");
  terminal_printf(term, "Stack Base:      0x%08x (LOW address)\r\n",
                  stack_bottom);
  terminal_printf(term, "Stack Size:      %u bytes (%u KB)\r\n",
                  task->stack_size, task->stack_size / 1024);
  terminal_printf(term, "Stack Top (calc):0x%08x (HIGH address)\r\n",
                  stack_top_calc);
  terminal_printf(term, "Stack Top (stor):0x%08x%s\r\n", stack_top_stored,
                  (stack_top_stored == stack_top_calc) ? " [OK]"
                                                       : " [MISMATCH!]");
  terminal_printf(term, "Current ESP:     0x%08x\r\n", current_esp);

  // Validación de ESP
  if (current_esp >= stack_bottom && current_esp <= stack_top_calc) {
    terminal_printf(term, "ESP Status:      VALID (within stack bounds)\r\n");
  } else {
    terminal_printf(term, "ESP Status:      INVALID [OUT OF BOUNDS!]\r\n");
  }

  // Cálculo detallado
  if (current_esp >= stack_bottom && current_esp <= stack_top_calc) {
    int32_t growth = stack_top_calc - current_esp;
    terminal_printf(term, "Stack Growth:    %d bytes from top\r\n", growth);
    terminal_printf(term, "Stack Used:      %u bytes (%u%%)\r\n", stack_used,
                    stack_percent);
    terminal_printf(term, "Stack Free:      %u bytes\r\n",
                    task->stack_size - stack_used);
  }

  terminal_printf(term, "\r\n--- Context Information ---\r\n");
  terminal_printf(term, "Entry Point:  0x%08x\r\n",
                  (uint32_t)task->entry_point);
  terminal_printf(term, "EIP:          0x%08x\r\n", task->context.eip);
  terminal_printf(term, "EBP:          0x%08x\r\n", task->context.ebp);
  terminal_printf(term, "EFLAGS:       0x%08x\r\n", task->context.eflags);

  // Flags
  terminal_printf(term, "\r\n--- Flags ---\r\n");
  terminal_printf(term, "Flags:        0x%08x", task->flags);
  if (task->flags & TASK_FLAG_USER_MODE) {
    terminal_puts(term, " [USER_MODE]");
  }
  if (task->flags == 0) {
    terminal_puts(term, " [KERNEL_MODE]");
  }
  terminal_puts(term, "\r\n\r\n");
}

// ✅ NUEVA FUNCIÓN: Debug de stack para todas las tareas
void cmd_stack_debug(Terminal *term) {
  if (!scheduler.task_list) {
    terminal_puts(term, "No tasks in scheduler.\r\n");
    return;
  }

  terminal_puts(term, "\r\n=== Stack Debug Information ===\r\n\r\n");

  task_t *current = scheduler.task_list;
  do {
    uintptr_t stack_bottom = (uintptr_t)current->stack_base;
    uintptr_t stack_top = stack_bottom + current->stack_size;
    uintptr_t esp = (uintptr_t)current->context.esp;

    terminal_printf(term, "Task: %-16s (ID: %u)\r\n", current->name,
                    current->task_id);
    terminal_printf(term, "  Stack: 0x%08x -> 0x%08x (%u bytes)\r\n",
                    stack_bottom, stack_top, current->stack_size);
    terminal_printf(term, "  ESP:   0x%08x ", esp);

    if (esp >= stack_bottom && esp <= stack_top) {
      size_t used = stack_top - esp;
      uint64_t percent64 = ((uint64_t)used * 100) / current->stack_size;
      uint32_t percent = (uint32_t)percent64;

      // Mostrar al menos 1% si hay uso
      if (percent == 0 && used > 0) {
        percent = 1;
      }

      terminal_printf(term, "[OK] Used: %u bytes (%u%%) = %.2f KB\r\n", used,
                      percent, (float)used / 1024.0f);
    } else if (esp == 0) {
      terminal_puts(term, "[WARN] ESP is NULL\r\n");
    } else {
      terminal_puts(term, "[ERROR] OUT OF BOUNDS!\r\n");
    }

    terminal_puts(term, "\r\n");
    current = current->next;
  } while (current != scheduler.task_list);
}