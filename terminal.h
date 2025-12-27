#ifndef TERMINAL_CIRCULAR_H
#define TERMINAL_CIRCULAR_H

#include <stdbool.h>
#include "drawing.h"
#include "keyboard.h"
#include "vfs.h"

#define ANSI_COLOR_BLACK    "\033[30m"
#define ANSI_COLOR_RED      "\033[31m"
#define ANSI_COLOR_GREEN    "\033[32m"
#define ANSI_COLOR_YELLOW   "\033[33m"
#define ANSI_COLOR_BLUE     "\033[34m"
#define ANSI_COLOR_MAGENTA  "\033[35m"
#define ANSI_COLOR_CYAN     "\033[36m"
#define ANSI_COLOR_WHITE    "\033[37m"
#define ANSI_COLOR_RESET    "\033[0m"

// Ahora estos valores se calculan dinámicamente
#define COMMAND_HISTORY_SIZE 10

// Buffer circular configuración - factores de cálculo
#define BUFFER_LINE_MULTIPLIER 55  // Multiplicador para calcular líneas del buffer (55x más que visible)
#define MIN_BUFFER_LINES 1024      // Mínimo número de líneas en buffer
#define MAX_BUFFER_LINES 8192      // Máximo número de líneas en buffer

// Estados para el parser ANSI
typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESCAPE,
    ANSI_STATE_CSI,       // Control Sequence Introducer
    ANSI_STATE_OSC        // Operating System Command
} AnsiParserState;

// Estructura para el buffer circular
typedef struct {
    char* data;                    // Buffer de caracteres
    uint32_t* line_attrs;         // Atributos por línea (colores, flags)
    uint32_t* char_colors;        // Colores por caracter (FG color)
    uint32_t size;                // Tamaño total del buffer
    uint32_t lines;               // Número total de líneas
    uint32_t width;               // Ancho de cada línea (caracteres por línea)
    uint32_t head;                // Posición de escritura (línea más reciente)
    uint32_t tail;                // Posición más antigua válida
    uint32_t count;               // Número de líneas válidas
    uint8_t wrapped;              // 1 si el buffer ha dado la vuelta
} CircularBuffer;

// Estructura para atributos de texto
typedef struct {
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t bold;
    uint8_t underline;
    uint8_t blink;
    uint8_t reverse;
    uint8_t conceal;
} TextAttributes;

// Estructura para información del prompt
typedef struct {
    char username[32];
    char hostname[64];
    char current_dir[VFS_PATH_MAX];
    uint32_t last_exit_code;
    uint32_t job_count;
    uint8_t is_root;
    char time_format[32];
} PromptInfo;

typedef struct {
    CircularBuffer buffer;        // Buffer circular principal
    
    // Dimensiones calculadas dinámicamente
    uint32_t width;               // Caracteres por fila
    uint32_t height;              // Filas visibles en pantalla
    
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t view_offset;         // Offset de visualización (para scroll)
    uint32_t view_start_line;     // Línea de inicio en el buffer circular
    uint32_t fg_color;            // Color actual del texto
    uint32_t bg_color;            // Color actual del fondo
    uint32_t default_fg;          // Color por defecto del texto
    uint32_t default_bg;          // Color por defecto del fondo
    char input_buffer[256];
    uint32_t input_pos;
    char command_history[COMMAND_HISTORY_SIZE][256];
    uint32_t history_pos;
    uint32_t history_count;
    uint32_t current_history;
    uint8_t echo;
    uint8_t needs_full_redraw;
    uint8_t in_history_mode;
    char saved_input[256];
    uint8_t cursor_visible;
    uint32_t cursor_blink_rate;
    uint32_t last_blink_time;
    uint8_t cursor_state_changed;
    uint8_t* dirty_lines;         // Array dinámico de líneas sucias
    uint8_t flags;
    uint32_t current_fg_color;    // Color actual del texto
    uint32_t current_bg_color;    // Color actual del fondo
    uint8_t ansi_parser_state;    // Estado del parser ANSI
    char ansi_buffer[16];         // Buffer para secuencias ANSI
    uint8_t ansi_buffer_pos;
    char cwd[VFS_PATH_MAX];       // Current working directory
    char path[512];               // PATH environment variable
    
    // Atributos de texto y prompt mejorado
    TextAttributes current_attrs;    // Atributos de texto actuales
    TextAttributes saved_attrs;      // Atributos guardados
    PromptInfo prompt_info;          // Información para el prompt
    uint32_t last_exit_code;         // Código de salida del último comando
    char prompt_buffer[256];         // Buffer para el prompt renderizado
    uint32_t prompt_length;          // Longitud del prompt actual  // ¡AGREGAR ESTO!
    uint8_t show_time_in_prompt;     // Mostrar hora en el prompt
    uint8_t show_user_in_prompt;     // Mostrar usuario en el prompt
    uint8_t show_path_in_prompt;     // Mostrar ruta en el prompt
    
    // Estadísticas para debug
    uint32_t total_lines_written;
    uint32_t page_faults_avoided;
} Terminal;

