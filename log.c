#include "log.h"
#include "terminal.h"
#include "string.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "irq.h"
#include "boot_log.h"
#include <stdarg.h>

extern Terminal main_terminal;
static int log_fd = -1;
static char log_path[VFS_PATH_MAX] = LOG_DEFAULT_PATH;
static bool log_initialized = false;

// ========================================================================
// INICIALIZACIÓN DEL SISTEMA DE LOGS
// ========================================================================

void log_init(void) {
    // No intentar escribir en pantalla durante boot
    // Solo configurar el archivo de log
    
    log_fd = vfs_open(log_path, VFS_O_RDWR | VFS_O_CREAT);
    if (log_fd < 0) {
        // Silenciosamente fallar durante boot
        log_initialized = false;
        return;
    }

    log_initialized = true;
    
    // Escribir header en el log
    const char* header = "\n========== NEW LOG SESSION ==========\n";
    vfs_write(log_fd, header, strlen(header));
}

// ========================================================================
// CONFIGURAR RUTA DEL LOG
// ========================================================================

void log_set_path(const char *new_path) {
    if (!new_path || strlen(new_path) >= VFS_PATH_MAX) {
        if (log_initialized) {
            terminal_printf(&main_terminal, "[log] Invalid log path\r\n");
        }
        return;
    }

    // Cerrar archivo actual
    if (log_fd >= 0) {
        vfs_close(log_fd);
        log_fd = -1;
    }

    // Actualizar ruta
    strncpy(log_path, new_path, VFS_PATH_MAX - 1);
    log_path[VFS_PATH_MAX - 1] = '\0';

    // Reabrir
    log_fd = vfs_open(log_path, VFS_O_RDWR | VFS_O_CREAT);
    if (log_fd < 0) {
        log_initialized = false;
        if (boot_is_active()) {
            // Durante boot, reportar con boot_log
            boot_log_info("Failed to open log file");
        } else {
            terminal_printf(&main_terminal, "[log] Failed to open new path %s\r\n", log_path);
        }
        return;
    }

    log_initialized = true;
    
    if (!boot_is_active()) {
        terminal_printf(&main_terminal, "[log] Path changed to %s\r\n", log_path);
    }
}

// ========================================================================
// REGISTRO DE MENSAJES EN EL LOG
// ========================================================================

void log_message(log_level_t level, const char *format, ...) {
    char buffer[LOG_MAX_MESSAGE_SIZE];
    uint32_t len = 0;

    // Añadir timestamp
    len += snprintf(buffer, sizeof(buffer), "[%u] ", ticks_since_boot / 100);

    // Añadir nivel de log
    switch (level) {
        case LOG_INFO:
            len += snprintf(buffer + len, sizeof(buffer) - len, "INFO: ");
            break;
        case LOG_WARN:
            len += snprintf(buffer + len, sizeof(buffer) - len, "WARN: ");
            break;
        case LOG_ERROR:
            len += snprintf(buffer + len, sizeof(buffer) - len, "ERROR: ");
            break;
    }

    // Añadir mensaje principal
    va_list args;
    va_start(args, format);
    len += vsnprintf(buffer + len, sizeof(buffer) - len, format, args);
    va_end(args);

    // Asegurar salto de línea
    if (len > 0 && len < sizeof(buffer) - 1 && buffer[len - 1] != '\n') {
        buffer[len++] = '\n';
    }

    // Asegurar terminador nulo
    if (len < sizeof(buffer)) {
        buffer[len] = '\0';
    } else {
        buffer[sizeof(buffer) - 1] = '\0';
        len = sizeof(buffer) - 1;
    }

    // Escribir en archivo si está disponible
    if (log_initialized && log_fd >= 0) {
        vfs_write(log_fd, buffer, len);
    }

}

// ========================================================================
// FUNCIONES DE LOGGING CONVENIENTES
// ========================================================================

void log_info(const char* format, ...) {
    char buffer[LOG_MAX_MESSAGE_SIZE - 64]; // Espacio para timestamp y nivel
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    log_message(LOG_INFO, "%s", buffer);
}

void log_warn(const char* format, ...) {
    char buffer[LOG_MAX_MESSAGE_SIZE - 64];
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    log_message(LOG_WARN, "%s", buffer);
}

