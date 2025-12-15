// disk_io_daemon.c
#include "disk_io_daemon.h"
#include "task.h"
#include "task_utils.h"
#include "disk.h"
#include "kernel.h"
#include "terminal.h"
#include "memory.h"
#include "irq.h"
#include "log.h"

// Definiciones de mensajes
#define MSG_DISK_READ_REQUEST   100
#define MSG_DISK_WRITE_REQUEST  101
#define MSG_DISK_FLUSH_REQUEST  102
#define MSG_DISK_RESPONSE       103

// Estructura de solicitud de disco
typedef struct {
    disk_t* disk;
    uint64_t lba;
    uint32_t sector_count;
    void* buffer;           // Para lecturas: buffer donde escribir
                           // Para escrituras: buffer a leer
    uint32_t requester_id;
    uint32_t request_id;    // ID único de la solicitud
} disk_request_t;

// Estructura de respuesta
typedef struct {
    disk_err_t result;
    uint32_t request_id;
    uint32_t sectors_processed;
} disk_response_t;

static uint32_t next_request_id = 1;
static task_t* disk_io_task = NULL;

// ========================================================================
// FUNCIONES INTERNAS DEL DAEMON
// ========================================================================

static void process_read_request(disk_request_t* req) {
    if (!req || !req->disk || !req->buffer) {
        log_message(LOG_ERROR, "[DISK_IO] Invalid read request parameters\r\n");
        return;
    }
    
    log_message(LOG_INFO, 
        "[DISK_IO] → Reading: LBA %llu, count %u, buffer 0x%08x, requester %u\r\n",
        req->lba, req->sector_count, (uint32_t)req->buffer, req->requester_id);
    
    // Verificar tipo de disco
    disk_err_t err;
    if (disk_is_atapi(req->disk)) {
        log_message(LOG_INFO, "[DISK_IO] Device type: ATAPI\r\n");
        err = disk_read_dispatch(req->disk, req->lba, req->sector_count, req->buffer);
    } else if (req->disk->type == DEVICE_TYPE_SATA_DISK) {
        log_message(LOG_INFO, "[DISK_IO] Device type: SATA\r\n");
        err = disk_read_dispatch(req->disk, req->lba, req->sector_count, req->buffer);
    } else {
        log_message(LOG_INFO, "[DISK_IO] Device type: IDE\r\n");
        err = disk_read_dispatch(req->disk, req->lba, req->sector_count, req->buffer);
    }
    
    log_message(LOG_INFO, "[DISK_IO] ← Read result: %d\r\n", err);
    
    // Preparar respuesta
    disk_response_t response = {
        .result = err,
        .request_id = req->request_id,
        .sectors_processed = (err == DISK_ERR_NONE) ? req->sector_count : 0
    };
    
    log_message(LOG_INFO, 
        "[DISK_IO] Sending response to task %u (req_id: %u)\r\n",
        req->requester_id, req->request_id);
    
    // Enviar respuesta al solicitante
    if (!message_send(req->requester_id, MSG_DISK_RESPONSE, &response, sizeof(response))) {
        log_message(LOG_ERROR, 
            "[DISK_IO] Failed to send response to task %u\r\n", req->requester_id);
    } else {
        log_message(LOG_INFO, "[DISK_IO] Response sent successfully\r\n");
    }
}

static void process_write_request(disk_request_t* req) {
    if (!req || !req->disk || !req->buffer) {
        log_message(LOG_WARN, "[DISK_IO] Invalid write request\r\n");
        return;
    }
    
    // ATAPI devices son read-only
    if (disk_is_atapi(req->disk)) {
        log_message(LOG_WARN, "[DISK_IO] Write not supported on ATAPI device\r\n");
        
        disk_response_t response = {
            .result = DISK_ERR_ATAPI,
            .request_id = req->request_id,
            .sectors_processed = 0
        };
        message_send(req->requester_id, MSG_DISK_RESPONSE, &response, sizeof(response));
        return;
    }
    
    // Verificar tipo de disco
    disk_err_t err;
    if (req->disk->type == DEVICE_TYPE_SATA_DISK) {
        log_message(LOG_INFO, 
            "[DISK_IO] Writing %u sectors to SATA disk at LBA %llu\r\n",
            req->sector_count, req->lba);
        err = disk_write_dispatch(req->disk, req->lba, req->sector_count, req->buffer);
    } else {
        log_message(LOG_INFO, 
            "[DISK_IO] Writing %u sectors to IDE disk at LBA %llu\r\n",
            req->sector_count, req->lba);
        err = disk_write_dispatch(req->disk, req->lba, req->sector_count, req->buffer);
    }
    
    // Preparar respuesta
    disk_response_t response = {
        .result = err,
        .request_id = req->request_id,
        .sectors_processed = (err == DISK_ERR_NONE) ? req->sector_count : 0
    };
    
    // Enviar respuesta
    if (!message_send(req->requester_id, MSG_DISK_RESPONSE, &response, sizeof(response))) {
        log_message(LOG_ERROR, 
            "[DISK_IO] Failed to send response to task %u\r\n", req->requester_id);
    }
}

