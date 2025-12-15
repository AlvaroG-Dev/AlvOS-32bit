#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <stdint.h>
#include <stdbool.h>

// Colores para mensajes de boot
#define BOOT_COLOR_INFO    0x00FFFF  // Cyan
#define BOOT_COLOR_OK      0x00FF00  // Verde
#define BOOT_COLOR_ERROR   0xFF0000  // Rojo
#define BOOT_COLOR_WARN    0xFFFF00  // Amarillo
#define BOOT_COLOR_TEXT    0xFFFFFF  // Blanco

// Estado del sistema de boot
typedef struct {
    bool boot_phase;           // true durante el boot, false después
    uint32_t current_line;     // Línea actual en pantalla
    uint32_t max_lines;        // Máximo de líneas disponibles
    uint32_t step_count;       // Contador de pasos de boot
} boot_state_t;

extern boot_state_t boot_state;

// Funciones públicas
void boot_log_init(void);
void boot_log_start(const char* message);
void boot_log_ok(void);
void boot_log_error(void);
void boot_log_info(const char* format, ...);
void boot_log_warn(const char* format, ...);
void boot_log_finish(void);
bool boot_is_active(void);

// Funciones auxiliares
void boot_log_draw_progress_bar(uint32_t current, uint32_t total);
void boot_log_clear_screen(void);

#endif // BOOT_LOG_H