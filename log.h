#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include "task_utils.h"
#include "vfs.h"

// Constantes
#define LOG_MAX_MESSAGE_SIZE 512
#define LOG_DEFAULT_PATH "/ramfs/syslog.log"

// Niveles de log
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

// Funciones
void log_init(void);
void log_set_path(const char *new_path);
void log_message(log_level_t level, const char *format, ...);
int log_read(char *buffer, uint32_t size, uint32_t offset);
void log_test(void);

#endif