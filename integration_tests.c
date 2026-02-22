// integration_tests.c
// Tests de integración que demuestran que los mecanismos de sincronización
// del sistema están funcionando correctamente.
//
// Principio: un test que no puede FALLAR no es un test real.
// Cada test aquí verifica una condición concreta y reporta PASS/FAIL.

#include "task_test.h"
#include "task_utils.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

extern Terminal main_terminal;
extern uint32_t ticks_since_boot;

static int it_passed = 0;
static int it_failed = 0;

#define IT_PASS(name) do { \
    terminal_printf(&main_terminal, "  [PASS] %s\r\n", name); \
    serial_printf(COM1_BASE, "[ITEST PASS] %s\r\n", name); \
    it_passed++; \
} while(0)

#define IT_FAIL(name, reason) do { \
    terminal_printf(&main_terminal, "  [FAIL] %s: %s\r\n", name, reason); \
    serial_printf(COM1_BASE, "[ITEST FAIL] %s: %s\r\n", name, reason); \
    it_failed++; \
} while(0)

#define IT_CHECK(name, cond, reason) do { \
    if (cond) { IT_PASS(name); } else { IT_FAIL(name, reason); } \
} while(0)

// ============================================================================
// TEST 1: Mutex básico - lock/unlock y estado interno
// ============================================================================
static void itest_mutex_state(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 1. Estado interno del mutex\r\n");

    mutex_t m;
    mutex_init(&m, "itest_m");

    IT_CHECK("init: !locked",     m.locked == false,      "mutex debería empezar desbloqueado");
    IT_CHECK("init: lock_count=0", m.lock_count == 0,     "lock_count inicial debe ser 0");
    IT_CHECK("init: owner=NULL",  m.owner == NULL,        "owner inicial debe ser NULL");

    bool got = mutex_try_lock(&m);
    IT_CHECK("try_lock: returns true",  got == true,      "try_lock en mutex libre debe retornar true");
    IT_CHECK("try_lock: locked=true",   m.locked == true, "mutex debe estar locked tras try_lock");
    IT_CHECK("try_lock: lock_count=1",  m.lock_count == 1,"lock_count debe ser 1 tras first lock");
    IT_CHECK("try_lock: owner=current", m.owner == task_current(), "owner debe ser la tarea actual");

    // Reentrancia
    bool reentrant = mutex_try_lock(&m);
    IT_CHECK("reentrant: returns true",  reentrant == true,  "reentrancia debe retornar true");
    IT_CHECK("reentrant: lock_count=2",  m.lock_count == 2,  "lock_count debe ser 2 tras segundo lock");

    mutex_unlock(&m);
    IT_CHECK("unlock1: still locked",  m.locked == true,  "unlock con lock_count=2 no debe desbloquear");
    IT_CHECK("unlock1: lock_count=1",  m.lock_count == 1, "lock_count debe bajar a 1 tras primer unlock");

    mutex_unlock(&m);
    IT_CHECK("unlock2: !locked",       m.locked == false, "mutex debe estar libre tras segundo unlock");
    IT_CHECK("unlock2: lock_count=0",  m.lock_count == 0, "lock_count debe ser 0 al final");
    IT_CHECK("unlock2: owner=NULL",    m.owner == NULL,   "owner debe limpiarse al desbloquear");
}

// ============================================================================
// TEST 2: Reentrancia real - misma tarea puede bloquear N veces
// ============================================================================
static mutex_t reentrant_mutex;
static volatile int reentrant_depth = 0;

static void recursive_lock_task(void *arg) {
    int depth = (int)(uintptr_t)arg;
    mutex_lock(&reentrant_mutex);
    reentrant_depth++;
    
    if (depth > 1) {
        // Recursión: intentar bloquear de nuevo desde la misma tarea
        mutex_lock(&reentrant_mutex);
        reentrant_depth++;
        mutex_unlock(&reentrant_mutex);
        reentrant_depth--;
    }
    
    mutex_unlock(&reentrant_mutex);
    reentrant_depth--;
    task_exit(0);
}