static void process_flush_request(disk_request_t* req) {
    if (!req || !req->disk) {
        log_message(LOG_WARN, "[DISK_IO] Invalid flush request\r\n");
        return;
    }
    
    log_message(LOG_INFO, 
        "[DISK_IO] Flushing disk cache (drive 0x%02x)\r\n", req->disk->drive_number);
    
    disk_err_t err = disk_flush_dispatch(req->disk);
    
    disk_response_t response = {
        .result = err,
        .request_id = req->request_id,
        .sectors_processed = 0
    };
    
    if (!message_send(req->requester_id, MSG_DISK_RESPONSE, &response, sizeof(response))) {
        log_message(LOG_ERROR, 
            "[DISK_IO] Failed to send flush response to task %u\r\n", req->requester_id);
    }
}

// ========================================================================
// FUNCIÓN PRINCIPAL DEL DAEMON
// ========================================================================

void disk_io_daemon(void* arg) {
    (void)arg;
    message_t msg;
    uint32_t requests_processed = 0;
    uint32_t last_activity = ticks_since_boot;
    
    log_message(LOG_INFO, "[DISK_IO] Daemon started (task ID: %u)\n", 
                   task_current()->task_id);
    
    // ✅ FIX: Asegurar que estamos en estado READY
    task_current()->state = TASK_READY;
    
    while (1) {
        // ✅ FIX: Usar receive NO BLOQUEANTE con yield
        if (message_receive(&msg, false)) {
            disk_request_t* req = (disk_request_t*)msg.data;
            last_activity = ticks_since_boot;
            
            switch (msg.type) {
                case MSG_DISK_READ_REQUEST:
                    process_read_request(req);
                    requests_processed++;
                    break;
                    
                case MSG_DISK_WRITE_REQUEST:
                    process_write_request(req);
                    requests_processed++;
                    break;
                    
                case MSG_DISK_FLUSH_REQUEST:
                    process_flush_request(req);
                    requests_processed++;
                    break;
                    
                default:
                    log_message(LOG_INFO, 
                        "[DISK_IO] Unknown message type: %u\n", msg.type);
                    break;
            }
            
            // Debug cada 10 requests
            if (requests_processed % 10 == 0) {
                log_message(LOG_INFO, 
                    "[DISK_IO] Processed %u requests\n", requests_processed);
            }
        } else {
            // ✅ FIX: Si no hay mensajes, sleep corto en lugar de busy-wait
            task_sleep(1);
        }
        
        // ✅ FIX: Yield después de cada iteración
        task_yield();
        
        // ✅ FIX: Verificar timeout de inactividad (prevenir deadlocks)
        if (ticks_since_boot - last_activity > 1000) { // 10 segundos
            log_message(LOG_INFO, 
                "[DISK_IO] Activity timeout, resetting state\n");
            last_activity = ticks_since_boot;
            
            // Forzar estado READY por si acaso
            task_current()->state = TASK_READY;
        }
    }
}

// ========================================================================
// API PÚBLICA PARA OTRAS TAREAS
// ========================================================================