extern bool graphical_mode;

// Funciones para calcular dimensiones de terminal
uint32_t terminal_calculate_width(void);
uint32_t terminal_calculate_height(void);
void terminal_recalculate_dimensions(Terminal* term);
int terminal_resize(Terminal* term);

// Funciones del buffer circular
int circular_buffer_init(CircularBuffer* cb, uint32_t width, uint32_t buffer_lines);
void circular_buffer_destroy(CircularBuffer* cb);
int circular_buffer_add_line(CircularBuffer* cb);
char* circular_buffer_get_line(CircularBuffer* cb, uint32_t line_offset);
uint32_t* circular_buffer_get_colors(CircularBuffer* cb, uint32_t line_offset);
uint32_t circular_buffer_get_line_attrs(CircularBuffer* cb, uint32_t line_offset);
void circular_buffer_set_line_attrs(CircularBuffer* cb, uint32_t line_offset, uint32_t attrs);
int circular_buffer_is_valid_line(CircularBuffer* cb, uint32_t line_offset);
void circular_buffer_clear(CircularBuffer* cb);
int circular_buffer_resize(CircularBuffer* cb, uint32_t new_width, uint32_t new_buffer_lines);

// Funciones del terminal
uint32_t terminal_get_cursor_x(Terminal *term);
uint32_t terminal_get_cursor_y(Terminal *term);
void terminal_init(Terminal* term);
void terminal_destroy(Terminal* term);
void terminal_clear(Terminal* term);
void terminal_putchar(Terminal* term, char c);
void terminal_puts(Terminal* term, const char* str);
void terminal_handle_key(Terminal* term, int key);
void terminal_process_command(Terminal* term);
void terminal_draw(Terminal* term);
void terminal_scroll(Terminal* term);
void terminal_execute(Terminal* term, const char* cmd);
void terminal_update_cursor_blink(Terminal* term, uint32_t current_tick);
void terminal_scroll_up(Terminal* term);
void terminal_scroll_down(Terminal* term);
void terminal_printf(Terminal* term, const char* format, ...);
void terminal_scroll_to_bottom(Terminal* term);
void terminal_get_stats(Terminal* term, uint32_t* total_lines, uint32_t* valid_lines, uint32_t* buffer_usage);
void terminal_draw_line(Terminal* term, uint32_t screen_y);

// Funciones nuevas para colores ANSI
uint32_t ansi_to_color(uint8_t ansi_code, uint8_t is_bright);
void terminal_apply_ansi_code(Terminal* term, int code);
void terminal_process_ansi_sequence(Terminal* term, const char* seq);
void terminal_putchar_with_attrs(Terminal* term, char c);

// Funciones para prompt mejorado
void terminal_update_prompt_info(Terminal* term);
void terminal_render_prompt(Terminal* term);
void terminal_show_enhanced_prompt(Terminal* term);
const char* terminal_get_prompt_color(Terminal* term, uint8_t is_error);

// Funciones de verificación y seguridad
int terminal_verify_memory_access(Terminal* term, uint32_t line_offset);
void terminal_safe_memset(char* ptr, char value, size_t size);
void terminal_safe_memcpy(char* dst, const char* src, size_t size);

void terminal_set_cursor(Terminal *term, uint32_t x, uint32_t y);
void terminal_set_color(Terminal *term, uint32_t fg, uint32_t bg);
void terminal_reset_colors(Terminal* term);
void terminal_show_cursor(Terminal *term, bool show);
uint32_t terminal_get_cursor_x(Terminal *term);
uint32_t terminal_get_cursor_y(Terminal *term);

#endif