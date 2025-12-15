#include "task_test.h"
#include "task_utils.h"
#include "task.h"
#include "terminal.h"
#include "string.h"
#include "kernel.h"
#include "memory.h"
#include "irq.h"

// ========================================================================
// TEST SUITE - VARIABLES GLOBALES
// ========================================================================

static int tests_passed = 0;
static int tests_failed = 0;
static char test_output[512];

// ========================================================================
// TESTS CORREGIDOS
// ========================================================================

static void test_mutex_basic(void) {
    TEST_START("Mutex Basic Lock/Unlock");
    
    mutex_t mutex;
    mutex_init(&mutex, "test_mutex");
    
    TEST_ASSERT(mutex_try_lock(&mutex) == true, "No pudo adquirir mutex libre");
    TEST_ASSERT(mutex.locked == true, "Mutex no marcado como locked");
    TEST_ASSERT(mutex.owner == task_current(), "Owner incorrecto");
    
    TEST_ASSERT(mutex_try_lock(&mutex) == false, "Pudo adquirir mutex ya locked");
    
    mutex_unlock(&mutex);
    TEST_ASSERT(mutex.locked == false, "Mutex no se desbloqueó");
    TEST_ASSERT(mutex.owner == NULL, "Owner no se limpió");
    
    TEST_PASS();
}

static void test_mutex_reentrancy(void) {
    TEST_START("Mutex Reentrancy (Recursive Lock)");
    
    mutex_t mutex;
    mutex_init(&mutex, "test_reentrant");
    
    // Primera adquisición
    TEST_ASSERT(mutex_try_lock(&mutex) == true, "Primera adquisición falló");
    TEST_ASSERT(mutex.lock_count == 1, "Lock count incorrecto (esperado 1)");
    
    // Segunda adquisición (reentrante)
    TEST_ASSERT(mutex_try_lock(&mutex) == true, "Reentrada falló");
    TEST_ASSERT(mutex.lock_count == 2, "Lock count incorrecto (esperado 2)");
    
    // Primer unlock
    mutex_unlock(&mutex);
    TEST_ASSERT(mutex.locked == true, "Mutex liberado prematuramente");
    TEST_ASSERT(mutex.lock_count == 1, "Lock count incorrecto después de unlock");
    
    // Segundo unlock
    mutex_unlock(&mutex);
    TEST_ASSERT(mutex.locked == false, "Mutex no liberado");
    TEST_ASSERT(mutex.lock_count == 0, "Lock count no se reseteo");
    
    TEST_PASS();
}

static mutex_t shared_mutex;
static volatile int shared_counter = 0;
static volatile int race_condition_detected = 0;

static void race_condition_task(void* arg) {
    int task_num = (int)(uintptr_t)arg;
    
    terminal_printf(&main_terminal, "[RACE_%d] Starting\r\n", task_num);
    
    for (int i = 0; i < 100; i++) {
        mutex_lock(&shared_mutex);
        
        int temp = shared_counter;
        // Simular trabajo crítico
        for (volatile int j = 0; j < 1000; j++);
        shared_counter = temp + 1;
        
        if (shared_counter != temp + 1) {
            race_condition_detected = 1;
            terminal_printf(&main_terminal, "[RACE_%d] RACE DETECTED!\r\n", task_num);
        }
        
        mutex_unlock(&shared_mutex);
        
        // Yield para dar oportunidad a otras tareas
        if (i % 10 == 0) {
            task_yield();
        }
    }
    
    terminal_printf(&main_terminal, "[RACE_%d] Finished\r\n", task_num);
    task_exit(0);
}

