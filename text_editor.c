#include "text_editor.h"
#include "irq.h"
#include "kernel.h"
#include "keyboard.h"
#include "memory.h"
#include "string.h"
#include "task.h"
#include "vfs.h"

// Variable global para el editor activo (usado por el callback de teclado)
text_editor_t *g_active_editor = NULL;
bool needs_redraw = false;

// Declaraciones de funciones wrapper del terminal (pueden estar en otro
// archivo)
extern void terminal_set_cursor(Terminal *term, uint32_t x, uint32_t y);
extern void terminal_set_color(Terminal *term, uint32_t fg, uint32_t bg);
extern uint32_t terminal_get_cursor_x(Terminal *term);
extern void terminal_show_cursor(Terminal *term, bool show);

// ============================================================================
// CREACIÓN Y DESTRUCCIÓN
// ============================================================================

text_editor_t *editor_create(Terminal *term) {
  if (!term)
    return NULL;

  text_editor_t *editor = (text_editor_t *)kernel_malloc(sizeof(text_editor_t));
  if (!editor)
    return NULL;

  memset(editor, 0, sizeof(text_editor_t));

  // Asignar buffer de líneas
  editor->lines =
      (editor_line_t *)kernel_malloc(sizeof(editor_line_t) * EDITOR_MAX_LINES);
  if (!editor->lines) {
    kernel_free(editor);
    return NULL;
  }

  memset(editor->lines, 0, sizeof(editor_line_t) * EDITOR_MAX_LINES);

  editor->term = term;
  editor->max_lines = EDITOR_MAX_LINES;
  editor->line_count = 1; // Al menos una línea vacía
  editor->cursor_x = 0;
  editor->cursor_y = 0;
  editor->viewport_x = 0;
  editor->viewport_y = 0;
  editor->viewport_width = term->width;
  editor->viewport_height = term->height - EDITOR_STATUS_HEIGHT;
  editor->mode = EDITOR_MODE_NORMAL;
  editor->running = true;
  editor->modified = false;
  editor->file_open = false;
  editor->file_descriptor = -1;

  strncpy(editor->status_message, "Editor listo. ^G = Ayuda, ^X = Salir",
          sizeof(editor->status_message) - 1);

  return editor;
}

void editor_mark_line_dirty(text_editor_t *editor, uint32_t line) {
  if (!editor || !editor->term)
    return;

  // Calcular línea en pantalla
  if (line >= editor->viewport_y &&
      line < editor->viewport_y + editor->viewport_height) {
    uint32_t screen_line = line - editor->viewport_y;
    if (screen_line < editor->term->height) {
      editor->term->dirty_lines[screen_line] = 1;
      needs_redraw = true;
    }
  }
}

void editor_destroy(text_editor_t *editor) {
  if (!editor)
    return;

  if (editor->file_open && editor->file_descriptor >= 0) {
    vfs_close(editor->file_descriptor);
  }

  if (editor->lines) {
    kernel_free(editor->lines);
  }

  kernel_free(editor);
}

// ============================================================================
// OPERACIONES DE ARCHIVO
// ============================================================================

int editor_open_file(text_editor_t *editor, const char *filename) {
  if (!editor || !filename)
    return -1;

  // Cerrar archivo anterior si existe
  if (editor->file_open && editor->file_descriptor >= 0) {
    vfs_close(editor->file_descriptor);
    editor->file_descriptor = -1;
    editor->file_open = false;
  }

  // ✅ Limpiar buffer antes de cargar
  memset(editor->lines, 0, sizeof(editor_line_t) * EDITOR_MAX_LINES);
  editor->line_count = 0;

  // Abrir archivo
  int fd = vfs_open(filename, VFS_O_RDONLY);
  if (fd < 0) {
    // Archivo no existe, crear buffer vacío
    editor->line_count = 1;
    memset(&editor->lines[0], 0, sizeof(editor_line_t));
    strncpy(editor->filename, filename, sizeof(editor->filename) - 1);
    editor->filename[sizeof(editor->filename) - 1] = '\0';
    editor->modified = false;
    editor_set_status_message(editor, "Archivo nuevo");
    needs_redraw = true;
    return 0;
  }

  // ✅ Leer en chunks pequeños para evitar problemas de memoria
  char buffer[512];
  uint32_t total_read = 0;
  uint32_t current_line = 0;
  uint32_t line_pos = 0;

  while (current_line < EDITOR_MAX_LINES) {
    int bytes_read = vfs_read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
      break;

    buffer[bytes_read] = '\0';

    for (int i = 0; i < bytes_read && current_line < EDITOR_MAX_LINES; i++) {
      char c = buffer[i];

      if (c == '\n') {
        // Finalizar línea actual
        editor->lines[current_line].data[line_pos] = '\0';
        editor->lines[current_line].length = line_pos;
        editor->lines[current_line].modified = false;
        current_line++;
        line_pos = 0;
      } else if (c == '\r') {
        // Ignorar \r
        continue;
      } else if (line_pos < EDITOR_LINE_MAX_LENGTH - 1) {
        editor->lines[current_line].data[line_pos++] = c;
      }
    }

    total_read += bytes_read;

    // ✅ Prevenir lecturas infinitas
    if (total_read > EDITOR_BUFFER_SIZE)
      break;
  }

  // Finalizar última línea si no termina en \n
  if (line_pos > 0 && current_line < EDITOR_MAX_LINES) {
    editor->lines[current_line].data[line_pos] = '\0';
    editor->lines[current_line].length = line_pos;
    editor->lines[current_line].modified = false;
    current_line++;
  }

  vfs_close(fd);

  // ✅ Asegurar al menos una línea
  editor->line_count = (current_line > 0) ? current_line : 1;
  if (editor->line_count == 0) {
    editor->line_count = 1;
    memset(&editor->lines[0], 0, sizeof(editor_line_t));
  }

  strncpy(editor->filename, filename, sizeof(editor->filename) - 1);
  editor->filename[sizeof(editor->filename) - 1] = '\0';
  editor->file_open = true;
  editor->modified = false;
  editor->cursor_x = 0;
  editor->cursor_y = 0;
  editor->viewport_x = 0;
  editor->viewport_y = 0;

  char msg[128];
  snprintf(msg, sizeof(msg), "Cargado: %u lineas, %u bytes", editor->line_count,
           total_read);
  editor_set_status_message(editor, msg);
  needs_redraw = true;

  return 0;
}