static void itest_mutex_reentrancy_task(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 2. Reentrancia en tarea separada\r\n");
    
    mutex_init(&reentrant_mutex, "itest_reentrant");
    reentrant_depth = 0;
    
    // Crear tarea que hace bloqueo reentrante en profundidad 2
    task_t *t = task_create("reentrant_t", recursive_lock_task, 
                            (void*)(uintptr_t)2, TASK_PRIORITY_HIGH);
    IT_CHECK("create: task exists", t != NULL, "no se pudo crear tarea reentrante");
    
    if (t) {
        // Esperar a que termine
        for (int i = 0; i < 200; i++) {
            task_sleep(10);
            if (t->state == TASK_FINISHED || t->state == TASK_ZOMBIE) break;
        }
        IT_CHECK("reentrant task: completed",
                 t->state == TASK_FINISHED || t->state == TASK_ZOMBIE,
                 "la tarea debería haber terminado");
    }
    
    // El mutex debe quedar libre
    IT_CHECK("after task: mutex free", reentrant_mutex.locked == false,
             "el mutex debe liberarse completamente al salir la tarea");
    IT_CHECK("after task: lock_count=0", reentrant_mutex.lock_count == 0,
             "lock_count debe ser 0 tras salir la tarea");
}

// ============================================================================
// TEST 3: Exclusión mutua real - dos tareas NO deben solaparse
// ============================================================================
static mutex_t exclusion_mutex;
static volatile int exclusion_inside = 0;   // tareas simultáneas dentro
static volatile int exclusion_max    = 0;   // máximo simultáneo detectado
static volatile int exclusion_total  = 0;   // operaciones completadas

static void exclusion_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&exclusion_mutex);
        
        exclusion_inside++;
        if (exclusion_inside > exclusion_max) {
            exclusion_max = exclusion_inside;
        }
        
        // Simular trabajo (ceder CPU para maximizar probabilidad de solapamiento)
        task_yield();
        
        exclusion_inside--;
        exclusion_total++;
        
        mutex_unlock(&exclusion_mutex);
        task_yield(); // Dar oportunidad a la otra tarea
    }
    task_exit(0);
}

static void itest_mutual_exclusion(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 3. Exclusion mutua (dos tareas)\r\n");

    mutex_init(&exclusion_mutex, "itest_exclusion");
    exclusion_inside = 0;
    exclusion_max    = 0;
    exclusion_total  = 0;

    task_t *t1 = task_create("excl1", exclusion_task, NULL, TASK_PRIORITY_HIGH);
    task_t *t2 = task_create("excl2", exclusion_task, NULL, TASK_PRIORITY_HIGH);

    IT_CHECK("create t1", t1 != NULL, "no se pudo crear excl1");
    IT_CHECK("create t2", t2 != NULL, "no se pudo crear excl2");

    if (t1 && t2) {
        // Esperar hasta 10 segundos
        for (int i = 0; i < 200; i++) {
            task_sleep(50);
            if ((t1->state == TASK_FINISHED || t1->state == TASK_ZOMBIE) &&
                (t2->state == TASK_FINISHED || t2->state == TASK_ZOMBIE)) {
                break;
            }
        }

        IT_CHECK("both tasks finished",
                 (t1->state == TASK_FINISHED || t1->state == TASK_ZOMBIE) &&
                 (t2->state == TASK_FINISHED || t2->state == TASK_ZOMBIE),
                 "ambas tareas deben terminar");

        IT_CHECK("total ops = 100", exclusion_total == 100,
                 "deben completarse exactamente 100 operaciones (50 x 2 tareas)");

        // ¡CRÍTICO! Si el mutex funciona, nunca entran dos tareas a la vez
        IT_CHECK("max simultaneous = 1", exclusion_max <= 1,
                 "RACE CONDITION: dos tareas entraron a la sección crítica a la vez!");
        
        terminal_printf(&main_terminal, "  [INFO] max_simultaneous=%d, total_ops=%d\r\n",
                        exclusion_max, exclusion_total);
    }
}

