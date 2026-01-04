#include "network_daemon.h"
#include "kernel.h"
#include "network_stack.h"
#include "task.h"
#include "terminal.h"

static task_t *network_daemon_task = NULL;
static volatile bool daemon_running = false;

// Función principal del daemon de red
void network_daemon_func(void *arg) {
  (void)arg; // No usado

  terminal_puts(&main_terminal,
                "[NET_DAEMON] Network daemon started (kernel task)\r\n");
  daemon_running = true;

  while (daemon_running) {
    // ✅ Protección crítica contra race conditions
    uint32_t flags;
    __asm__ __volatile__("pushf; cli; pop %0" : "=r"(flags));

    // Procesar paquetes de red
    network_stack_tick();

    __asm__ __volatile__("push %0; popf" : : "r"(flags));

    // Ceder CPU a otras tareas - usar yield para mayor estabilidad
    task_yield();
  }

  terminal_puts(&main_terminal, "[NET_DAEMON] Network daemon stopped\r\n");
}

// Iniciar el daemon de red
bool network_daemon_start(void) {
  if (network_daemon_task != NULL) {
    terminal_puts(&main_terminal, "[NET_DAEMON] Daemon already running\r\n");
    return false;
  }

  // Crear tarea kernel de alta prioridad para procesamiento de red
  network_daemon_task =
      task_create("net_daemon", network_daemon_func, NULL, TASK_PRIORITY_HIGH);

  if (!network_daemon_task) {
    terminal_puts(&main_terminal,
                  "[NET_DAEMON] Failed to create network daemon task\r\n");
    return false;
  }

  terminal_puts(&main_terminal,
                "[NET_DAEMON] Network daemon task created successfully\r\n");
  return true;
}

// Detener el daemon de red
void network_daemon_stop(void) {
  if (!network_daemon_task) {
    return;
  }

  daemon_running = false;

  // Esperar a que termine (simplificado - en producción usaríamos
  // sincronización)
  task_sleep(100);

  network_daemon_task = NULL;
}

// Verificar si el daemon está corriendo
bool network_daemon_is_running(void) { return daemon_running; }
