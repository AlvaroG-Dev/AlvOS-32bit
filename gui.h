#ifndef GUI_H
#define GUI_H

#include "drawing.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>

// Colores Windows 2000
#define GUI_COLOR_BG 0x3D7E7E           // Teal clásico
#define GUI_COLOR_WIN_BG 0xC0C0C0       // Silver
#define GUI_COLOR_TITLE_ACTIVE 0x000080 // Navy
#define GUI_COLOR_TITLE_INACT 0x808080  // Gray
#define GUI_COLOR_TEXT 0x000000         // Black
#define GUI_COLOR_BTN_HIGH 0xFFFFFF     // White (brillo)
#define GUI_COLOR_BTN_SHADOW 0x808080   // Gray (sombra)
#define GUI_COLOR_BTN_DARK 0x404040     // Dark gray (sombra exterior)
#define GUI_COLOR_LIGHT_GRAY 0xE0E0E0
#define GUI_COLOR_DARK_BLUE 0x0000A0
#define GUI_COLOR_WHITE 0xFFFFFF

// Tipos de eventos
#define GUI_EVENT_MOUSE_MOVE 1
#define GUI_EVENT_MOUSE_DOWN 2
#define GUI_EVENT_MOUSE_UP 3
#define GUI_EVENT_MOUSE_SCROLL 4
#define GUI_EVENT_KEY_DOWN 5
#define GUI_EVENT_RESIZE 6

#define MAX_WINDOWS 12
#define WIN_TITLE_MAX 64

typedef struct window {
  uint32_t id;
  int32_t x, y;
  uint32_t w, h;
  char title[WIN_TITLE_MAX];
  bool active;
  bool dragging;
  bool resizing;
  int32_t drag_off_x;
  int32_t drag_off_y;
  bool minimized;
  bool resizable;

  void *user_data; // ✅ NUEVO: Datos de usuario (ej: ruta imagen)

  // Callback para dibujo del cliente
  void (*on_draw)(struct window *self);
  // Callback para eventos
  void (*on_event)(struct window *self, int type, int p1, int p2);
} window_t;

typedef struct {
  uint16_t w;
  uint16_t h;
  uint32_t *pixels;
} gui_image_t;

typedef enum {
  GUI_CURSOR_ARROW = 0,
  GUI_CURSOR_TEXT,
  GUI_CURSOR_HAND,
  GUI_CURSOR_RESIZE_NS,
  GUI_CURSOR_RESIZE_EW,
  GUI_CURSOR_RESIZE_NWSE,
  GUI_CURSOR_RESIZE_NESW
} gui_cursor_type_t;

typedef struct {
  window_t windows[MAX_WINDOWS];
  uint32_t window_count;
  uint32_t active_window_idx;

  uint32_t *backbuffer;
  bool needs_redraw;
  bool start_menu_open; // ✅ NUEVO: Estado del menú Inicio

  int32_t mouse_x, mouse_y;
  gui_cursor_type_t current_cursor; // ✅ NUEVO: Tipo de cursor actual
  uint8_t mouse_buttons;
  uint8_t last_mouse_buttons;
  int32_t pressing_close_idx; // ✅ NUEVO: Índice de la ventana cuya 'X' está
                              // presionada
  bool running;
} gui_context_t;

// Prototipos
void gui_init(void);
void gui_main_task(void *arg);
window_t *gui_create_window(const char *title, int32_t x, int32_t y, uint32_t w,
                            uint32_t h);
void gui_draw_window_frame(window_t *win);
void gui_draw_bevel(int x, int y, int w, int h, bool pressed);
void gui_draw_button(int x, int y, int w, int h, const char *text,
                     bool pressed);
void gui_draw_image(int x, int y, const char *path);
gui_image_t *gui_load_image(const char *path);
void gui_draw_image_resource(int x, int y, gui_image_t *img, int cx, int cy,
                             int cw, int ch);
void gui_open_image_viewer(const char *path);
void gui_render(void);

#endif