int editor_save_file(text_editor_t *editor) {
  if (!editor)
    return -1;

  if (!editor->filename[0]) {
    editor_set_status_message(
        editor, "No hay nombre de archivo. Use :w nombre_archivo");
    return -1;
  }

  int result = editor_save_as(editor, editor->filename);
  if (result == 0) {
    editor->modified = false;
    editor_set_status_message(editor, "Archivo guardado correctamente");
  } else {
    editor_set_status_message(editor, "Error al guardar archivo");
  }

  return result;
}

int editor_save_as(text_editor_t *editor, const char *filename) {
  if (!editor || !filename)
    return -1;

  // Primero intentar eliminar el archivo existente si existe
  if (vfs_unlink(filename) == 0) {
    editor_set_status_message(editor, "Archivo existente eliminado");
  }
  // Si falla, puede ser porque no existe (lo cual está bien)

  // Crear nuevo archivo
  int fd = vfs_open(filename, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
  if (fd < 0) {
    editor_set_status_message(editor, "Error: No se pudo crear archivo");
    return -1;
  }

  // Escribir cada línea
  int total_written = 0;
  for (uint32_t i = 0; i < editor->line_count; i++) {
    editor_set_status_message(editor, "Guardando...");
    needs_redraw = true;
    // Escribir contenido de la línea
    if (editor->lines[i].length > 0) {
      int written =
          vfs_write(fd, editor->lines[i].data, editor->lines[i].length);
      if (written < 0) {
        vfs_close(fd);
        editor_set_status_message(editor, "Error al escribir archivo");
        return -1;
      }
      total_written += written;
    }

    // Escribir salto de línea (excepto última línea)
    if (i < editor->line_count - 1) {
      char newline = '\n';
      int written = vfs_write(fd, &newline, 1);
      if (written < 0) {
        vfs_close(fd);
        editor_set_status_message(editor, "Error al escribir archivo");
        return -1;
      }
      total_written += written;
    }

    editor->lines[i].modified = false;
  }

  vfs_close(fd);

  // Actualizar estado del editor
  strncpy(editor->filename, filename, sizeof(editor->filename) - 1);
  editor->filename[sizeof(editor->filename) - 1] = '\0';
  editor->modified = false;
  editor->file_open = true;

  char msg[128];
  snprintf(msg, sizeof(msg), "Guardado: %d bytes escritos", total_written);
  editor_set_status_message(editor, msg);

  return 0;
}

void editor_close_file(text_editor_t *editor) {
  if (!editor)
    return;

  if (editor->file_descriptor >= 0) {
    vfs_close(editor->file_descriptor);
    editor->file_descriptor = -1;
  }

  editor->file_open = false;
  editor->filename[0] = '\0';
}

// ============================================================================
// OPERACIONES DE EDICIÓN
// ============================================================================

void editor_insert_char(text_editor_t *editor, char c) {
  if (!editor || editor->cursor_y >= editor->line_count)
    return;

  if (c != '\t' && (c < 32 || c >= 127)) {
    return;
  }

  editor_line_t *line = &editor->lines[editor->cursor_y];

  if (line->length >= EDITOR_LINE_MAX_LENGTH - 1) {
    editor_set_status_message(editor, "Linea demasiado larga");
    needs_redraw = true;
    return;
  }

  if (editor->cursor_x > line->length) {
    editor->cursor_x = line->length;
  }

  if (editor->cursor_x < line->length) {
    memmove(&line->data[editor->cursor_x + 1], &line->data[editor->cursor_x],
            line->length - editor->cursor_x);
  }

  line->data[editor->cursor_x] = c;
  line->length++;
  line->data[line->length] = '\0';
  line->modified = true;
  editor->modified = true;

  editor->cursor_x++;

  // ✅ Solo marcar esta línea como sucia
  editor_mark_line_dirty(editor, editor->cursor_y);
}

void editor_backspace(text_editor_t *editor) {
  if (!editor)
    return;

  // Si estamos al inicio de la línea
  if (editor->cursor_x == 0) {
    // Si no estamos en la primera línea, unir con línea anterior
    if (editor->cursor_y > 0) {
      editor_line_t *prev_line = &editor->lines[editor->cursor_y - 1];
      editor_line_t *curr_line = &editor->lines[editor->cursor_y];

      // Verificar si cabe
      if (prev_line->length + curr_line->length < EDITOR_LINE_MAX_LENGTH) {
        uint32_t old_prev_len = prev_line->length;

        // Copiar línea actual al final de la anterior
        memcpy(&prev_line->data[prev_line->length], curr_line->data,
               curr_line->length);
        prev_line->length += curr_line->length;
        prev_line->data[prev_line->length] = '\0';
        prev_line->modified = true;

        // Eliminar línea actual
        if (editor->cursor_y < editor->line_count - 1) {
          memmove(&editor->lines[editor->cursor_y],
                  &editor->lines[editor->cursor_y + 1],
                  (editor->line_count - editor->cursor_y - 1) *
                      sizeof(editor_line_t));
        }

        editor->line_count--;
        editor->cursor_y--;
        editor->cursor_x = old_prev_len;
        editor->modified = true;

        // ✅ Ajustar scroll si es necesario
        editor_scroll_if_needed(editor);

        // ✅ Marcar redibujado completo (cambio estructural)
        if (editor->term) {
          editor->term->needs_full_redraw = 1;
          memset(editor->term->dirty_lines, 1, editor->term->height);
        }
        needs_redraw = true;
      }
    }
    return;
  }

  // ✅ Retroceder cursor
  editor->cursor_x--;

  // ✅ Eliminar carácter anterior
  editor_line_t *line = &editor->lines[editor->cursor_y];

  if (editor->cursor_x < line->length) {
    if (editor->cursor_x < line->length - 1) {
      // Mover el resto hacia la izquierda
      memmove(&line->data[editor->cursor_x], &line->data[editor->cursor_x + 1],
              line->length - editor->cursor_x - 1);
    }

    line->length--;
    line->data[line->length] = '\0';
    line->modified = true;
    editor->modified = true;

    // ✅ Marcar solo esta línea como sucia
    editor_mark_line_dirty(editor, editor->cursor_y);
  }
}

void editor_delete_char(text_editor_t *editor) {
  if (!editor || editor->cursor_y >= editor->line_count)
    return;

  editor_line_t *line = &editor->lines[editor->cursor_y];

  // Si estamos al final de la línea, unir con siguiente
  if (editor->cursor_x >= line->length) {
    if (editor->cursor_y < editor->line_count - 1) {
      editor_line_t *next_line = &editor->lines[editor->cursor_y + 1];

      // Verificar si cabe
      if (line->length + next_line->length < EDITOR_LINE_MAX_LENGTH) {
        // Copiar siguiente línea al final de actual
        memcpy(&line->data[line->length], next_line->data, next_line->length);
        line->length += next_line->length;
        line->data[line->length] = '\0';
        line->modified = true;

        // Eliminar siguiente línea
        if (editor->cursor_y + 1 < editor->line_count - 1) {
          memmove(&editor->lines[editor->cursor_y + 1],
                  &editor->lines[editor->cursor_y + 2],
                  (editor->line_count - editor->cursor_y - 2) *
                      sizeof(editor_line_t));
        }

        editor->line_count--;
        editor->modified = true;

        // ✅ Marcar redibujado completo (cambio estructural)
        if (editor->term) {
          editor->term->needs_full_redraw = 1;
          memset(editor->term->dirty_lines, 1, editor->term->height);
        }
        needs_redraw = true;
      }
    }
    return;
  }

  // ✅ Eliminar carácter en cursor
  if (editor->cursor_x < line->length) {
    if (editor->cursor_x < line->length - 1) {
      // Mover el resto de caracteres hacia la izquierda
      memmove(&line->data[editor->cursor_x], &line->data[editor->cursor_x + 1],
              line->length - editor->cursor_x - 1);
    }

    line->length--;
    line->data[line->length] = '\0';
    line->modified = true;
    editor->modified = true;

    // ✅ Marcar solo esta línea como sucia
    editor_mark_line_dirty(editor, editor->cursor_y);
  }
}

void editor_insert_newline(text_editor_t *editor) {
  if (!editor || editor->line_count >= EDITOR_MAX_LINES) {
    if (editor)
      editor_set_status_message(editor, "Máximo de líneas alcanzado");
    return;
  }

  // Mover líneas hacia abajo
  if (editor->cursor_y < editor->line_count - 1) {
    memmove(&editor->lines[editor->cursor_y + 2],
            &editor->lines[editor->cursor_y + 1],
            (editor->line_count - editor->cursor_y - 1) *
                sizeof(editor_line_t));
  }

  editor_line_t *curr_line = &editor->lines[editor->cursor_y];
  editor_line_t *new_line = &editor->lines[editor->cursor_y + 1];

  // Dividir línea actual
  memset(new_line, 0, sizeof(editor_line_t));

  if (editor->cursor_x < curr_line->length) {
    // Copiar resto de línea a nueva línea
    uint32_t rest_len = curr_line->length - editor->cursor_x;
    memcpy(new_line->data, &curr_line->data[editor->cursor_x], rest_len);
    new_line->length = rest_len;
    new_line->data[rest_len] = '\0';

    // Truncar línea actual
    curr_line->length = editor->cursor_x;
    curr_line->data[editor->cursor_x] = '\0';
  }

  curr_line->modified = true;
  new_line->modified = true;
  editor->line_count++;
  editor->cursor_y++;
  editor->cursor_x = 0;
  editor->modified = true;

  // ✅ Ajustar scroll
  editor_scroll_if_needed(editor);

  // ✅ Cambio estructural - redibujado completo
  if (editor->term) {
    editor->term->needs_full_redraw = 1;
    memset(editor->term->dirty_lines, 1, editor->term->height);
  }
  needs_redraw = true;
}

void editor_delete_line(text_editor_t *editor) {
  if (!editor || editor->line_count <= 1)
    return;

  // Mover líneas hacia arriba
  if (editor->cursor_y < editor->line_count - 1) {
    memmove(
        &editor->lines[editor->cursor_y], &editor->lines[editor->cursor_y + 1],
        (editor->line_count - editor->cursor_y - 1) * sizeof(editor_line_t));
  }

  editor->line_count--;
  editor->modified = true;

  // Ajustar cursor
  if (editor->cursor_y >= editor->line_count) {
    editor->cursor_y = editor->line_count - 1;
  }
  editor->cursor_x = 0;

  // ✅ Ajustar scroll
  editor_scroll_if_needed(editor);

  // ✅ Cambio estructural
  if (editor->term) {
    editor->term->needs_full_redraw = 1;
    memset(editor->term->dirty_lines, 1, editor->term->height);
  }
  needs_redraw = true;
}

// ============================================================================
// NAVEGACIÓN
// ============================================================================

void editor_move_cursor(text_editor_t *editor, int dx, int dy) {
  if (!editor)
    return;

  uint32_t old_y = editor->cursor_y;
  uint32_t old_x = editor->cursor_x;

  int new_y = (int)editor->cursor_y + dy;
  int new_x = (int)editor->cursor_x + dx;

  // Movimiento vertical
  if (dy != 0) {
    if (new_y < 0)
      new_y = 0;
    if (new_y >= (int)editor->line_count)
      new_y = editor->line_count - 1;
    editor->cursor_y = new_y;

    // ✅ Ajustar cursor_x si la nueva línea es más corta
    uint32_t new_line_len = editor->lines[editor->cursor_y].length;
    if (editor->cursor_x > new_line_len) {
      editor->cursor_x = new_line_len;
    }
  }

  // Movimiento horizontal
  if (dx != 0) {
    if (new_x < 0)
      new_x = 0;
    uint32_t max_x = editor->lines[editor->cursor_y].length;
    if (new_x > (int)max_x)
      new_x = max_x;
    editor->cursor_x = new_x;
  }

  editor_scroll_if_needed(editor);

  // ✅ Solo marcar líneas afectadas
  if (old_y != editor->cursor_y) {
    editor_mark_line_dirty(editor, old_y);
    editor_mark_line_dirty(editor, editor->cursor_y);
  } else if (old_x != editor->cursor_x) {
    editor_mark_line_dirty(editor, editor->cursor_y);
  }
}

void editor_move_cursor_home(text_editor_t *editor) {
  if (!editor)
    return;
  editor->cursor_x = 0;
  editor_scroll_if_needed(editor);
}

void editor_move_cursor_end(text_editor_t *editor) {
  if (!editor || editor->cursor_y >= editor->line_count)
    return;
  editor->cursor_x = editor->lines[editor->cursor_y].length;
  editor_scroll_if_needed(editor);
}

void editor_page_up(text_editor_t *editor) {
  if (!editor)
    return;

  int lines_to_move = editor->viewport_height;
  if ((int)editor->cursor_y - lines_to_move < 0) {
    editor->cursor_y = 0;
  } else {
    editor->cursor_y -= lines_to_move;
  }

  if (editor->cursor_x > editor->lines[editor->cursor_y].length) {
    editor->cursor_x = editor->lines[editor->cursor_y].length;
  }

  editor_scroll_if_needed(editor);
}

void editor_page_down(text_editor_t *editor) {
  if (!editor)
    return;

  int lines_to_move = editor->viewport_height;
  if (editor->cursor_y + lines_to_move >= editor->line_count) {
    editor->cursor_y = editor->line_count - 1;
  } else {
    editor->cursor_y += lines_to_move;
  }

  if (editor->cursor_x > editor->lines[editor->cursor_y].length) {
    editor->cursor_x = editor->lines[editor->cursor_y].length;
  }

  editor_scroll_if_needed(editor);
}

void editor_goto_line(text_editor_t *editor, uint32_t line) {
  if (!editor)
    return;

  if (line < 1)
    line = 1;
  if (line > editor->line_count)
    line = editor->line_count;

  editor->cursor_y = line - 1;
  editor->cursor_x = 0;
  editor_scroll_if_needed(editor);
}

// ============================================================================
// OPERACIONES DE TEXTO
// ============================================================================

void editor_copy_line(text_editor_t *editor) {
  if (!editor || editor->cursor_y >= editor->line_count)
    return;

  editor_line_t *line = &editor->lines[editor->cursor_y];
  memcpy(editor->clipboard, line->data, line->length);
  editor->clipboard[line->length] = '\0';
  editor->clipboard_length = line->length;

  editor_set_status_message(editor, "Línea copiada");
}

void editor_cut_line(text_editor_t *editor) {
  if (!editor || editor->cursor_y >= editor->line_count)
    return;

  editor_copy_line(editor);
  editor_delete_line(editor);
  editor_set_status_message(editor, "Línea cortada");
}

void editor_paste(text_editor_t *editor) {
  if (!editor || editor->clipboard_length == 0)
    return;

  if (editor->line_count >= EDITOR_MAX_LINES) {
    editor_set_status_message(editor, "Máximo de líneas alcanzado");
    return;
  }

  // Insertar nueva línea
  if (editor->cursor_y < editor->line_count - 1) {
    memmove(&editor->lines[editor->cursor_y + 2],
            &editor->lines[editor->cursor_y + 1],
            (editor->line_count - editor->cursor_y - 1) *
                sizeof(editor_line_t));
  }

  editor_line_t *new_line = &editor->lines[editor->cursor_y + 1];
  memcpy(new_line->data, editor->clipboard, editor->clipboard_length);
  new_line->data[editor->clipboard_length] = '\0';
  new_line->length = editor->clipboard_length;
  new_line->modified = true;

  editor->line_count++;
  editor->cursor_y++;
  editor->cursor_x = 0;
  editor->modified = true;

  editor_set_status_message(editor, "Línea pegada");
}

void editor_undo(text_editor_t *editor) {
  // TODO: Implementar sistema de undo/redo
  if (editor) {
    editor_set_status_message(editor, "Undo no implementado aún");
  }
}

// ============================================================================
// BÚSQUEDA
// ============================================================================

void editor_search(text_editor_t *editor, const char *term) {
  if (!editor || !term)
    return;

  strncpy(editor->search_term, term, sizeof(editor->search_term) - 1);
  editor->search_term[sizeof(editor->search_term) - 1] = '\0';
  editor->search_length = strlen(editor->search_term);

  editor_search_next(editor);
}

void editor_search_next(text_editor_t *editor) {
  if (!editor || editor->search_length == 0)
    return;

  // Buscar desde la posición actual
  for (uint32_t i = editor->cursor_y; i < editor->line_count; i++) {
    uint32_t start = (i == editor->cursor_y) ? editor->cursor_x + 1 : 0;
    editor_line_t *line = &editor->lines[i];

    for (uint32_t j = start; j <= line->length - editor->search_length; j++) {
      if (strncmp(&line->data[j], editor->search_term, editor->search_length) ==
          0) {
        editor->cursor_y = i;
        editor->cursor_x = j;
        editor_scroll_if_needed(editor);
        editor_set_status_message(editor, "Encontrado");
        return;
      }
    }
  }

  editor_set_status_message(editor, "No se encontró más coincidencias");
}

// ============================================================================
// UTILIDADES
// ============================================================================

uint32_t editor_get_line_display_length(const char *line, uint32_t length) {
  uint32_t display_len = 0;
  for (uint32_t i = 0; i < length; i++) {
    if (line[i] == '\t') {
      display_len += EDITOR_TAB_SIZE - (display_len % EDITOR_TAB_SIZE);
    } else {
      display_len++;
    }
  }
  return display_len;
}

void editor_scroll_if_needed(text_editor_t *editor) {
  if (!editor)
    return;

  uint32_t old_viewport_y = editor->viewport_y;
  uint32_t old_viewport_x = editor->viewport_x;

  // Scroll vertical
  if (editor->cursor_y < editor->viewport_y) {
    editor->viewport_y = editor->cursor_y;
  }
  if (editor->cursor_y >= editor->viewport_y + editor->viewport_height) {
    editor->viewport_y = editor->cursor_y - editor->viewport_height + 1;
  }

  // Scroll horizontal
  if (editor->cursor_x < editor->viewport_x) {
    editor->viewport_x = editor->cursor_x;
  }
  if (editor->cursor_x >= editor->viewport_x + editor->viewport_width) {
    editor->viewport_x = editor->cursor_x - editor->viewport_width + 1;
  }

  // Si el viewport cambió, forzar redibujado completo
  if (old_viewport_y != editor->viewport_y ||
      old_viewport_x != editor->viewport_x) {
    if (editor->term) {
      editor->term->needs_full_redraw = 1;
      memset(editor->term->dirty_lines, 1, editor->term->height);
    }
    needs_redraw = true;
  }
}

void editor_set_status_message(text_editor_t *editor, const char *message) {
  if (!editor || !message)
    return;

  strncpy(editor->status_message, message, sizeof(editor->status_message) - 1);
  editor->status_message[sizeof(editor->status_message) - 1] = '\0';
  editor->status_time = ticks_since_boot;
}

// ============================================================================
// RENDERIZADO
// ============================================================================

void editor_render(text_editor_t *editor) {
  if (!editor || !editor->term)
    return;

  Terminal *term = editor->term;

  // ✅ NO limpiar toda la pantalla, usar dirty_lines del terminal
  // Solo limpiar en el primer render o cambio de modo
  static editor_mode_t last_mode = EDITOR_MODE_NORMAL;
  static bool first_render = true;

  if (first_render || last_mode != editor->mode) {
    terminal_clear(term);
    term->needs_full_redraw = 1;
    first_render = false;
    last_mode = editor->mode;
  }

  if (editor->mode == EDITOR_MODE_HELP) {
    // Modo ayuda - renderizar solo si cambió de modo
    if (last_mode != EDITOR_MODE_HELP) {
      terminal_set_color(term, 0x00FFFF, 0x000000);
      terminal_puts(term, "=== Editor de Texto - Ayuda ===\r\n\r\n");
      terminal_set_color(term, 0xFFFFFF, 0x000000);

      terminal_puts(term, "Modo Normal:\r\n");
      terminal_puts(term, "  i/a/o - Modo insercion\r\n");
      terminal_puts(term, "  h/j/k/l - Navegar\r\n");
      terminal_puts(term, "  0/$ - Inicio/fin linea\r\n");
      terminal_puts(term, "  x/d - Eliminar\r\n");
      terminal_puts(term, "  y/p - Copiar/pegar\r\n");
      terminal_puts(term, "  / - Buscar\r\n");
      terminal_puts(term, "  : - Comandos\r\n\r\n");

      terminal_puts(term, "Comandos:\r\n");
      terminal_puts(term, "  :w - Guardar\r\n");
      terminal_puts(term, "  :q - Salir\r\n");
      terminal_puts(term, "  :wq - Guardar y salir\r\n\r\n");

      terminal_puts(term, "Atajos:\r\n");
      terminal_puts(term, "  ^S - Guardar\r\n");
      terminal_puts(term, "  ^X - Salir\r\n");
      terminal_puts(term, "  ^Q - Forzar salir\r\n");
      terminal_puts(term, "  ^G - Ayuda\r\n\r\n");

      terminal_puts(term, "Presione ESC para continuar...");

      editor_render_status_bar(editor);
    }

    terminal_show_cursor(term, false);
    return;
  }

  // ✅ Renderizar solo líneas visibles que cambiaron
  uint32_t viewport_end_y = editor->viewport_y + editor->viewport_height;
  if (viewport_end_y > editor->line_count) {
    viewport_end_y = editor->line_count;
  }

  // ✅ Usar el sistema de dirty lines del terminal
  for (uint32_t i = editor->viewport_y; i < viewport_end_y; i++) {
    uint32_t screen_y = i - editor->viewport_y;

    // Solo redibujar si la línea está sucia
    if (!term->dirty_lines[screen_y] && !term->needs_full_redraw) {
      continue;
    }

    if (i >= editor->line_count)
      break;

    editor_line_t *line = &editor->lines[i];

    // Limpiar solo esta línea
    fill_rect(0, screen_y * g_current_font.height,
              term->width * (g_current_font.width + g_current_font.spacing),
              g_current_font.height, term->bg_color);

    // Número de línea
    terminal_set_cursor(term, 0, screen_y);
    terminal_set_color(term, 0x808080, 0x000000);

    char line_num[8];
    snprintf(line_num, sizeof(line_num), "%4u ", i + 1);
    terminal_puts(term, line_num);

    terminal_set_color(term, 0xFFFFFF, 0x000000);

    // Renderizar contenido
    uint32_t display_x = 0;
    uint32_t j_start =
        (editor->viewport_x < line->length) ? editor->viewport_x : 0;

    for (uint32_t j = j_start; j < line->length && display_x < term->width - 5;
         j++) {
      char c = line->data[j];

      if (c == '\t') {
        uint32_t spaces = EDITOR_TAB_SIZE - (display_x % EDITOR_TAB_SIZE);
        for (uint32_t s = 0; s < spaces && display_x < term->width - 5; s++) {
          terminal_putchar(term, ' ');
          display_x++;
        }
      } else if (c >= 32 && c < 127) {
        terminal_putchar(term, c);
        display_x++;
      } else if (c != 0) {
        terminal_set_color(term, 0xFF0000, 0x000000);
        terminal_putchar(term, '?');
        terminal_set_color(term, 0xFFFFFF, 0x000000);
        display_x++;
      }
    }

    // Marcar líneas modificadas
    if (line->modified) {
      terminal_set_cursor(term, term->width - 1, screen_y);
      terminal_set_color(term, 0xFFFF00, 0x000000);
      terminal_putchar(term, '*');
      terminal_set_color(term, 0xFFFFFF, 0x000000);
    }

    term->dirty_lines[screen_y] = 0; // Marcar como limpia
  }

  // Renderizar líneas vacías (~) solo si es necesario
  if (term->needs_full_redraw) {
    for (uint32_t i = viewport_end_y - editor->viewport_y;
         i < editor->viewport_height; i++) {
      fill_rect(0, i * g_current_font.height,
                term->width * (g_current_font.width + g_current_font.spacing),
                g_current_font.height, term->bg_color);

      terminal_set_cursor(term, 0, i);
      terminal_set_color(term, 0x0000FF, 0x000000);
      terminal_putchar(term, '~');
      terminal_set_color(term, 0xFFFFFF, 0x000000);
    }
  }

  // ✅ Renderizar barra de estado SOLO si cambió
  static char last_status[256] = {0};
  static uint32_t last_cursor_x = 0, last_cursor_y = 0;
  static editor_mode_t last_status_mode = EDITOR_MODE_NORMAL;

  bool status_changed = (strcmp(last_status, editor->status_message) != 0) ||
                        (last_cursor_x != editor->cursor_x) ||
                        (last_cursor_y != editor->cursor_y) ||
                        (last_status_mode != editor->mode);

  if (status_changed || term->needs_full_redraw) {
    editor_render_status_bar(editor);
    strncpy(last_status, editor->status_message, sizeof(last_status) - 1);
    last_cursor_x = editor->cursor_x;
    last_cursor_y = editor->cursor_y;
    last_status_mode = editor->mode;
  }

  // ✅ Posicionar cursor
  uint32_t cursor_screen_x = 5;
  uint32_t cursor_screen_y = editor->cursor_y - editor->viewport_y;

  if (editor->cursor_y < editor->line_count) {
    editor_line_t *line = &editor->lines[editor->cursor_y];
    for (uint32_t i = editor->viewport_x;
         i < editor->cursor_x && i < line->length; i++) {
      if (line->data[i] == '\t') {
        cursor_screen_x +=
            EDITOR_TAB_SIZE - ((cursor_screen_x - 5) % EDITOR_TAB_SIZE);
      } else {
        cursor_screen_x++;
      }
    }
  }

  if (cursor_screen_x >= term->width)
    cursor_screen_x = term->width - 1;
  if (cursor_screen_y >= editor->viewport_height)
    cursor_screen_y = editor->viewport_height - 1;

  terminal_set_cursor(term, cursor_screen_x, cursor_screen_y);
  terminal_show_cursor(term, true);

  term->needs_full_redraw = 0;
}

void editor_render_status_bar(text_editor_t *editor) {
  if (!editor || !editor->term)
    return;

  Terminal *term = editor->term;
  uint32_t status_y = editor->viewport_height;

  // Primera línea de estado
  terminal_set_cursor(term, 0, status_y);
  terminal_set_color(term, 0x000000, 0xC0C0C0);

  // Nombre del archivo
  char filename_display[32];
  if (editor->filename[0]) {
    strncpy(filename_display, editor->filename, sizeof(filename_display) - 1);
    filename_display[sizeof(filename_display) - 1] = '\0';
  } else {
    strcpy(filename_display, "[Sin nombre]");
  }

  const char *mod_indicator = editor->modified ? " [+]" : "";

  const char *mode_str;
  switch (editor->mode) {
  case EDITOR_MODE_NORMAL:
    mode_str = "NRM";
    break;
  case EDITOR_MODE_INSERT:
    mode_str = "INS";
    break;
  case EDITOR_MODE_COMMAND:
    mode_str = "CMD";
    break;
  case EDITOR_MODE_SEARCH:
    mode_str = "SEA";
    break;
  case EDITOR_MODE_REPLACE:
    mode_str = "REP";
    break;
  case EDITOR_MODE_HELP:
    mode_str = "HLP";
    break;
  default:
    mode_str = "???";
    break;
  }

  terminal_printf(term, " %s%s | %s ", filename_display, mod_indicator,
                  mode_str);

  // Rellenar hasta el final
  uint32_t current_x = terminal_get_cursor_x(term);
  for (uint32_t i = current_x; i < term->width; i++) {
    terminal_putchar(term, ' ');
  }

  // Información de posición
  char pos_info[32];
  snprintf(pos_info, sizeof(pos_info), "Ln %u, Col %u ", editor->cursor_y + 1,
           editor->cursor_x + 1);
  terminal_set_cursor(term, term->width - strlen(pos_info), status_y);
  terminal_printf(term, "%s", pos_info);

  // Segunda línea de estado
  terminal_set_cursor(term, 0, status_y + 1);
  terminal_set_color(term, 0xFFFFFF, 0x000000);

  if (editor->mode == EDITOR_MODE_COMMAND) {
    terminal_putchar(term, ':');
    terminal_printf(term, "%s", editor->command_buffer);
  } else if (editor->mode == EDITOR_MODE_SEARCH) {
    terminal_putchar(term, '/');
    terminal_printf(term, "%s", editor->command_buffer);
  } else {
    terminal_printf(term, " %s", editor->status_message);
  }

  // Rellenar resto
  current_x = terminal_get_cursor_x(term);
  for (uint32_t i = current_x; i < term->width; i++) {
    terminal_putchar(term, ' ');
  }

  terminal_set_color(term, 0xFFFFFF, 0x000000);
}

// ============================================================================
// PROCESAMIENTO DE TECLAS
// ============================================================================

void editor_process_key(text_editor_t *editor, int key) {
  if (!editor)
    return;

  // Teclas globales
  if (key == 24) { // Ctrl+X
    if (editor->modified) {
      editor_set_status_message(
          editor, "Archivo modificado. ^S guardar, ^Q salir sin guardar");
    } else {
      editor->running = false;
    }
    needs_redraw = true;
    return;
  }

  if (key == 17) { // Ctrl+Q
    editor->running = false;
    needs_redraw = true;
    return;
  }

  if (key == 19) { // Ctrl+S
    if (editor->filename[0]) {
      editor_save_file(editor);
    } else {
      editor_set_status_message(editor, "Use :w nombre_archivo para guardar");
    }
    needs_redraw = true;
    return;
  }

  if (key == 7) { // Ctrl+G
    editor_show_help(editor);
    needs_redraw = true;
    return;
  }

  // Procesar según modo
  switch (editor->mode) {
  case EDITOR_MODE_NORMAL:
    editor_process_key_normal(editor, key);
    needs_redraw = true;
    break;
  case EDITOR_MODE_INSERT:
    editor_process_key_insert(editor, key);
    needs_redraw = true;
    break;
  case EDITOR_MODE_COMMAND:
    editor_process_key_command(editor, key);
    needs_redraw = true;
    break;
  case EDITOR_MODE_SEARCH:
    editor_process_key_search(editor, key);
    needs_redraw = true;
    break;
  case EDITOR_MODE_HELP:
    // En modo ayuda, solo ESC sale
    if (key == 27) { // ESC
      editor->mode = EDITOR_MODE_NORMAL;
      editor_set_status_message(editor, "");
      needs_redraw = true;
    }
    break;
  default:
    needs_redraw = true;
    break;
  }
}

void editor_process_key_normal(text_editor_t *editor, int key) {
  switch (key) {
  case 'i':
    editor->mode = EDITOR_MODE_INSERT;
    editor_set_status_message(editor, "-- INSERT --");
    needs_redraw = true;
    break;

  case 'a':
    editor->mode = EDITOR_MODE_INSERT;
    if (editor->cursor_x < editor->lines[editor->cursor_y].length) {
      editor->cursor_x++;
    }
    editor_set_status_message(editor, "-- INSERT --");
    needs_redraw = true;
    break;

  case 'o':
    editor_insert_newline(editor);
    editor->mode = EDITOR_MODE_INSERT;
    editor_set_status_message(editor, "-- INSERT --");
    needs_redraw = true;
    break;

  case 'O':
    if (editor->line_count < EDITOR_MAX_LINES) {
      if (editor->cursor_y > 0) {
        editor->cursor_y--;
        editor->cursor_x = editor->lines[editor->cursor_y].length;
        editor_insert_newline(editor);
      } else {
        memmove(&editor->lines[1], &editor->lines[0],
                editor->line_count * sizeof(editor_line_t));
        memset(&editor->lines[0], 0, sizeof(editor_line_t));
        editor->line_count++;
        editor->cursor_x = 0;
      }
      editor->mode = EDITOR_MODE_INSERT;
      editor_set_status_message(editor, "-- INSERT --");
    }
    needs_redraw = true;
    break;

  case ':':
    editor->mode = EDITOR_MODE_COMMAND;
    editor->command_length = 0;
    editor->command_buffer[0] = '\0';
    needs_redraw = true;
    break;

  case '/':
    editor->mode = EDITOR_MODE_SEARCH;
    editor->command_length = 0;
    editor->command_buffer[0] = '\0';
    needs_redraw = true;
    break;

  case 'n':
    editor_search_next(editor);
    needs_redraw = true;
    break;

  case 'x':
    editor_delete_char(editor);
    needs_redraw = true;
    break;

  case 'd':
    editor_delete_line(editor);
    needs_redraw = true;
    break;

  case 'y':
    editor_copy_line(editor);
    needs_redraw = true;
    break;

  case 'p':
    editor_paste(editor);
    needs_redraw = true;
    break;

  case 'u':
    editor_undo(editor);
    needs_redraw = true;
    break;

  // Navegación
  case 'h':
  case KEY_LEFT:
    editor_move_cursor(editor, -1, 0);
    needs_redraw = true;
    break;

  case 'l':
  case KEY_RIGHT:
    editor_move_cursor(editor, 1, 0);
    needs_redraw = true;
    break;

  case 'k':
  case KEY_UP:
    editor_move_cursor(editor, 0, -1);
    needs_redraw = true;
    break;

  case 'j':
  case KEY_DOWN:
    editor_move_cursor(editor, 0, 1);
    needs_redraw = true;
    break;

  case KEY_HOME:
  case '0':
    editor_move_cursor_home(editor);
    needs_redraw = true;
    break;

  case KEY_END:
  case '$':
    editor_move_cursor_end(editor);
    needs_redraw = true;
    break;

  case KEY_PGUP:
    editor_page_up(editor);
    needs_redraw = true;
    break;

  case KEY_PGDOWN:
    editor_page_down(editor);
    needs_redraw = true;
    break;

  default:
    needs_redraw = true;
    break;
  }
}

void editor_process_key_insert(text_editor_t *editor, int key) {
  if (key == 27) { // ESC
    editor->mode = EDITOR_MODE_NORMAL;
    if (editor->cursor_x > 0)
      editor->cursor_x--;
    editor_set_status_message(editor, "");
    needs_redraw = true;
    return;
  }

  switch (key) {
  case '\n':
  case '\r':
    editor_insert_newline(editor);
    needs_redraw = true;
    break;

  case '\b':
  case 127:
    editor_backspace(editor);
    needs_redraw = true;
    break;

  case KEY_DELETE:
    editor_delete_char(editor);
    needs_redraw = true;
    break;

  case '\t':
    for (int i = 0; i < EDITOR_TAB_SIZE; i++) {
      editor_insert_char(editor, ' ');
    }
    needs_redraw = true;
    break;

  case KEY_LEFT:
    editor_move_cursor(editor, -1, 0);
    needs_redraw = true;
    break;

  case KEY_RIGHT:
    editor_move_cursor(editor, 1, 0);
    needs_redraw = true;
    break;

  case KEY_UP:
    editor_move_cursor(editor, 0, -1);
    needs_redraw = true;
    break;

  case KEY_DOWN:
    editor_move_cursor(editor, 0, 1);
    needs_redraw = true;
    break;

  case KEY_HOME:
    editor_move_cursor_home(editor);
    needs_redraw = true;
    break;

  case KEY_END:
    editor_move_cursor_end(editor);
    needs_redraw = true;
    break;

  default:
    if (key >= 32 && key < 127) {
      editor_insert_char(editor, (char)key);
    }
    needs_redraw = true;
    break;
  }
}

void editor_process_key_command(text_editor_t *editor, int key) {
  if (key == 27) { // ESC
    editor->mode = EDITOR_MODE_NORMAL;
    editor_set_status_message(editor, "");
    needs_redraw = true;
    return;
  }

  if (key == '\n' || key == '\r') {
    editor_execute_command(editor, editor->command_buffer);
    editor->mode = EDITOR_MODE_NORMAL;
    needs_redraw = true;
    return;
  }

  if (key == '\b' || key == 127) {
    if (editor->command_length > 0) {
      editor->command_length--;
      editor->command_buffer[editor->command_length] = '\0';
    } else {
      editor->mode = EDITOR_MODE_NORMAL;
    }
    needs_redraw = true;
    return;
  }

  if (key >= 32 && key < 127 &&
      editor->command_length < sizeof(editor->command_buffer) - 1) {
    editor->command_buffer[editor->command_length++] = (char)key;
    editor->command_buffer[editor->command_length] = '\0';
  }
  needs_redraw = true;
}

void editor_process_key_search(text_editor_t *editor, int key) {
  if (key == 27) { // ESC
    editor->mode = EDITOR_MODE_NORMAL;
    editor_set_status_message(editor, "");
    needs_redraw = true;
    return;
  }

  if (key == '\n' || key == '\r') {
    editor_search(editor, editor->command_buffer);
    editor->mode = EDITOR_MODE_NORMAL;
    needs_redraw = true;
    return;
  }

  if (key == '\b' || key == 127) {
    if (editor->command_length > 0) {
      editor->command_length--;
      editor->command_buffer[editor->command_length] = '\0';
    } else {
      editor->mode = EDITOR_MODE_NORMAL;
    }
    needs_redraw = true;
    return;
  }

  if (key >= 32 && key < 127 &&
      editor->command_length < sizeof(editor->command_buffer) - 1) {
    editor->command_buffer[editor->command_length++] = (char)key;
    editor->command_buffer[editor->command_length] = '\0';
  }
  needs_redraw = true;
}

// ============================================================================
// COMANDOS
// ============================================================================

void editor_execute_command(text_editor_t *editor, const char *cmd) {
  if (!editor || !cmd || !cmd[0])
    return;

  if (strcmp(cmd, "w") == 0) {
    if (editor->filename[0]) {
      editor_save_file(editor);
    } else {
      editor_set_status_message(editor, "No hay nombre de archivo");
    }
  } else if (strncmp(cmd, "w ", 2) == 0) {
    const char *filename = cmd + 2;
    editor_save_as(editor, filename);
  } else if (strcmp(cmd, "q") == 0) {
    if (editor->modified) {
      editor_set_status_message(
          editor,
          "Archivo modificado. Use :q! para forzar o :wq para guardar y salir");
    } else {
      editor->running = false;
    }
  } else if (strcmp(cmd, "q!") == 0) {
    editor->running = false;
  } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
    if (editor->filename[0]) {
      if (editor_save_file(editor) == 0) {
        editor->running = false;
      }
    } else {
      editor_set_status_message(editor, "No hay nombre de archivo");
    }
  } else if (cmd[0] >= '0' && cmd[0] <= '9') {
    uint32_t line = atoi(cmd);
    editor_goto_line(editor, line);
  } else {
    editor_set_status_message(editor, "Comando desconocido");
  }
  needs_redraw = true;
}