void log_error(const char* format, ...) {
    char buffer[LOG_MAX_MESSAGE_SIZE - 64];
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    log_message(LOG_ERROR, "%s", buffer);
}

// ========================================================================
// LECTURA DE LOGS
// ========================================================================

int log_read(char *buffer, uint32_t size, uint32_t offset) {
    if (!buffer || size == 0) return -1;
    
    int fd = vfs_open(log_path, VFS_O_RDONLY);
    if (fd < 0) {
        if (!boot_is_active()) {
            terminal_puts(&main_terminal, "[log] Cannot open log file for reading\r\n");
        }
        return -1;
    }

    // TODO: Implementar vfs_seek cuando esté disponible
    // Por ahora, solo lectura desde el inicio
    int read = vfs_read(fd, buffer, size);
    vfs_close(fd);
    return read;
}

// Leer las últimas N líneas del log
int log_read_tail(char *buffer, uint32_t size, uint32_t lines) {
    if (!buffer || size == 0) return -1;
    
    int fd = vfs_open(log_path, VFS_O_RDONLY);
    if (fd < 0) return -1;
    
    // Leer todo el archivo
    char *temp = (char*)kernel_malloc(LOG_MAX_MESSAGE_SIZE * 100);
    if (!temp) {
        vfs_close(fd);
        return -1;
    }
    
    int total_read = vfs_read(fd, temp, LOG_MAX_MESSAGE_SIZE * 100);
    vfs_close(fd);
    
    if (total_read <= 0) {
        kernel_free(temp);
        return -1;
    }
    
    // Encontrar las últimas N líneas
    uint32_t line_count = 0;
    int pos = total_read - 1;
    
    while (pos >= 0 && line_count < lines) {
        if (temp[pos] == '\n') {
            line_count++;
        }
        pos--;
    }
    
    pos++; // Ajustar posición
    if (pos < 0) pos = 0;
    
    // Copiar al buffer de salida
    uint32_t copy_size = total_read - pos;
    if (copy_size > size - 1) copy_size = size - 1;
    
    memcpy(buffer, temp + pos, copy_size);
    buffer[copy_size] = '\0';
    
    kernel_free(temp);
    return copy_size;
}

// ========================================================================
// VOLCADO DEL LOG A TERMINAL
// ========================================================================

void log_dump(void) {
    if (boot_is_active()) return; // No dumping durante boot
    
    char buffer[4096];
    int read = log_read(buffer, sizeof(buffer) - 1, 0);
    
    if (read > 0) {
        buffer[read] = '\0';
        terminal_puts(&main_terminal, "\n========== LOG DUMP ==========\n");
        terminal_puts(&main_terminal, buffer);
        terminal_puts(&main_terminal, "========== END LOG ==========\n\n");
    } else {
        terminal_puts(&main_terminal, "[log] No log data available\r\n");
    }
}

void log_dump_tail(uint32_t lines) {
    if (boot_is_active()) return;
    
    char buffer[4096];
    int read = log_read_tail(buffer, sizeof(buffer) - 1, lines);
    
    if (read > 0) {
        buffer[read] = '\0';
        terminal_printf(&main_terminal, "\n========== LAST %u LINES ==========\n", lines);
        terminal_puts(&main_terminal, buffer);
        terminal_puts(&main_terminal, "========== END LOG ==========\n\n");
    } else {
        terminal_puts(&main_terminal, "[log] No log data available\r\n");
    }
}

// ========================================================================
// TEST DEL SISTEMA DE LOGS
// ========================================================================

void log_test(void) {
    if (boot_is_active()) return;
    
    terminal_puts(&main_terminal, "[log] Running logging system test\r\n");

    log_info("Test info message");
    log_warn("Test warning message");
    log_error("Test error message");

    // Pequeña pausa para asegurar que se escriba
    for (volatile int i = 0; i < 1000000; i++);
    
    log_dump_tail(10);
}

// ========================================================================
// LIMPIEZA
// ========================================================================

void log_cleanup(void) {
    if (log_fd >= 0) {
        vfs_close(log_fd);
        log_fd = -1;
    }
    log_initialized = false;
}