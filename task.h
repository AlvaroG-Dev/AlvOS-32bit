#ifndef TASK_H
#define TASK_H

#include "isr.h"
#include "memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Estados de las tareas
typedef enum {
  TASK_CREATED,  // Tarea creada pero no iniciada
  TASK_RUNNING,  // Tarea en ejecución
  TASK_READY,    // Tarea lista para ejecutar
  TASK_SLEEPING, // Tarea durmiendo
  TASK_WAITING,  // Tarea esperando por algo
  TASK_FINISHED, // Tarea terminada
  TASK_ZOMBIE    // Tarea terminada pero no recolectada
} task_state_t;

// Prioridades de tareas (0 = máxima, 7 = mínima)
typedef enum {
  TASK_PRIORITY_HIGH = 0,
  TASK_PRIORITY_NORMAL = 3,
  TASK_PRIORITY_LOW = 7
} task_priority_t;

// Tamaño del stack para cada tarea
#define TASK_STACK_SIZE (16 * 1024)
#define USER_STACK_SIZE (8 * 1024) // Stack más pequeño para usuario
#define MAX_TASKS 32
#define TASK_NAME_MAX 32

// Flags para tareas
#define TASK_FLAG_USER_MODE 0x00000001  // Ejecuta en modo usuario (Ring 3)
#define TASK_FLAG_USER_STACK 0x00000002 // Tiene stack de usuario asignado

// Contexto de CPU para cambio de tareas
typedef struct {
  // Registros de propósito general
  uint32_t eax, ebx, ecx, edx;
  uint32_t esi, edi, ebp, esp;
  uint32_t eip;

  // Registros de segmento
  uint32_t cs, ds, es, fs, gs, ss;

  // Flags
  uint32_t eflags;
} cpu_context_t;

// Estructura de control de tarea (TCB)
typedef struct task {
  uint32_t task_id;         // ID único de la tarea
  char name[TASK_NAME_MAX]; // Nombre de la tarea
  task_state_t state;       // Estado actual
  task_priority_t priority; // Prioridad de la tarea

  // Contexto de CPU
  cpu_context_t context;

  // Stack de la tarea
  void *stack_base;  // Base del stack
  void *stack_top;   // Tope del stack
  size_t stack_size; // Tamaño del stack

  // Stack de usuario (solo para tareas usuario)
  void *user_stack_base;
  void *user_stack_top;
  size_t user_stack_size;

  // **NUEVO: Flags para la tarea**
  uint32_t flags; // Flags de la tarea

  // Información de usuario
  void *user_code_base;
  size_t user_code_size;
  void *user_entry_point; // <-- NUEVO: Punto de entrada de usuario
  address_space_t *address_space;

  // Información de tiempo
  uint32_t time_slice;  // Quantum de tiempo asignado
  uint32_t sleep_until; // Tick hasta el que duerme
  uint32_t wake_time;   // Tiempo de despertar

  // Lista enlazada
  struct task *next; // Siguiente tarea en la lista
  struct task *prev; // Tarea anterior

  // Función de entrada y datos
  void (*entry_point)(void *); // Función principal de la tarea (kernel wrapper)
  void *arg;                   // Argumento para la función

  // Estadísticas
  uint32_t total_runtime; // Tiempo total de ejecución
  uint32_t switch_count;  // Número de cambios de contexto

  // Valor de retorno
  int exit_code; // Código de salida

} task_t;

// Planificador de tareas
typedef struct {
  task_t *current_task; // Tarea actualmente en ejecución
  task_t *idle_task;    // Tarea idle del sistema
  task_t *task_list;    // Lista de todas las tareas

  uint32_t next_task_id;   // Próximo ID de tarea a asignar
  uint32_t task_count;     // Número de tareas activas
  uint32_t total_switches; // Total de cambios de contexto

  bool scheduler_enabled; // Si el planificador está habilitado
  uint32_t quantum_ticks; // Duración del quantum en ticks
} task_scheduler_t;

// Funciones principales del planificador
void task_init(void);
task_t *task_create(const char *name, void (*entry_point)(void *), void *arg,
                    task_priority_t priority);
void task_destroy(task_t *task);
void task_yield(void);         // Ceder voluntariamente el CPU
void task_sleep(uint32_t ms);  // Dormir por tiempo específico
void task_exit(int exit_code); // Terminar tarea actual

// Control del planificador
void scheduler_start(void);
void scheduler_stop(void);
void scheduler_tick(void);         // Llamar desde el timer interrupt
task_t *scheduler_next_task(void); // Obtener próxima tarea a ejecutar

// Funciones de información
task_t *task_current(void); // Obtener tarea actual
task_t *task_find_by_id(uint32_t task_id);
task_t *task_find_by_name(const char *name);
void task_list_all(void); // Listar todas las tareas

// Funciones auxiliares
void task_setup_stack(task_t *task, void (*entry_point)(void *), void *arg);
bool task_is_ready(task_t *task);
void task_update_sleep_states(void);
void show_system_stats(void);
void stress_test_task(void *arg);
static bool validate_task_context(task_t *task);
void cleanup_task(void *arg);

// USER MODE
task_t *task_create_user(const char *name, void *user_code_addr, void *arg,
                         task_priority_t priority);
void task_setup_user_mode(task_t *task, void (*entry_point)(void *), void *arg,
                          void *user_stack);
// Macros útiles
#define CURRENT_TASK() task_current()
#define TASK_ID() (task_current() ? task_current()->task_id : 0)

// Selectores de segmento
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS 0x1B // 0x18 | 3
#define USER_DS 0x23 // 0x20 | 3

// Declaraciones externas
extern task_scheduler_t scheduler;

#endif // TASK_H