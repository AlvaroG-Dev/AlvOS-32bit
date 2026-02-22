// mutex_types.h - Definiciones de tipos de sincronización sin dependencias
// Este header existe para romper el ciclo de includes entre task.h, vfs.h
// y terminal.h. NO incluyas task.h ni task_utils.h desde aquí.
#ifndef MUTEX_TYPES_H
#define MUTEX_TYPES_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration - la definición completa está en task.h
struct task;

// ========================================================================
// WAIT QUEUE - tipo básico sin dependencias
// ========================================================================
typedef struct {
    struct task *head;
    struct task *tail;
    const char  *name;
    uint32_t     count;
} wait_queue_t;

// ========================================================================
// MUTEX - tipo básico sin dependencias
// ========================================================================
typedef struct {
    volatile bool locked;
    struct task  *owner;
    uint32_t      lock_count;   // Para locks reentrantes
    const char   *name;
    wait_queue_t  wait_queue;   // Cola de tareas esperando por este mutex
} mutex_t;

#endif // MUTEX_TYPES_H
