#include "task.h"
#include "kernel.h"
#include "memory.h"
#include "terminal.h"
#include "string.h"
#include "task_utils.h"
#include "irq.h"
#include "log.h"

// ========================================================================
// FUNCIONES DE SINCRONIZACIÓN BÁSICA (CORREGIDAS)
// ========================================================================

void mutex_init(mutex_t* mutex, const char* name) {
    if (!mutex) return;
    mutex->locked = false;
    mutex->owner = NULL;
    mutex->lock_count = 0;
    mutex->name = name ? name : "unnamed_mutex";
}

bool mutex_try_lock(mutex_t* mutex) {
    if (!mutex) return false;
    
    // Deshabilitar interrupciones para operación atómica
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    
    bool success = false;
    task_t* current = task_current();
    
    // ✅ FIX: NO permitir reentrada en try_lock (comportamiento estándar)
    // Solo permitir si el mutex está completamente libre
    if (!mutex->locked) {
        mutex->locked = true;
        mutex->owner = current;
        mutex->lock_count = 1;
        success = true;
    }
    
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return success;
}

void mutex_lock(mutex_t* mutex) {
    if (!mutex) return;
    
    task_t* current = task_current();
    
    // Permitir reentrada
    if (mutex->owner == current) {
        uint32_t flags;
        __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
        mutex->lock_count++;
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        return;
    }
    
    uint32_t start_ticks = ticks_since_boot;
    uint32_t attempts = 0;
    uint32_t backoff = 1;  // ✅ NUEVO: Backoff exponencial
    
    while (!mutex_try_lock(mutex)) {
        attempts++;
        
        // Timeout de 5 segundos
        if (ticks_since_boot - start_ticks > 500) {
            log_message(LOG_INFO, 
                "[MUTEX] TIMEOUT: %s waiting for %s (owner: %s, attempts: %u)\r\n",
                current ? current->name : "unknown",
                mutex->name,
                mutex->owner ? mutex->owner->name : "none",
                attempts);
            return;
        }
        
        // ✅ FIX: Backoff exponencial para reducir contención
        for (volatile uint32_t i = 0; i < backoff; i++) {
            __asm__ volatile("pause");
        }
        
        // Aumentar backoff hasta un máximo
        if (backoff < 1000) {
            backoff *= 2;
        }
        
        // Yield cada 10 intentos
        if (attempts % 10 == 0) {
            task_yield();
        }
    }
}

void mutex_unlock(mutex_t* mutex) {
    if (!mutex || !mutex->locked) return;
    
    // Verificar que solo el propietario pueda desbloquear
    if (mutex->owner != task_current()) {
        log_message(LOG_INFO, 
            "[MUTEX] WARNING: %s trying to unlock %s owned by %s\r\n",
            task_current() ? task_current()->name : "unknown",
            mutex->name,
            mutex->owner ? mutex->owner->name : "unknown");
        return;
    }
    
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    
    // ✅ FIX: Decrementar contador de locks reentrantes
    if (mutex->lock_count > 1) {
        mutex->lock_count--;
    } else {
        mutex->locked = false;
        mutex->owner = NULL;
        mutex->lock_count = 0;
        
        // ✅ FIX: Barrera de memoria después de liberar (x86-32 compatible)
        __asm__ __volatile__("lock; addl $0, (%%esp)" ::: "memory", "cc");
    }
    
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}

// ========================================================================
// SISTEMA DE MENSAJES ENTRE TAREAS (CORREGIDO)
// ========================================================================

static message_queue_t message_queues[MAX_MESSAGE_QUEUES];
static bool message_system_initialized = false;

void message_system_init(void) {
    if (message_system_initialized) return;
    
    for (int i = 0; i < MAX_MESSAGE_QUEUES; i++) {
        message_queues[i].owner_task_id = 0;
        message_queues[i].head = NULL;
        message_queues[i].tail = NULL;
        message_queues[i].message_count = 0;
        message_queues[i].has_messages = false;  // ✅ NUEVO
        
        char mutex_name[32];
        snprintf(mutex_name, sizeof(mutex_name), "msgqueue_%d", i);
        mutex_init(&message_queues[i].queue_mutex, mutex_name);
    }
    
    message_system_initialized = true;
    log_message(LOG_INFO, "Message system initialized\r\n");
}