static void test_mutex_race_condition(void) {
    TEST_START("Mutex Race Condition Protection");
    
    mutex_init(&shared_mutex, "race_test");
    shared_counter = 0;
    race_condition_detected = 0;
    
    terminal_puts(&main_terminal, "\r\n[TEST] Creating race condition tasks...\r\n");
    
    task_t* task1 = task_create("race1", race_condition_task, (void*)1, TASK_PRIORITY_NORMAL);
    task_t* task2 = task_create("race2", race_condition_task, (void*)2, TASK_PRIORITY_NORMAL);
    task_t* task3 = task_create("race3", race_condition_task, (void*)3, TASK_PRIORITY_NORMAL);
    
    TEST_ASSERT(task1 != NULL, "No se pudo crear task1");
    TEST_ASSERT(task2 != NULL, "No se pudo crear task2");
    TEST_ASSERT(task3 != NULL, "No se pudo crear task3");
    
    terminal_puts(&main_terminal, "[TEST] Tasks created, waiting...\r\n");
    
    // Esperar hasta 20 segundos
    for (int i = 0; i < 2000; i++) {
        if (i % 100 == 0) {
            terminal_printf(&main_terminal, "[TEST] Progress: counter=%d/300\r\n", shared_counter);
        }
        
        if (shared_counter >= 300 &&
            task1->state == TASK_FINISHED &&
            task2->state == TASK_FINISHED &&
            task3->state == TASK_FINISHED) {
            terminal_puts(&main_terminal, "[TEST] All tasks completed!\r\n");
            break;
        }
        
        task_sleep(10);
    }
    
    TEST_ASSERT(race_condition_detected == 0, "Se detectó race condition");
    TEST_ASSERT_FORMAT(shared_counter == 300, 
                      "Contador incorrecto (esperado: 300, actual: %d)", 
                      shared_counter);
    
    // Cleanup
    task_cleanup_zombies();
    
    TEST_PASS();
}

// ========================================================================
// TESTS DE MENSAJES CORREGIDOS
// ========================================================================

static void test_message_system_basic(void) {
    TEST_START("Message System Basic");
    
    message_system_init();
    
    task_t* current = task_current();
    message_queue_t* queue = message_queue_create(current->task_id);
    TEST_ASSERT(queue != NULL, "No se pudo crear message queue");
    
    const char* test_data = "Hello World";
    bool sent = message_send(current->task_id, 1, test_data, strlen(test_data) + 1);
    TEST_ASSERT(sent == true, "No se pudo enviar mensaje");
    
    message_t received;
    bool received_ok = message_receive(&received, false);
    TEST_ASSERT(received_ok == true, "No se pudo recibir mensaje");
    TEST_ASSERT(received.type == 1, "Tipo de mensaje incorrecto");
    TEST_ASSERT(strcmp((char*)received.data, test_data) == 0, "Datos incorrectos");
    
    message_queue_destroy(queue);
    TEST_PASS();
}

static volatile int message_received_count = 0;
static volatile bool receiver_ready = false;

static void message_receiver_task(void* arg) {
    task_t* me = task_current();
    terminal_printf(&main_terminal, "[RECEIVER] Starting (ID=%u, name=%s)\r\n", 
                   me->task_id, me->name);
    
    // ✅ FIX: Asegurar que tenemos cola (debería estar creada por task_create)
    message_queue_t* my_queue = message_queue_get(me->task_id);
    if (!my_queue) {
        // Si no existe, crearla
        my_queue = message_queue_create(me->task_id);
        if (!my_queue) {
            terminal_puts(&main_terminal, "[RECEIVER] ERROR: Could not create queue!\r\n");
            task_exit(1);
            return;
        }
    }
    
    terminal_printf(&main_terminal, "[RECEIVER] Queue ready (count=%u)\r\n", my_queue->message_count);
    
    // ✅ FIX: Señalizar que estamos listos Y ceder CPU inmediatamente
    receiver_ready = true;
    task_yield(); // Dar CPU al emisor
    
    message_t msg;
    int received_count = 0;
    uint32_t start_time = ticks_since_boot;
    
    while (received_count < 10) {
        // ✅ FIX: Verificar primero sin mutex para eficiencia
        if (my_queue->message_count > 0) {
            if (message_receive(&msg, false)) {
                received_count++;
                message_received_count = received_count;
                
                terminal_printf(&main_terminal, 
                    "[RECEIVER] Got message %d (type=%u, data=%d)\r\n",
                    received_count, msg.type, *(int*)msg.data);
                
                // Pequeña pausa para ver los mensajes
                for (volatile int i = 0; i < 10000; i++);
            }
        } else {
            // ✅ FIX: Si no hay mensajes, yield activo
            task_yield();
            
            // Timeout de seguridad
            if (ticks_since_boot - start_time > 500) { // 5 segundos
                terminal_printf(&main_terminal, 
                    "[RECEIVER] TIMEOUT: Only received %d/10 messages after %u ticks\r\n",
                    received_count, ticks_since_boot - start_time);
                break;
            }
        }
    }
    
    terminal_printf(&main_terminal, "[RECEIVER] Finished! Received %d messages\r\n", received_count);
    task_exit(0);
}