void editor_show_help(text_editor_t *editor) {
  if (!editor)
    return;

  // Cambiar a modo ayuda
  editor->mode = EDITOR_MODE_HELP;
  editor_set_status_message(editor, "Presione ESC para continuar...");
  needs_redraw = true;
}

// ============================================================================
// BUCLE PRINCIPAL
// ============================================================================

void editor_run(text_editor_t *editor) {
  if (!editor)
    return;

  editor_set_active(editor);
  keyboard_set_handler(editor_keyboard_callback);

  editor->running = true;

  // Render inicial
  editor->term->needs_full_redraw = 1;
  editor_render(editor);

  uint32_t last_render = ticks_since_boot;
  uint32_t frame_time =
      5; // ~50ms entre frames (20 FPS es suficiente para un editor)

  while (editor->running) {
    uint32_t current_ticks = ticks_since_boot;

    // ✅ Solo redibujar si:
    // 1. Hay cambios (needs_redraw)
    // 2. Ha pasado suficiente tiempo (evitar redibujado excesivo)
    if (needs_redraw && (current_ticks - last_render >= frame_time)) {
      editor_render(editor);
      last_render = current_ticks;
      needs_redraw = false;
    }

    // ✅ Sleep más largo para reducir uso de CPU
    task_sleep(20); // 20ms entre checks
    task_yield();
  }

  // Limpiar al salir
  Terminal *term = editor->term;

  keyboard_set_handler(keyboard_terminal_handler);
  editor_set_active(NULL);

  terminal_clear(term);
  terminal_set_cursor(term, 0, 0);
  terminal_set_color(term, 0xFFFFFF, 0x000000);
  terminal_show_cursor(term, true);

  term->needs_full_redraw = 1;
  terminal_draw(term);

  terminal_puts(term, "Editor cerrado.\r\n");
  terminal_draw(term);
}

void editor_keyboard_callback(int key) {
  if (g_active_editor && key != 0) {
    editor_process_key(g_active_editor, key);
  }
}

void editor_set_active(text_editor_t *editor) { g_active_editor = editor; }