#ifndef TEXT_EDITOR_H
#define TEXT_EDITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"
#include "terminal.h"

// Configuración del editor
#define EDITOR_MAX_LINES        1024
#define EDITOR_LINE_MAX_LENGTH  256
#define EDITOR_TAB_SIZE         4
#define EDITOR_STATUS_HEIGHT    2
#define EDITOR_BUFFER_SIZE      (EDITOR_MAX_LINES * EDITOR_LINE_MAX_LENGTH)

// Modos del editor
typedef enum {
    EDITOR_MODE_NORMAL,      // Navegación y comandos
    EDITOR_MODE_INSERT,      // Inserción de texto
    EDITOR_MODE_COMMAND,     // Modo de comandos (línea inferior)
    EDITOR_MODE_SEARCH,      // Búsqueda
    EDITOR_MODE_REPLACE,      // Reemplazo
    EDITOR_MODE_HELP         // Ayuda
} editor_mode_t;

// Estructura de una línea de texto
typedef struct editor_line {
    char data[EDITOR_LINE_MAX_LENGTH];
    uint32_t length;
    bool modified;
} editor_line_t;

// Estructura principal del editor
typedef struct text_editor {
    // Buffer de líneas
    editor_line_t *lines;
    uint32_t line_count;
    uint32_t max_lines;
    
    // Cursor
    uint32_t cursor_x;      // Columna (carácter)
    uint32_t cursor_y;      // Línea
    
    // Viewport (porción visible de texto)
    uint32_t viewport_x;    // Primera columna visible
    uint32_t viewport_y;    // Primera línea visible
    uint32_t viewport_width;
    uint32_t viewport_height;
    
    // Terminal asociado
    Terminal *term;
    
    // Archivo
    char filename[256];
    int file_descriptor;
    bool file_open;
    bool modified;
    
    // Modo actual
    editor_mode_t mode;
    
    // Buffer de comandos
    char command_buffer[256];
    uint32_t command_length;
    
    // Buffer del portapapeles
    char clipboard[EDITOR_LINE_MAX_LENGTH];
    uint32_t clipboard_length;
    
    // Búsqueda
    char search_term[128];
    uint32_t search_length;
    
    // Estado
    bool running;
    char status_message[256];
    uint32_t status_time;
    
} text_editor_t;

// Funciones públicas del editor
text_editor_t* editor_create(Terminal *term);
void editor_destroy(text_editor_t *editor);

// Operaciones de archivo
int editor_open_file(text_editor_t *editor, const char *filename);
int editor_save_file(text_editor_t *editor);
int editor_save_as(text_editor_t *editor, const char *filename);
void editor_close_file(text_editor_t *editor);

// Operaciones de edición
void editor_insert_char(text_editor_t *editor, char c);
void editor_delete_char(text_editor_t *editor);
void editor_backspace(text_editor_t *editor);
void editor_insert_newline(text_editor_t *editor);
void editor_delete_line(text_editor_t *editor);

// Navegación
void editor_move_cursor(text_editor_t *editor, int dx, int dy);
void editor_move_cursor_home(text_editor_t *editor);
void editor_move_cursor_end(text_editor_t *editor);
void editor_page_up(text_editor_t *editor);
void editor_page_down(text_editor_t *editor);
void editor_goto_line(text_editor_t *editor, uint32_t line);

// Operaciones de texto
void editor_copy_line(text_editor_t *editor);
void editor_cut_line(text_editor_t *editor);
void editor_paste(text_editor_t *editor);
void editor_undo(text_editor_t *editor);

// Búsqueda
void editor_search(text_editor_t *editor, const char *term);
void editor_search_next(text_editor_t *editor);

// Interfaz
void editor_render(text_editor_t *editor);
void editor_render_status_bar(text_editor_t *editor);
void editor_set_status_message(text_editor_t *editor, const char *message);
void editor_process_key(text_editor_t *editor, int key);
void editor_process_key_normal(text_editor_t *editor, int key);
void editor_process_key_insert(text_editor_t *editor, int key);
void editor_process_key_command(text_editor_t *editor, int key);
void editor_process_key_search(text_editor_t *editor, int key);
void editor_execute_command(text_editor_t *editor, const char *cmd);
void editor_show_help(text_editor_t *editor);

// Callback de teclado
void editor_keyboard_callback(int key);
void editor_set_active(text_editor_t *editor);

// Bucle principal
void editor_run(text_editor_t *editor);

// Utilidades
uint32_t editor_get_line_display_length(const char *line, uint32_t length);
void editor_scroll_if_needed(text_editor_t *editor);

#endif // TEXT_EDITOR_H