static void test_message_blocking(void) {
    TEST_START("Message Blocking Receive");
    
    message_received_count = 0;
    receiver_ready = false;
    
    terminal_puts(&main_terminal, "\r\n[TEST] Initializing message system...\r\n");
    message_system_init();
    
    terminal_puts(&main_terminal, "[TEST] Creating receiver task...\r\n");
    
    task_t* receiver = task_create("msg_receiver", message_receiver_task, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT(receiver != NULL, "No se pudo crear receiver task");
    
    terminal_printf(&main_terminal, "[TEST] Receiver created with ID=%u, state=%d\r\n", 
                   receiver->task_id, receiver->state);
    
    // ✅ FIX: Esperar activamente con yields
    terminal_puts(&main_terminal, "[TEST] Waiting for receiver to be ready...\r\n");
    
    int wait_attempts = 0;
    while (!receiver_ready && wait_attempts < 100) {
        wait_attempts++;
        
        // ✅ CRÍTICO: Dar CPU al receiver múltiples veces
        for (int i = 0; i < 10; i++) {
            task_yield();
        }
        
        if (wait_attempts % 10 == 0) {
            terminal_printf(&main_terminal, 
                "[TEST] Waiting... attempt %d, receiver_state=%d, runtime=%u\r\n",
                wait_attempts, receiver->state, receiver->total_runtime);
        }
    }
    
    if (!receiver_ready) {
        terminal_printf(&main_terminal, 
            "[TEST] ERROR: Receiver never ready! State=%d, runtime=%u\r\n",
            receiver->state, receiver->total_runtime);
        TEST_FAIL("Receiver no se inicializó");
        return;
    }
    
    terminal_puts(&main_terminal, "[TEST] Receiver is ready! Sending messages...\r\n");
    
    // ✅ FIX: Verificar que la cola existe
    message_queue_t* receiver_queue = message_queue_get(receiver->task_id);
    if (!receiver_queue) {
        terminal_printf(&main_terminal, "[TEST] ERROR: No queue for task %u\r\n", receiver->task_id);
        TEST_FAIL("No hay cola para el receptor");
        return;
    }
    
    // Enviar mensajes con yields entre ellos
    for (int i = 0; i < 10; i++) {
        bool sent = message_send(receiver->task_id, 100 + i, &i, sizeof(i));
        terminal_printf(&main_terminal, 
            "[TEST] Message %d: %s (queue_count=%u)\r\n", 
            i, sent ? "OK" : "FAIL", receiver_queue->message_count);
        
        if (!sent) {
            terminal_printf(&main_terminal, "[TEST] RETRYING message %d...\r\n", i);
            // Reintentar
            sent = message_send(receiver->task_id, 100 + i, &i, sizeof(i));
        }
        
        // ✅ FIX: Dar CPU al receiver después de cada mensaje
        for (int j = 0; j < 5; j++) {
            task_yield();
        }
        
        // Pequeña pausa para ver el progreso
        for (volatile int k = 0; k < 5000; k++);
    }
    
    terminal_puts(&main_terminal, "[TEST] All messages sent, waiting for processing...\r\n");
    
    // Esperar a que se procesen
    uint32_t wait_start = ticks_since_boot;
    while (message_received_count < 10) {
        terminal_printf(&main_terminal, 
            "[TEST] Progress: %d/10 received, receiver_state=%d, queue_count=%u\r\n",
            message_received_count, receiver->state, receiver_queue->message_count);
        
        // ✅ FIX: Muchos yields para dar CPU
        for (int i = 0; i < 10; i++) {
            task_yield();
        }
        
        // Timeout de seguridad
        if (ticks_since_boot - wait_start > 1000) { // 10 segundos
            terminal_printf(&main_terminal, 
                "[TEST] TIMEOUT: Only %d/10 messages received after 10 seconds\r\n",
                message_received_count);
            break;
        }
        
        task_sleep(10);
    }
    
    TEST_ASSERT_FORMAT(message_received_count == 10, 
                      "Solo se recibieron %d/10 mensajes", message_received_count);
    
    // Cleanup
    task_sleep(100); // Dar tiempo para cleanup
    task_cleanup_zombies();
    
    TEST_PASS();
}

static void test_message_overflow(void) {
    TEST_START("Message System Overflow");
    
    task_t* current = task_current();
    message_queue_t* queue = message_queue_create(current->task_id);
    TEST_ASSERT(queue != NULL, "No se pudo crear queue");
    
    int messages_sent = 0;
    for (int i = 0; i < MAX_MESSAGES_PER_QUEUE + 5; i++) {
        if (message_send(current->task_id, i, &i, sizeof(i))) {
            messages_sent++;
        }
    }
    
    TEST_ASSERT_FORMAT(messages_sent == MAX_MESSAGES_PER_QUEUE, 
                      "No se respetó límite (enviados: %d, max: %d)",
                      messages_sent, MAX_MESSAGES_PER_QUEUE);
    
    // Limpiar
    message_t msg;
    while (message_receive(&msg, false)) {
        // Vaciar cola
    }
    
    message_queue_destroy(queue);
    TEST_PASS();
}

// ========================================================================
// TESTS DE PROFILING Y SALUD
// ========================================================================

static void test_profiling_basic(void) {
    TEST_START("Profiling Basic");
    
    task_profiling_enable();
    
    task_t* current = task_current();
    if (current) {
        for (int i = 0; i < 10; i++) {
            task_profiling_update(current, 5 + i);
            task_sleep(1);
        }
    }
    
    task_profiling_disable();
    TEST_PASS();
}

static void dummy_task(void* arg) {
    task_sleep(100);
    task_exit(0);
}

static void test_health_monitor(void) {
    TEST_START("Health Monitor");
    
    task_t* test1 = task_create("health_test1", dummy_task, NULL, TASK_PRIORITY_NORMAL);
    task_t* test2 = task_create("health_test2", dummy_task, NULL, TASK_PRIORITY_NORMAL);
    
    TEST_ASSERT(test1 != NULL, "No se pudo crear health_test1");
    TEST_ASSERT(test2 != NULL, "No se pudo crear health_test2");
    
    task_monitor_health();
    
    task_destroy(test1);
    task_destroy(test2);
    
    TEST_PASS();
}

static void test_zombie_cleanup(void) {
    TEST_START("Zombie Cleanup");
    
    int initial_task_count = scheduler.task_count;
    
    task_t* zombie1 = task_create("zombie1", dummy_task, NULL, TASK_PRIORITY_NORMAL);
    task_t* zombie2 = task_create("zombie2", dummy_task, NULL, TASK_PRIORITY_NORMAL);
    
    TEST_ASSERT(zombie1 != NULL, "No se pudo crear zombie1");
    TEST_ASSERT(zombie2 != NULL, "No se pudo crear zombie2");
    
    zombie1->state = TASK_ZOMBIE;
    zombie2->state = TASK_ZOMBIE;
    
    task_cleanup_zombies();
    
    TEST_ASSERT_FORMAT(scheduler.task_count == initial_task_count, 
                      "No se limpiaron todos los zombies (antes: %d, después: %d)",
                      initial_task_count + 2, scheduler.task_count);
    
    TEST_PASS();
}

static void test_context_dump(void) {
    TEST_START("Context Dump");
    
    task_t* current = task_current();
    if (current) {
        task_dump_context(current);
    }
    
    TEST_PASS();
}

// ========================================================================
// TEST DE DIAGNÓSTICO DEL SCHEDULER
// ========================================================================

static volatile int scheduler_test_counter = 0;

static void simple_counting_task(void* arg) {
    int task_num = (int)(uintptr_t)arg;
    
    terminal_printf(&main_terminal, "[COUNT_%d] Starting\r\n", task_num);
    
    for (int i = 0; i < 5; i++) {
        scheduler_test_counter++;
        terminal_printf(&main_terminal, "[COUNT_%d] Iteration %d, counter=%d\r\n", 
                       task_num, i, scheduler_test_counter);
        task_yield();
    }
    
    terminal_printf(&main_terminal, "[COUNT_%d] Finished\r\n", task_num);
    task_exit(0);
}

static void test_scheduler_basic(void) {
    TEST_START("Scheduler Basic Functionality");
    
    scheduler_test_counter = 0;
    
    terminal_puts(&main_terminal, "\r\n[TEST] Creating simple counting tasks...\r\n");
    
    task_t* task1 = task_create("count1", simple_counting_task, (void*)1, TASK_PRIORITY_NORMAL);
    task_t* task2 = task_create("count2", simple_counting_task, (void*)2, TASK_PRIORITY_NORMAL);
    
    TEST_ASSERT(task1 != NULL, "No se pudo crear count1");
    TEST_ASSERT(task2 != NULL, "No se pudo crear count2");
    
    terminal_puts(&main_terminal, "[TEST] Waiting for tasks to complete...\r\n");
    
    // Esperar a que terminen
    for (int i = 0; i < 200; i++) {
        if (i % 20 == 0) {
            terminal_printf(&main_terminal, "[TEST] Tick %d: counter=%d, states=[%d,%d]\r\n",
                           i, scheduler_test_counter, task1->state, task2->state);
        }
        
        if (task1->state == TASK_FINISHED && task2->state == TASK_FINISHED) {
            terminal_printf(&main_terminal, "[TEST] Both tasks finished at tick %d\r\n", i);
            break;
        }
        
        task_yield();
        task_sleep(10);
    }
    
    TEST_ASSERT_FORMAT(scheduler_test_counter == 10,
                      "Contador incorrecto (esperado: 10, actual: %d)",
                      scheduler_test_counter);
    
    task_cleanup_zombies();
    TEST_PASS();
}

// ========================================================================
// TEST RUNNER PRINCIPAL (ACTUALIZADO)
// ========================================================================

void run_task_utils_test_suite(void) {
    terminal_puts(&main_terminal, "\r\n\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    terminal_puts(&main_terminal, "     TASK_UTILS TEST SUITE - STARTING\r\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    
    tests_passed = 0;
    tests_failed = 0;
    
    // ✅ NUEVO: Test básico del scheduler primero
    terminal_puts(&main_terminal, "\r\n--- SCHEDULER TESTS ---\r\n");
    test_scheduler_basic();
    
    // Tests de mutex
    terminal_puts(&main_terminal, "\r\n--- MUTEX TESTS ---\r\n");
    test_mutex_basic();
    test_mutex_reentrancy();
    test_mutex_race_condition();
    
    // Tests de mensajes
    terminal_puts(&main_terminal, "\r\n--- MESSAGE TESTS ---\r\n");
    test_message_system_basic();
    test_message_overflow();
    test_message_blocking();
    
    // Tests de profiling y salud
    terminal_puts(&main_terminal, "\r\n--- PROFILING & HEALTH TESTS ---\r\n");
    test_profiling_basic();
    test_health_monitor();
    test_zombie_cleanup();
    test_context_dump();
    
    // Resultados finales
    terminal_puts(&main_terminal, "\r\n\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    terminal_puts(&main_terminal, "     TEST RESULTS\r\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    terminal_printf(&main_terminal, "Tests Passed: %d\r\n", tests_passed);
    terminal_printf(&main_terminal, "Tests Failed: %d\r\n", tests_failed);
    terminal_printf(&main_terminal, "Total Tests:  %d\r\n", tests_passed + tests_failed);
    
    if (tests_failed == 0) {
        terminal_puts(&main_terminal, "✅ ALL TESTS PASSED!\r\n");
    } else {
        terminal_puts(&main_terminal, "❌ SOME TESTS FAILED!\r\n");
    }
    
    terminal_puts(&main_terminal, "\r\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    terminal_puts(&main_terminal, "     SYSTEM HEALTH CHECK\r\n");
    terminal_puts(&main_terminal, "===============================================\r\n");
    task_monitor_health();
    show_system_stats();
}

void test_task_utils_command(void) {
    terminal_puts(&main_terminal, "\r\nStarting task_utils test suite...\r\n");
    run_task_utils_test_suite();
}