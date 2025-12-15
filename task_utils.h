#ifndef TASK_UTILS_H
#define TASK_UTILS_H

#include "task.h"
#include "kernel.h"
#include "memory.h"
#include "terminal.h"
#include "string.h"

// ========================================================================
// MACROS DE TESTING
// ========================================================================

#define TEST_START(name) \
    do { \
        terminal_printf(&main_terminal, "\r\n[TEST] %s...", name); \
    } while(0)

#define TEST_PASS() \
    do { \
        terminal_puts(&main_terminal, " PASS"); \
        tests_passed++; \
    } while(0)

#define TEST_FAIL(reason) \
    do { \
        terminal_printf(&main_terminal, " FAIL: %s", reason); \
        tests_failed++; \
    } while(0)

#define TEST_ASSERT(condition, reason) \
    do { \
        if (!(condition)) { \
            TEST_FAIL(reason); \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_FORMAT(condition, reason, ...) \
    do { \
        if (!(condition)) { \
            snprintf(test_output, sizeof(test_output), reason, ##__VA_ARGS__); \
            TEST_FAIL(test_output); \
            return; \
        } \
    } while(0)

// ========================================================================
// ESTRUCTURA MUTEX (MEJORADA)
// ========================================================================

typedef struct {
    volatile bool locked;
    task_t* owner;
    uint32_t lock_count;  // ✅ NUEVO: Para locks reentrantes
    const char* name;
} mutex_t;

// Funciones de sincronización
void mutex_init(mutex_t* mutex, const char* name);
bool mutex_try_lock(mutex_t* mutex);
void mutex_lock(mutex_t* mutex);
void mutex_unlock(mutex_t* mutex);

// ========================================================================
// SISTEMA DE MENSAJES
// ========================================================================

#define MAX_MESSAGE_QUEUES 16
#define MAX_MESSAGES_PER_QUEUE 32
#define MAX_MESSAGE_SIZE 256

typedef struct message {
    uint32_t sender_id;
    uint32_t type;
    size_t size;
    uint8_t data[MAX_MESSAGE_SIZE];
    struct message* next;
} message_t;

typedef struct {
    uint32_t owner_task_id;
    message_t* head;
    message_t* tail;
    uint32_t message_count;
    mutex_t queue_mutex;
    volatile bool has_messages;
} message_queue_t;

// Funciones de mensajes
void message_system_init(void);
message_queue_t* message_queue_create(uint32_t task_id);
message_queue_t* message_queue_get(uint32_t task_id);  // ✅ NUEVO
bool message_send(uint32_t target_task_id, uint32_t type, const void* data, size_t size);
bool message_receive(message_t* msg_out, bool blocking);
void message_queue_destroy(message_queue_t* queue);

// ========================================================================
// PROFILING
// ========================================================================

void task_profiling_enable(void);
void task_profiling_disable(void);
void task_profiling_update(task_t* task, uint32_t runtime_ticks);
void task_profiling_report(void);

// ========================================================================
// MONITOREO Y DEBUGGING
// ========================================================================

void task_monitor_health(void);
void task_cleanup_zombies(void);
void task_dump_context(task_t* task);

#endif // TASK_UTILS_H