// ============================================================================
// TEST 4: VFS - escritura concurrente en tmpfs es segura
// ============================================================================
static mutex_t vfs_test_mutex;
static volatile int vfs_write_count = 0;

static void vfs_writer_task(void *arg) {
    int task_id = (int)(uintptr_t)arg;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/itest_%d.txt", task_id);
    
    // Abrir/crear fichero
    int fd = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT);
    if (fd >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Task %d was here\n", task_id);
        vfs_write(fd, buf, strlen(buf));
        vfs_close(fd);
        
        mutex_lock(&vfs_test_mutex);
        vfs_write_count++;
        mutex_unlock(&vfs_test_mutex);
    }
    task_exit(0);
}

static void itest_vfs_concurrent(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 4. Escritura concurrente en VFS (tmpfs)\r\n");

    mutex_init(&vfs_test_mutex, "itest_vfs");
    vfs_write_count = 0;

    // Intentar montar tmpfs en /tmp si no existe
    vfs_mount("/tmp", "tmpfs", NULL);

    task_t *writers[4];
    int created = 0;
    for (int i = 0; i < 4; i++) {
        writers[i] = task_create("vfs_writer", vfs_writer_task,
                                 (void*)(uintptr_t)(i + 1), TASK_PRIORITY_HIGH);
        if (writers[i]) created++;
    }

    IT_CHECK("writers created", created == 4, "deben crearse las 4 tareas escritoras");

    if (created > 0) {
        // Esperar hasta 5 segundos
        for (int i = 0; i < 100; i++) {
            task_sleep(50);
            bool all_done = true;
            for (int j = 0; j < 4; j++) {
                if (writers[j] && writers[j]->state != TASK_FINISHED &&
                    writers[j]->state != TASK_ZOMBIE) {
                    all_done = false;
                }
            }
            if (all_done) break;
        }

        IT_CHECK("all writers finished", vfs_write_count == created,
                 "todos los escritores deben haber completado");
        terminal_printf(&main_terminal, "  [INFO] writes_completed=%d/%d\r\n",
                        vfs_write_count, created);

        // Verificar que los ficheros existen y son legibles
        char verify_buf[64];
        int fd = vfs_open("/tmp/itest_1.txt", VFS_O_RDONLY);
        bool readable = (fd >= 0);
        if (fd >= 0) {
            int got = vfs_read(fd, verify_buf, sizeof(verify_buf) - 1);
            readable = (got > 0);
            vfs_close(fd);
        }
        IT_CHECK("file readable after concurrent writes", readable,
                 "el fichero debe ser legible tras escrituras concurrentes");
    }
}

// ============================================================================
// TEST 5: Terminal mutex - múltiples tareas escriben y no hay corrupción
// ============================================================================
static volatile int terminal_lines_written = 0;
static mutex_t terminal_count_mutex;

static void terminal_writer_task(void *arg) {
    int id = (int)(uintptr_t)arg;
    for (int i = 0; i < 5; i++) {
        // terminal_printf ya usa mutex internamente; esto prueba que
        // múltiples llamadas concurrentes no crashean
        terminal_printf(&main_terminal, "  [TW_%d] Line %d/5\r\n", id, i + 1);
        
        mutex_lock(&terminal_count_mutex);
        terminal_lines_written++;
        mutex_unlock(&terminal_count_mutex);
        
        task_yield();
    }
    task_exit(0);
}