disk_err_t async_disk_read(disk_t* disk, uint64_t lba, uint32_t count, void* buffer) {
    // ✅ CRÍTICO: Verificar que estamos en una tarea con cola de mensajes
    task_t* current = task_current();
    if (!current) {
        log_message(LOG_WARN, "[DISK_IO] No current task, using sync I/O\r\n");
        return disk_read_dispatch(disk, lba, count, buffer);
    }
    
    if (!disk_io_task) {
        log_message(LOG_WARN, "[DISK_IO] Daemon not running, using synchronous I/O\r\n");
        return disk_read_dispatch(disk, lba, count, buffer);
    }
    
    // Verificar que el daemon esté vivo
    if (disk_io_task->state == TASK_FINISHED || disk_io_task->state == TASK_ZOMBIE) {
        log_message(LOG_WARN, "[DISK_IO] Daemon dead, using synchronous I/O\r\n");
        return disk_read_dispatch(disk, lba, count, buffer);
    }
    
    // ✅ Verificar que nuestra tarea tenga cola de mensajes
    message_queue_t* my_queue = message_queue_get(current->task_id);
    if (!my_queue) {
        log_message(LOG_INFO, "[DISK_IO] Task %s has no message queue, creating one...\r\n", 
                       current->name);
        my_queue = message_queue_create(current->task_id);
        if (!my_queue) {
            log_message(LOG_ERROR, "[DISK_IO] Failed to create queue, using sync I/O\r\n");
            return disk_read_dispatch(disk, lba, count, buffer);
        }
    }
    
    // Crear solicitud
    disk_request_t req = {
        .disk = disk,
        .lba = lba,
        .sector_count = count,
        .buffer = buffer,
        .requester_id = current->task_id,
        .request_id = next_request_id++
    };
    
    log_message(LOG_INFO, "[DISK_IO] Task %s (ID:%u) sending read request %u to disk_io (ID:%u)\r\n", 
                   current->name, current->task_id, req.request_id, disk_io_task->task_id);
    
    // Enviar solicitud
    if (!message_send(disk_io_task->task_id, MSG_DISK_READ_REQUEST, &req, sizeof(req))) {
        log_message(LOG_ERROR, "[DISK_IO] Failed to send read request\r\n");
        return DISK_ERR_ATA;
    }
    
    log_message(LOG_INFO, "[DISK_IO] Request sent, waiting for response...\r\n");
    
    // ✅ Esperar respuesta con timeout
    uint32_t start_tick = ticks_since_boot;
    uint32_t timeout_ticks = 100; // 5 segundos a 100Hz
    uint32_t attempts = 0;
    
    while ((ticks_since_boot - start_tick) < timeout_ticks) {
        attempts++;
        
        task_yield();
        
        // Intentar recibir mensaje (NON-blocking)
        message_t response_msg;
        if (message_receive(&response_msg, false)) {
            log_message(LOG_INFO, "[DISK_IO] Received message type %u\r\n", response_msg.type);
            
            if (response_msg.type == MSG_DISK_RESPONSE) {
                disk_response_t* response = (disk_response_t*)response_msg.data;
                log_message(LOG_INFO, "[DISK_IO] Response: req_id=%u (expected %u), result=%d\r\n",
                               response->request_id, req.request_id, response->result);
                
                if (response->request_id == req.request_id) {
                    log_message(LOG_INFO, "[DISK_IO] ✓ Got correct response after %u attempts\r\n", 
                                   attempts);
                    return response->result;
                } else {
                    log_message(LOG_ERROR, "[DISK_IO] ✗ Wrong request_id, continuing wait...\r\n");
                }
            } else {
                log_message(LOG_ERROR, "[DISK_IO] ✗ Wrong message type, continuing wait...\r\n");
            }
        }
        
        // Debug periódico
        if (attempts % 100 == 0) {
            log_message(LOG_INFO, "[DISK_IO] Still waiting... (attempt %u, elapsed %u ticks, daemon state: %d)\r\n",
                           attempts, ticks_since_boot - start_tick, disk_io_task->state);
            
            // Verificar estado de la cola del daemon
            message_queue_t* daemon_queue = message_queue_get(disk_io_task->task_id);
            if (daemon_queue) {
                log_message(LOG_INFO, "[DISK_IO]   Daemon queue: %u pending messages\r\n", 
                               daemon_queue->message_count);
            }
            
            // Verificar nuestra cola
            log_message(LOG_INFO, "[DISK_IO]   Our queue: %u pending messages\r\n", 
                           my_queue->message_count);
        }
        
        // Sleep corto
        task_sleep(1);
    }
    
    log_message(LOG_ERROR, "[DISK_IO] ✗ Timeout after %u attempts (%u ticks)\r\n", 
                   attempts, ticks_since_boot - start_tick);
    return DISK_ERR_TIMEOUT;
}