// ✅ FIX: Nueva función para obtener la cola de una tarea
message_queue_t* message_queue_get(uint32_t task_id) {
    if (!message_system_initialized) {
        message_system_init();
    }
    
    for (int i = 0; i < MAX_MESSAGE_QUEUES; i++) {
        if (message_queues[i].owner_task_id == task_id) {
            return &message_queues[i];
        }
    }
    return NULL;
}

message_queue_t* message_queue_create(uint32_t task_id) {
    if (!message_system_initialized) {
        message_system_init();
    }
    
    // ✅ FIX: Verificar si ya existe una cola para esta tarea
    message_queue_t* existing = message_queue_get(task_id);
    if (existing) {
        log_message(LOG_INFO, 
            "[MSG] Queue already exists for task %u\r\n", task_id);
        return existing;
    }
    
    for (int i = 0; i < MAX_MESSAGE_QUEUES; i++) {
        if (message_queues[i].owner_task_id == 0) {
            message_queues[i].owner_task_id = task_id;
            log_message(LOG_INFO, 
                "[MSG] Created queue for task %u at slot %d\r\n", task_id, i);
            return &message_queues[i];
        }
    }
    
    log_message(LOG_ERROR, "[MSG] ERROR: No free message queues\r\n");
    return NULL;
}

bool message_send(uint32_t target_task_id, uint32_t type, const void* data, size_t size) {
    if (!message_system_initialized || size > MAX_MESSAGE_SIZE) {
        return false;
    }
    
    // ✅ Deshabilitar interrupciones para operación atómica completa
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    
    message_queue_t* queue = message_queue_get(target_task_id);
    
    if (!queue) {
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        log_message(LOG_WARN, "[MSG] No queue for task %u\n", target_task_id);
        return false;
    }
    
    if (queue->message_count >= MAX_MESSAGES_PER_QUEUE) {
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        log_message(LOG_WARN, "[MSG] Queue full for task %u\n", target_task_id);
        return false;
    }
    
    // Crear mensaje
    message_t* msg = (message_t*)kernel_malloc(sizeof(message_t));
    if (!msg) {
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        log_message(LOG_ERROR, "[MSG] Failed to allocate message\n");
        return false;
    }
    
    msg->sender_id = task_current() ? task_current()->task_id : 0;
    msg->type = type;
    msg->size = size;
    if (data && size > 0) {
        memcpy(msg->data, data, size);
    }
    msg->next = NULL;
    
    // Añadir a la cola
    if (queue->tail) {
        queue->tail->next = msg;
        queue->tail = msg;
    } else {
        queue->head = queue->tail = msg;
    }
    queue->message_count++;
    
    // ✅ FIX: Establecer flag de señalización
    queue->has_messages = true;
    
    // ✅ FIX: Usar barrera de memoria para asegurar visibilidad
    __asm__ __volatile__("mfence" ::: "memory");
    
    log_message(LOG_INFO, 
        "[MSG] Sent type=%u to task %u (count=%u)\n",
        type, target_task_id, queue->message_count);
    
    // ✅ FIX: Despertar tarea de manera segura
    task_t* target_task = task_find_by_id(target_task_id);
    if (target_task && target_task->state == TASK_SLEEPING) {
        target_task->state = TASK_READY;
        target_task->sleep_until = 0;  // Cancelar sleep
        log_message(LOG_INFO, "[MSG] Woke up task %s\n", target_task->name);
    }
    
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return true;
}

