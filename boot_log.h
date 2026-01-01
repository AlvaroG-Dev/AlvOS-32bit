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
#define BOOT_COLOR_DIM     0x808080  // Gris

// Estados para animación de asteriscos
typedef enum {
    BOOT_AST_DIM = 0,      // Rojo oscuro:   0x400000
    BOOT_AST_MEDIUM = 1,   // Rojo medio:    0x800000  
    BOOT_AST_BRIGHT = 2,   // Rojo brillante: 0xFF0000
    BOOT_AST_PULSE = 3     // Ciclo automático
} boot_ast_state_t;

// Estado del sistema de boot
typedef struct {
    bool boot_phase;           // true durante el boot, false después
    uint32_t current_line;     // Línea actual en pantalla
    uint32_t max_lines;        // Máximo de líneas disponibles
    uint32_t step_count;       // Contador de pasos de boot
    boot_ast_state_t ast_state; // Estado actual de asteriscos
    uint32_t animation_frame;  // Frame actual para animación
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
void boot_log_show_asterisks(uint32_t intensity);

// Funciones auxiliares
void boot_log_draw_progress_bar(uint32_t current, uint32_t total);
void boot_log_clear_screen(void);
void boot_log_loading(boot_ast_state_t state);  // Nueva: para estados de carga con asteriscos

#endif // BOOT_LOG_H