disk_err_t async_disk_write(disk_t* disk, uint64_t lba, uint32_t count, const void* buffer) {
    // ✅ FIX: Verificaciones más robustas
    if (!disk_io_task) {
        log_message(LOG_WARN, "[DISK_IO] Daemon not running, using synchronous I/O\n");
        return disk_write_dispatch(disk, lba, count, buffer);
    }
    
    // Verificar que el daemon esté vivo y en estado válido
    if (disk_io_task->state == TASK_FINISHED || disk_io_task->state == TASK_ZOMBIE) {
        log_message(LOG_WARN, "[DISK_IO] Daemon dead, using synchronous I/O\n");
        return disk_write_dispatch(disk, lba, count, buffer);
    }
    
    task_t* current = task_current();
    if (!current) {
        log_message(LOG_WARN, "[DISK_IO] No current task, using sync I/O\n");
        return disk_write_dispatch(disk, lba, count, buffer);
    }
    
    // ✅ FIX: Asegurar que tenemos cola de mensajes
    message_queue_t* my_queue = message_queue_get(current->task_id);
    if (!my_queue) {
        log_message(LOG_INFO, "[DISK_IO] Task %s has no message queue, creating one...\n", 
                       current->name);
        my_queue = message_queue_create(current->task_id);
        if (!my_queue) {
            log_message(LOG_WARN, "[DISK_IO] Failed to create queue, using sync I/O\n");
            return disk_write_dispatch(disk, lba, count, buffer);
        }
    }
    
    // Crear solicitud
    disk_request_t req = {
        .disk = disk,
        .lba = lba,
        .sector_count = count,
        .buffer = (void*)buffer,  // Cast away const
        .requester_id = current->task_id,
        .request_id = next_request_id++
    };
    
    log_message(LOG_INFO, 
        "[DISK_IO] Task %s sending write request %u to disk_io\n", 
        current->name, req.request_id);
    
    // ✅ FIX: Verificar estado del daemon antes de enviar
    log_message(LOG_INFO, "[DISK_IO] Daemon state: %d\n", disk_io_task->state);
    
    // Enviar solicitud
    if (!message_send(disk_io_task->task_id, MSG_DISK_WRITE_REQUEST, &req, sizeof(req))) {
        log_message(LOG_ERROR, "[DISK_IO] Failed to send write request\n");
        return DISK_ERR_ATA;
    }
    
    log_message(LOG_INFO, "[DISK_IO] Write request sent, waiting for response...\n");
    
    // ✅ FIX: Esperar respuesta con mejor manejo de timeouts
    uint32_t start_tick = ticks_since_boot;
    uint32_t timeout_ticks = 1000; // 10 segundos a 100Hz
    uint32_t attempts = 0;
    
    while ((ticks_since_boot - start_tick) < timeout_ticks) {
        attempts++;
        
        // ✅ FIX: Yield periódicamente para permitir que el daemon se ejecute
        if (attempts % 5 == 0) {
            task_yield();
        }
        
        // Intentar recibir mensaje (non-blocking)
        message_t response_msg;
        if (message_receive(&response_msg, false)) {
            log_message(LOG_INFO, "[DISK_IO] Received message type %u\n", response_msg.type);
            
            if (response_msg.type == MSG_DISK_RESPONSE) {
                disk_response_t* response = (disk_response_t*)response_msg.data;
                log_message(LOG_INFO, 
                    "[DISK_IO] Response: req_id=%u (expected %u), result=%d\n",
                    response->request_id, req.request_id, response->result);
                
                if (response->request_id == req.request_id) {
                    log_message(LOG_INFO, 
                        "[DISK_IO] ✓ Got correct write response after %u attempts\n", 
                        attempts);
                    return response->result;
                } else {
                    log_message(LOG_ERROR, 
                        "[DISK_IO] ✗ Wrong request_id (got %u, expected %u), continuing...\n",
                        response->request_id, req.request_id);
                    // ✅ Re-enqueue el mensaje para el destinatario correcto
                    message_send(response_msg.sender_id, response_msg.type, 
                                response_msg.data, response_msg.size);
                }
            } else {
                log_message(LOG_ERROR, 
                    "[DISK_IO] ✗ Wrong message type %u, re-queueing...\n", response_msg.type);
                // Re-enqueue mensajes que no son para nosotros
                message_send(response_msg.sender_id, response_msg.type, 
                            response_msg.data, response_msg.size);
            }
        }
        
        // ✅ FIX: Debug más informativo
        if (attempts % 50 == 0) {
            uint32_t elapsed = ticks_since_boot - start_tick;
            log_message(LOG_INFO,
                "[DISK_IO] Still waiting for write response... (attempt %u, elapsed %u ticks)\n",
                attempts, elapsed);
            log_message(LOG_INFO, 
                "[DISK_IO]   Daemon state: %d, Our queue: %u messages\n",
                disk_io_task->state, my_queue->message_count);
            
            // Verificar cola del daemon
            message_queue_t* daemon_queue = message_queue_get(disk_io_task->task_id);
            if (daemon_queue) {
                log_message(LOG_INFO, 
                    "[DISK_IO]   Daemon queue: %u pending messages\n", 
                    daemon_queue->message_count);
            }
        }
        
        // Sleep corto
        task_sleep(10);
    }
    
    log_message(LOG_ERROR, 
        "[DISK_IO] ✗ Write timeout after %u attempts (%u ticks)\n", 
        attempts, ticks_since_boot - start_tick);
    return DISK_ERR_TIMEOUT;
}