bool message_receive(message_t* msg_out, bool blocking) {
    if (!message_system_initialized || !msg_out) {
        log_message(LOG_WARN, "[MSG] receive: system not init or NULL output\r\n");
        return false;
    }
    
    task_t* current = task_current();
    if (!current) {
        log_message(LOG_WARN, "[MSG] receive: no current task\r\n");
        return false;
    }
    
    message_queue_t* queue = message_queue_get(current->task_id);
    if (!queue) {
        log_message(LOG_INFO, "[MSG] receive: No queue for task %u (%s)\r\n", 
                   current->task_id, current->name);
        return false;
    }
    
    uint32_t wait_start = ticks_since_boot;
    uint32_t check_count = 0;
    
    while (true) {
        check_count++;
        
        // ✅ FIX: Verificar flag primero (optimización)
        if (!queue->has_messages && !blocking) {
            return false;
        }
        
        // ✅ FIX: Lock solo cuando necesitamos acceder a la cola
        uint32_t flags;
        __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
        
        if (queue->head) {
            // Tomar el primer mensaje
            message_t* msg = queue->head;
            queue->head = msg->next;
            if (!queue->head) {
                queue->tail = NULL;
                queue->has_messages = false;  // ✅ Limpiar flag
            }
            queue->message_count--;
            
            // Copiar al output
            memcpy(msg_out, msg, sizeof(message_t));
            
            log_message(LOG_INFO, 
                "[MSG] Received type=%u by %s (remaining=%u)\r\n",
                msg_out->type, current->name, queue->message_count);
            
            // Liberar el mensaje
            kernel_free(msg);
            
            __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
            return true;
        }
        
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        
        // ✅ Si no es blocking, salir inmediatamente
        if (!blocking) {
            return false;
        }
        
        // ✅ FIX: Timeout con yield frecuente
        if (ticks_since_boot - wait_start > 500) { // 5 segundos
            log_message(LOG_INFO, 
                "[MSG] Timeout waiting for message (task %s, checks=%u)\r\n",
                current->name, check_count);
            return false;
        }
        
        // ✅ FIX: Sleep corto en lugar de spin
        // Esto permite que el sender se ejecute
        task_sleep(10);  // 10ms
    }
}

void message_queue_destroy(message_queue_t* queue) {
    if (!queue) return;
    
    mutex_lock(&queue->queue_mutex);
    
    // Liberar todos los mensajes pendientes
    message_t* current = queue->head;
    while (current) {
        message_t* next = current->next;
        kernel_free(current);
        current = next;
    }
    
    queue->head = NULL;
    queue->tail = NULL;
    queue->message_count = 0;
    queue->owner_task_id = 0;
    
    mutex_unlock(&queue->queue_mutex);
}

// ========================================================================
// PROFILING DE TAREAS
// ========================================================================

typedef struct {
    uint32_t task_switches;
    uint32_t total_runtime;
    uint32_t max_runtime_in_switch;
    uint32_t min_runtime_in_switch;
    uint32_t average_runtime_per_switch;
} task_profile_t;

static task_profile_t task_profiles[MAX_TASKS] = {0};
static bool profiling_enabled = false;

void task_profiling_enable(void) {
    if (profiling_enabled) return;
    memset(task_profiles, 0, sizeof(task_profiles));
    profiling_enabled = true;
    terminal_puts(&main_terminal, "Task profiling enabled\r\n");
}

void task_profiling_disable(void) {
    profiling_enabled = false;
    terminal_puts(&main_terminal, "Task profiling disabled\r\n");
}

void task_profiling_update(task_t* task, uint32_t runtime_ticks) {
    if (!profiling_enabled || !task || task->task_id >= MAX_TASKS) return;
    
    task_profile_t* profile = &task_profiles[task->task_id];
    profile->task_switches++;
    profile->total_runtime += runtime_ticks;
    
    if (runtime_ticks > profile->max_runtime_in_switch) {
        profile->max_runtime_in_switch = runtime_ticks;
    }
    if (runtime_ticks < profile->min_runtime_in_switch || profile->task_switches == 1) {
        profile->min_runtime_in_switch = runtime_ticks;
    }
    
    profile->average_runtime_per_switch = profile->total_runtime / profile->task_switches;
}

// ========================================================================
// FUNCIONES DE MONITOREO AVANZADO
// ========================================================================