static void itest_terminal_concurrent(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 5. Escritura concurrente en terminal\r\n");

    mutex_init(&terminal_count_mutex, "itest_term");
    terminal_lines_written = 0;

    task_t *tw[3];
    for (int i = 0; i < 3; i++) {
        tw[i] = task_create("term_writer", terminal_writer_task,
                             (void*)(uintptr_t)(i + 1), TASK_PRIORITY_HIGH);
    }

    // Esperar a que terminen
    for (int i = 0; i < 100; i++) {
        task_sleep(50);
        bool done = true;
        for (int j = 0; j < 3; j++) {
            if (tw[j] && tw[j]->state != TASK_FINISHED &&
                tw[j]->state != TASK_ZOMBIE) done = false;
        }
        if (done) break;
    }

    // 3 tareas × 5 líneas = 15
    IT_CHECK("all lines written", terminal_lines_written == 15,
             "deben escribirse exactamente 15 lineas (3 tareas x 5)");
    terminal_printf(&main_terminal, "  [INFO] lines_written=%d/15\r\n",
                    terminal_lines_written);
}

// ============================================================================
// TEST 6: Message queue - limpieza automática al destruir tarea
// ============================================================================
static void message_consumer_task(void *arg) {
    (void)arg;
    // Solo existir y salir — la cola debe limpiarse automáticamente
    task_sleep(50);
    task_exit(0);
}

static void itest_message_queue_cleanup(void) {
    terminal_puts(&main_terminal, "\r\n[ITEST] 6. Limpieza de cola de mensajes\r\n");

    message_system_init();

    // Contar colas ocupadas antes
    int before = 0;
    for (int i = 0; i < 8; i++) {
        // Crear tarea temporal y sacar su cola
        task_t *t = task_create("mq_cleanup_t", message_consumer_task,
                                NULL, TASK_PRIORITY_NORMAL);
        if (t) {
            before++;
            // Esperar a que salga para que la cola se limpie
            for (int j = 0; j < 100; j++) {
                task_sleep(20);
                if (t->state == TASK_FINISHED || t->state == TASK_ZOMBIE) break;
            }
            task_cleanup_zombies();
            
            // La cola debería haberse liberado
            message_queue_t *q = message_queue_get(t->task_id);
            IT_CHECK("queue freed after task exit", q == NULL,
                     "la cola debe estar libre tras destruir la tarea");
        }
    }

    IT_CHECK("test ran at all", before > 0, "deben crearse tareas de prueba");
}

// ============================================================================
// RUNNER PRINCIPAL
// ============================================================================
void run_integration_tests(void) {
    terminal_puts(&main_terminal, "\r\n");
    terminal_puts(&main_terminal, "╔══════════════════════════════════════════╗\r\n");
    terminal_puts(&main_terminal, "║   INTEGRATION TEST SUITE - AlvOS Sync   ║\r\n");
    terminal_puts(&main_terminal, "╚══════════════════════════════════════════╝\r\n");

    it_passed = 0;
    it_failed = 0;

    itest_mutex_state();
    itest_mutex_reentrancy_task();
    itest_mutual_exclusion();
    itest_vfs_concurrent();
    itest_terminal_concurrent();
    itest_message_queue_cleanup();

    // ── Resumen ──
    terminal_puts(&main_terminal, "\r\n");
    terminal_puts(&main_terminal, "══════════════════════════════════════════\r\n");
    terminal_printf(&main_terminal, "  PASSED : %d\r\n", it_passed);
    terminal_printf(&main_terminal, "  FAILED : %d\r\n", it_failed);
    terminal_printf(&main_terminal, "  TOTAL  : %d\r\n", it_passed + it_failed);
    terminal_puts(&main_terminal, "══════════════════════════════════════════\r\n");

    if (it_failed == 0) {
        terminal_puts(&main_terminal, "  ✅ ALL INTEGRATION TESTS PASSED!\r\n");
    } else {
        terminal_printf(&main_terminal,
                        "  ❌ %d TEST(S) FAILED - Check serial log!\r\n", it_failed);
    }
    terminal_puts(&main_terminal, "\r\n");
}