disk_err_t async_disk_flush(disk_t* disk) {
    if (!disk_io_task) {
        return disk_flush_dispatch(disk);
    }
    
    disk_request_t req = {
        .disk = disk,
        .lba = 0,
        .sector_count = 0,
        .buffer = NULL,
        .requester_id = task_current()->task_id,
        .request_id = next_request_id++
    };
    
    if (!message_send(disk_io_task->task_id, MSG_DISK_FLUSH_REQUEST, &req, sizeof(req))) {
        log_message(LOG_ERROR, "[DISK_IO] Failed to send flush request\r\n");
        return DISK_ERR_ATA;
    }
    
    message_t response_msg;
    uint32_t timeout = ticks_since_boot + 200;
    
    while (ticks_since_boot < timeout) {
        if (message_receive(&response_msg, false)) {
            if (response_msg.type == MSG_DISK_RESPONSE) {
                disk_response_t* response = (disk_response_t*)response_msg.data;
                if (response->request_id == req.request_id) {
                    return response->result;
                }
            }
        }
        task_sleep(10);
    }
    
    return DISK_ERR_TIMEOUT;
}

void disk_io_daemon_init(void) {
    disk_io_task = task_create("disk_io", disk_io_daemon, NULL, TASK_PRIORITY_NORMAL);
    
    if (disk_io_task) {
        log_message(LOG_INFO, 
            "[DISK_IO] Daemon initialized (task ID: %u)\r\n", disk_io_task->task_id);
    } else {
        log_message(LOG_ERROR, "[DISK_IO] Failed to create daemon task\r\n");
    }
}

void cmd_async_read_test(void) {
    terminal_puts(&main_terminal, "\r\n=== Async Disk Read Test ===\r\n");
    
    uint8_t* buffer = (uint8_t*)kernel_malloc(512);
    if (!buffer) {
        terminal_puts(&main_terminal, "Failed to allocate buffer\r\n");
        return;
    }
    
    terminal_puts(&main_terminal, "Reading sector 0 asynchronously...\r\n");
    
    disk_err_t err = async_disk_read(&main_disk, 0, 1, buffer);
    
    if (err == DISK_ERR_NONE) {
        terminal_puts(&main_terminal, "Read successful!\r\n");
        
        // Mostrar primeros 64 bytes
        terminal_puts(&main_terminal, "First 64 bytes:\r\n");
        for (int i = 0; i < 64; i += 16) {
            terminal_printf(&main_terminal, "%04x: ", i);
            for (int j = 0; j < 16; j++) {
                terminal_printf(&main_terminal, "%02x ", buffer[i+j]);
            }
            terminal_puts(&main_terminal, "\r\n");
        }
    } else {
        terminal_printf(&main_terminal, "Read failed with error %d\r\n", err);
    }
    
    kernel_free(buffer);
}

void cmd_async_write_test(void) {
    terminal_puts(&main_terminal, "\r\n=== Async Disk Write Test ===\r\n");
    terminal_puts(&main_terminal, "WARNING: This will write to disk!\r\n");
    
    uint8_t* buffer = (uint8_t*)kernel_malloc(512);
    if (!buffer) {
        terminal_puts(&main_terminal, "Failed to allocate buffer\r\n");
        return;
    }
    
    // Llenar buffer con patrón de prueba
    for (int i = 0; i < 512; i++) {
        buffer[i] = (uint8_t)(i & 0xFF);
    }
    
    terminal_puts(&main_terminal, "Writing test pattern to sector 100...\r\n");
    
    disk_err_t err = async_disk_write(&main_disk, 100, 1, buffer);
    
    if (err == DISK_ERR_NONE) {
        terminal_puts(&main_terminal, "Write successful!\r\n");
        
        // Verificar con lectura
        memset(buffer, 0, 512);
        err = async_disk_read(&main_disk, 100, 1, buffer);
        
        if (err == DISK_ERR_NONE) {
            terminal_puts(&main_terminal, "Verification read successful\r\n");
            
            // Verificar patrón
            bool match = true;
            for (int i = 0; i < 512; i++) {
                if (buffer[i] != (uint8_t)(i & 0xFF)) {
                    match = false;
                    break;
                }
            }
            
            if (match) {
                terminal_puts(&main_terminal, "Data verification: PASSED\r\n");
            } else {
                terminal_puts(&main_terminal, "Data verification: FAILED\r\n");
            }
        }
    } else {
        terminal_printf(&main_terminal, "Write failed with error %d\r\n", err);
    }
    
    kernel_free(buffer);
}