void task_monitor_health(void) {
    terminal_puts(&main_terminal, "\r\n=== Task Health Monitor ===\r\n");
    
    uint32_t healthy_tasks = 0;
    uint32_t zombie_tasks = 0;
    uint32_t sleeping_tasks = 0;
    uint32_t ready_tasks = 0;
    uint32_t running_tasks = 0;
    
    if (scheduler.task_list) {
        task_t* current = scheduler.task_list;
        do {
            switch (current->state) {
                case TASK_RUNNING:
                    running_tasks++;
                    healthy_tasks++;
                    break;
                case TASK_READY:
                case TASK_CREATED:
                    ready_tasks++;
                    healthy_tasks++;
                    break;
                case TASK_SLEEPING:
                    sleeping_tasks++;
                    healthy_tasks++;
                    break;
                case TASK_ZOMBIE:
                case TASK_FINISHED:
                    zombie_tasks++;
                    break;
                default:
                    break;
            }
            current = current->next;
        } while (current != scheduler.task_list);
    }
    
    terminal_printf(&main_terminal, "Total tasks: %u\r\n", scheduler.task_count);
    terminal_printf(&main_terminal, "  Running: %u\r\n", running_tasks);
    terminal_printf(&main_terminal, "  Ready: %u\r\n", ready_tasks);
    terminal_printf(&main_terminal, "  Sleeping: %u\r\n", sleeping_tasks);
    terminal_printf(&main_terminal, "  Zombie/Finished: %u\r\n", zombie_tasks);
    terminal_printf(&main_terminal, "  Healthy: %u\r\n", healthy_tasks);
    
    // Advertencias
    if (zombie_tasks > 0) {
        terminal_printf(&main_terminal, "WARNING: %u zombie tasks detected!\r\n", zombie_tasks);
    }
    
    if (scheduler.task_count > (MAX_TASKS * 0.8)) {
        terminal_printf(&main_terminal, "WARNING: Task limit nearly reached (%u/%u)\r\n", 
                       scheduler.task_count, MAX_TASKS);
    }
    
    // Verificar si hay deadlocks potenciales
    if (ready_tasks == 0 && running_tasks <= 1 && sleeping_tasks > 0) {
        terminal_puts(&main_terminal, "POTENTIAL DEADLOCK: All tasks sleeping!\r\n");
    }
    
    terminal_puts(&main_terminal, "\r\n");
}

void task_cleanup_zombies(void) {
    if (!scheduler.task_list) return;
    
    task_t* current = scheduler.task_list;
    task_t* start = current;
    
    do {
        task_t* next = current->next;
        
        if ((current->state == TASK_ZOMBIE || current->state == TASK_FINISHED) &&
            current != scheduler.idle_task) {
            
            log_message(LOG_INFO, 
                "Cleaning up task: %s (ID: %u, state: %s)\n", 
                current->name, current->task_id,
                current->state == TASK_ZOMBIE ? "ZOMBIE" : "FINISHED");
            task_destroy(current);
            
            if (current == start) {
                start = next;
            }
        }
        
        current = next;
    } while (current && current != start && scheduler.task_list);
}

// ========================================================================
// FUNCIONES AUXILIARES PARA DEBUGGING
// ========================================================================

void task_dump_context(task_t* task) {
    if (!task) {
        terminal_puts(&main_terminal, "Task is NULL\r\n");
        return;
    }
    
    terminal_printf(&main_terminal, "\r\n=== Task Context Dump: %s (ID: %u) ===\r\n", 
                   task->name, task->task_id);
    
    cpu_context_t* ctx = &task->context;
    terminal_printf(&main_terminal, "Registers:\r\n");
    terminal_printf(&main_terminal, "  EAX: 0x%08x  EBX: 0x%08x  ECX: 0x%08x  EDX: 0x%08x\r\n",
                   ctx->eax, ctx->ebx, ctx->ecx, ctx->edx);
    terminal_printf(&main_terminal, "  ESI: 0x%08x  EDI: 0x%08x  EBP: 0x%08x  ESP: 0x%08x\r\n",
                   ctx->esi, ctx->edi, ctx->ebp, ctx->esp);
    terminal_printf(&main_terminal, "  EIP: 0x%08x  EFLAGS: 0x%08x\r\n",
                   ctx->eip, ctx->eflags);
    terminal_printf(&main_terminal, "Segments:\r\n");
    terminal_printf(&main_terminal, "  CS: 0x%04x  DS: 0x%04x  ES: 0x%04x\r\n",
                   ctx->cs, ctx->ds, ctx->es);
    terminal_printf(&main_terminal, "  FS: 0x%04x  GS: 0x%04x  SS: 0x%04x\r\n",
                   ctx->fs, ctx->gs, ctx->ss);
    terminal_puts(&main_terminal, "\r\n");
}