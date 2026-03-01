#include "gui.h"
#include "kernel.h"
#include "memory.h"
#include "mouse.h"
#include "rtc.h"
#include "string.h"

gui_context_t gui;

// Definición manual de cursores mejorados (1 byte por fila)
static const uint8_t cursor_arrow[12] = {
    0b10000000, 0b11000000, 0b11100000, 0b11110000, 0b11111000, 0b11111100,
    0b11111110, 0b11100000, 0b10110000, 0b00011000, 0b00011000, 0b00000000};

static const uint8_t cursor_text[12] = {
    0b01111100, 0b00010000, 0b00010000, 0b00010000, 0b00010000, 0b00010000,
    0b00010000, 0b00010000, 0b00010000, 0b00010000, 0b00010000, 0b01111100};

static const uint8_t cursor_hand[12] = {
    0b00010000, 0b00010000, 0b01010100, 0b01010100, 0b01111110, 0b01111110,
    0b01111110, 0b00111110, 0b00011100, 0b00000000, 0b00000000, 0b00000000};

static const uint8_t cursor_ns[12] = {
    0b00010000, 0b00111000, 0b01111100, 0b00010000, 0b00010000, 0b00010000,
    0b00010000, 0b00010000, 0b00010000, 0b01111100, 0b00111000, 0b00010000};

static const uint8_t cursor_ew[12] = {
    0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b10000001,
    0b11011011, 0b11111111, 0b11011011, 0b10000001, 0b00000000, 0b00000000};

static const uint8_t cursor_nwse[12] = {
    0b11100000, 0b11000000, 0b10100000, 0b00010000, 0b00001000, 0b00000100,
    0b00000010, 0b00000001, 0b00000101, 0b00000011, 0b00000111, 0b00000000};

static const uint8_t cursor_nesw[12] = {
    0b00000111, 0b00000011, 0b00000101, 0b00000001, 0b00000010, 0b00000100,
    0b00001000, 0b00010000, 0b10100000, 0b11000000, 0b11100000, 0b00000000};

extern Terminal main_terminal;

void gui_init(void) {
  memset(&gui, 0, sizeof(gui_context_t));

  // Asignar backbuffer
  uint32_t size = g_screen_width * g_screen_height * sizeof(uint32_t);
  gui.backbuffer = (uint32_t *)kernel_malloc(size);
  if (!gui.backbuffer) {
    // Error fatal para la GUI
    return;
  }

  // Desactivar cursor automático del driver para dibujo manual
  mouse_set_cursor_visible(false);

  gui.window_count = 0;
  gui.active_window_idx = 0;
  gui.needs_redraw = true;
  gui.start_menu_open = false;
  gui.pressing_close_idx = -1; // Inicializar
  gui.running = true;
}

static void welcome_on_draw(window_t *self) {
  int cx = self->x + 3;
  int cy = self->y + 21;
  int cw = self->w - 6;
  int ch = self->h - 24;

  draw_string_clipped(self->x + 20, self->y + 40, "Bienvenido a AlvOS v1.0",
                      COLOR_BLACK, GUI_COLOR_WIN_BG, cx, cy, cw, ch);
  draw_string_clipped(self->x + 20, self->y + 60,
                      "Interfaz estilo Windows 2000", COLOR_BLACK,
                      GUI_COLOR_WIN_BG, cx, cy, cw, ch);
  draw_string_clipped(self->x + 20, self->y + 100, " - Ventanas arrastrables",
                      COLOR_DARK_BLUE, GUI_COLOR_WIN_BG, cx, cy, cw, ch);
  draw_string_clipped(self->x + 20, self->y + 120,
                      " - Doble bufering (sin parpadeo)", COLOR_DARK_BLUE,
                      GUI_COLOR_WIN_BG, cx, cy, cw, ch);
  draw_string_clipped(self->x + 20, self->y + 140, " - Reloj en tiempo real",
                      COLOR_DARK_BLUE, GUI_COLOR_WIN_BG, cx, cy, cw, ch);
}

static void terminal_on_draw(window_t *self) {
  // Ajustar posición del terminal a la ventana
  main_terminal.win_x = self->x + 3;
  main_terminal.win_y = self->y + 21;
  main_terminal.win_w = self->w - 6;
  main_terminal.win_h = self->h - 24;

  // No redimensionamos el buffer AQUÍ (durante el dibujo) para evitar
  // crasheos y pérdida de datos. Solo dibujamos lo que quepa.
  terminal_draw(&main_terminal);
}

static void terminal_on_event(window_t *self, int type, int p1, int p2) {
  if (type == GUI_EVENT_MOUSE_SCROLL) {
    int8_t scroll = (int8_t)p1;
    if (scroll > 0) {
      for (int i = 0; i < scroll; i++)
        terminal_scroll_up(&main_terminal);
    } else if (scroll < 0) {
      for (int i = 0; i < -scroll; i++)
        terminal_scroll_down(&main_terminal);
    }
  } else if (type == GUI_EVENT_RESIZE) {
    // Forzar redibujado completo al redimensionar
    main_terminal.needs_full_redraw = true;
    terminal_recalculate_dimensions(&main_terminal);
  }
}

void gui_draw_bevel(int x, int y, int w, int h, bool pressed) {
  uint32_t c1 = pressed ? GUI_COLOR_BTN_SHADOW : GUI_COLOR_BTN_HIGH;
  uint32_t c2 = pressed ? GUI_COLOR_BTN_HIGH : GUI_COLOR_BTN_SHADOW;
  uint32_t c3 = pressed ? GUI_COLOR_BTN_DARK : 0; // Dark bevel outer

  // Top
  fill_rect(x, y, w, 1, c1);
  // Left
  fill_rect(x, y, 1, h, c1);
  // Bottom
  fill_rect(x, y + h - 1, w, 1, c2);
  // Right
  fill_rect(x + w - 1, y, 1, h, c2);

  if (!pressed) {
    // Add outer dark shadow
    fill_rect(x + 1, y + h - 2, w - 2, 1, GUI_COLOR_BTN_SHADOW);
    fill_rect(x + w - 2, y + 1, 1, h - 2, GUI_COLOR_BTN_SHADOW);
  }
}

void gui_draw_button(int x, int y, int w, int h, const char *text,
                     bool pressed) {
  fill_rect(x, y, w, h, GUI_COLOR_WIN_BG);
  gui_draw_bevel(x, y, w, h, pressed);
  int text_len = strlen(text);
  int tx = x + (w - text_len * 8) / 2;
  int ty = y + (h - 16) / 2;
  draw_string(tx, ty, text, COLOR_BLACK, GUI_COLOR_WIN_BG);
}

void gui_draw_window_frame(window_t *win) {
  // Fondo de ventana con sombra sutil
  fill_rect(win->x + 2, win->y + 2, win->w, win->h, 0x404040); // Sombra
  fill_rect(win->x, win->y, win->w, win->h, GUI_COLOR_WIN_BG);

  // Bevel exterior (efecto 3D)
  gui_draw_bevel(win->x, win->y, win->w, win->h, false);

  // Title bar con degradado (simulado con líneas)
  uint32_t c1 = win->active ? 0x000080 : 0x808080;
  uint32_t c2 = win->active ? 0x1084D0 : 0xB0B0B0;

  for (int i = 0; i < 18; i++) {
    uint32_t color = win->active ? (0x000080 + (i * 4)) : 0x808080;
    fill_rect(win->x + 3, win->y + 3 + i, win->w - 6, 1, color);
  }

  // Título
  draw_string(win->x + 8, win->y + 5, win->title, COLOR_WHITE,
              COLOR_TRANSPARENT);

  // Botón de cerrar (X)
  int btn_x = win->x + win->w - 20;
  int btn_y = win->y + 5;

  // Dibujar botón presionado si es el caso
  bool win_pressing_close = (gui.pressing_close_idx != -1 &&
                             &gui.windows[gui.pressing_close_idx] == win);
  gui_draw_button(btn_x, btn_y, 14, 14, "x", win_pressing_close);

  // Resize handle (bottom-right)
  if (win->resizable) {
    int rx = win->x + win->w - 12;
    int ry = win->y + win->h - 12;
    fill_rect(rx, ry, 10, 10, GUI_COLOR_WIN_BG);
    gui_draw_bevel(rx, ry, 10, 10, false);
    // Dibujar líneas diagonales del grabber
    for (int i = 2; i < 8; i += 2) {
      draw_line(rx + 10 - i, ry + 10, rx + 10, ry + 10 - i,
                GUI_COLOR_BTN_SHADOW);
    }
  }
}

static void gui_render_start_menu(void) {
  int menu_w = 160;
  int menu_h = 200;
  int menu_x = 0;
  int menu_y = g_screen_height - 28 - menu_h;

  // Fondo menú
  fill_rect(menu_x, menu_y, menu_w, menu_h, GUI_COLOR_WIN_BG);
  gui_draw_bevel(menu_x, menu_y, menu_w, menu_h, false);

  // Barra lateral azul estilo Windows
  fill_rect(menu_x + 2, menu_y + 2, 20, menu_h - 4, 0x000080);

  // Items
  const char *items[] = {"Programs", "Documents", "Settings", "Find",
                         "Help",     "Run...",    "",         "Shut Down..."};
  for (int i = 0; i < 8; i++) {
    if (items[i][0] == '\0') {
      fill_rect(menu_x + 25, menu_y + 10 + i * 24 + 10, menu_w - 30, 1,
                GUI_COLOR_BTN_SHADOW);
      fill_rect(menu_x + 25, menu_y + 10 + i * 24 + 11, menu_w - 30, 1,
                GUI_COLOR_BTN_HIGH);
      continue;
    }

    // Hover effect (básico)
    bool hover =
        (gui.mouse_x >= menu_x + 22 && gui.mouse_x < menu_x + menu_w - 2 &&
         gui.mouse_y >= menu_y + 5 + i * 24 &&
         gui.mouse_y < menu_y + 5 + (i + 1) * 24);

    if (hover) {
      fill_rect(menu_x + 22, menu_y + 5 + i * 24, menu_w - 24, 22, 0x000080);
      draw_string(menu_x + 30, menu_y + 8 + i * 24, items[i], COLOR_WHITE,
                  0x000080);
    } else {
      draw_string(menu_x + 30, menu_y + 8 + i * 24, items[i], COLOR_BLACK,
                  GUI_COLOR_WIN_BG);
    }
  }
}

void gui_render(void) {
  if (!gui.running)
    return;

  // 1. Limpiar backbuffer con el color de fondo
  // Usamos fill_rect sobre el backbuffer?
  // Wait, fill_rect usa g_fb.buffer32. Necesito una versión que use backbuffer
  // o cambiar g_fb temporalmente.

  uint32_t *original_buffer = g_fb.buffer32;
  g_fb.buffer32 = gui.backbuffer;

  // Dibujar fondo escritorio
  fill_rect(0, 0, g_screen_width, g_screen_height, GUI_COLOR_BG);

  // Dibujar barra de tareas
  int taskbar_h = 28;
  fill_rect(0, g_screen_height - taskbar_h, g_screen_width, taskbar_h,
            GUI_COLOR_WIN_BG);
  gui_draw_bevel(0, g_screen_height - taskbar_h, g_screen_width, taskbar_h,
                 false);

  // Botón Inicio
  fill_rect(4, g_screen_height - taskbar_h + 3, 60, 22, GUI_COLOR_WIN_BG);
  gui_draw_bevel(4, g_screen_height - taskbar_h + 3, 60, 22, false);
  draw_string(24, g_screen_height - taskbar_h + 7, "Start", COLOR_BLACK,
              GUI_COLOR_WIN_BG);

  // Reloj en la esquina derecha
  rtc_time_t now;
  rtc_get_time(&now);
  char time_str[16];
  snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", now.hour, now.minute,
           now.second);

  int clock_w = 80;
  int clock_x = g_screen_width - clock_w - 5;
  fill_rect(clock_x, g_screen_height - taskbar_h + 4, clock_w, 20,
            GUI_COLOR_WIN_BG);
  gui_draw_bevel(clock_x, g_screen_height - taskbar_h + 4, clock_w, 20,
                 true); // Reloj hundido
  draw_string(clock_x + 8, g_screen_height - taskbar_h + 7, time_str,
              COLOR_BLACK, GUI_COLOR_WIN_BG);

  // Dibujar ventanas (de abajo a arriba)
  for (uint32_t i = 0; i < gui.window_count; i++) {
    if (!gui.windows[i].minimized) {
      gui_draw_window_frame(&gui.windows[i]);
      if (gui.windows[i].on_draw) {
        gui.windows[i].on_draw(&gui.windows[i]);
      }
    }
  }

  // Dibujar Menú Inicio si está abierto
  if (gui.start_menu_open) {
    gui_render_start_menu();
  }

  // Dibujar cursor en dos pasadas para evitar artefactos en el borde
  const uint8_t *cur_ptr = cursor_arrow;
  if (gui.current_cursor == GUI_CURSOR_TEXT)
    cur_ptr = cursor_text;
  else if (gui.current_cursor == GUI_CURSOR_HAND)
    cur_ptr = cursor_hand;
  else if (gui.current_cursor == GUI_CURSOR_RESIZE_NS)
    cur_ptr = cursor_ns;
  else if (gui.current_cursor == GUI_CURSOR_RESIZE_EW)
    cur_ptr = cursor_ew;
  else if (gui.current_cursor == GUI_CURSOR_RESIZE_NWSE)
    cur_ptr = cursor_nwse;
  else if (gui.current_cursor == GUI_CURSOR_RESIZE_NESW)
    cur_ptr = cursor_nesw;

  // Pasada 1: Borde negro (contorno 1px)
  for (int y = 0; y < 12; y++) {
    for (int x = 0; x < 8; x++) {
      if (cur_ptr[y] & (0x80 >> x)) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            put_pixel(gui.mouse_x + x + dx, gui.mouse_y + y + dy, COLOR_BLACK);
          }
        }
      }
    }
  }

  // Pasada 2: Centro blanco
  for (int y = 0; y < 12; y++) {
    for (int x = 0; x < 8; x++) {
      if (cur_ptr[y] & (0x80 >> x)) {
        put_pixel(gui.mouse_x + x, gui.mouse_y + y, COLOR_WHITE);
      }
    }
  }

  // Restaurar original y copiar (Flip optimizado con SSE si está disponible)
  g_fb.buffer32 = original_buffer;
  memcpy(g_fb.buffer32, gui.backbuffer, g_screen_width * g_screen_height * 4);
}

void gui_main_task(void *arg) {
  (void)arg;
  gui_init();

  // Ventana de bienvenida
  window_t *welcome = gui_create_window("Welcome to AlvOS", 100, 100, 300, 200);
  if (welcome) {
    welcome->on_draw = welcome_on_draw;
    welcome->resizable = true; // ✅ Habilitar resizing
  }

  // Ventana de Terminal
  // Tamaño ajustado para una terminal estándar (por ejemplo 80x25 caracteres)
  // Con fuente 8x16: 640x400 + bordes
  window_t *term_win = gui_create_window("AlvOS Terminal", 50, 50, 650, 430);
  if (term_win) {
    term_win->on_draw = terminal_on_draw;
    term_win->on_event = terminal_on_event;
    term_win->resizable = true;
    main_terminal.windowed = true;
    main_terminal.win_x = term_win->x + 3;
    main_terminal.win_y = term_win->y + 21;
    main_terminal.win_w = term_win->w - 6;
    main_terminal.win_h = term_win->h - 24;
    terminal_recalculate_dimensions(&main_terminal);
    main_terminal.needs_full_redraw = true;
  }

  while (gui.running) {
    // Actualizar ratón y cursor
    int32_t mx, my;
    mouse_get_position(&mx, &my);
    gui.mouse_x = mx;
    gui.mouse_y = my;
    gui.mouse_buttons = mouse_get_buttons();
    int8_t scroll = mouse_get_scroll();

    // Lógica de cursor dinámico
    gui.current_cursor = GUI_CURSOR_ARROW;

    // Cursor de mano sobre el botón Start
    if (mx >= 4 && mx < 64 && my >= (int)g_screen_height - 28) {
      gui.current_cursor = GUI_CURSOR_HAND;
    }

    for (int i = gui.window_count - 1; i >= 0; i--) {
      window_t *win = &gui.windows[i];
      if (mx >= win->x && mx < (int)(win->x + win->w) && my >= win->y &&
          my < (int)(win->y + win->h)) {

        // Cursor de redimensionamiento en la esquina inferior derecha
        if (win->resizable && mx >= (int)(win->x + win->w - 15) &&
            my >= (int)(win->y + win->h - 15)) {
          gui.current_cursor =
              GUI_CURSOR_RESIZE_NWSE; // Usaremos NS por simplicidad visual
          gui.current_cursor = GUI_CURSOR_RESIZE_NS;
        } else if (my >= win->y + 20 && my < (int)(win->y + win->h)) {
          // Área de contenido
          if (strcmp(win->title, "AlvOS Terminal") == 0) {
            gui.current_cursor = GUI_CURSOR_TEXT;
          }
        }
        break;
      }
    }

    // Manejar scroll en la ventana bajo el ratón
    if (scroll != 0 && gui.window_count > 0) {
      window_t *top = &gui.windows[gui.window_count - 1];
      // Verificar si el mouse está sobre la ventana de arriba (o la activa)
      if (gui.mouse_x >= top->x && gui.mouse_x < (int)(top->x + top->w) &&
          gui.mouse_y >= top->y && gui.mouse_y < (int)(top->y + top->h)) {
        if (top->on_event) {
          top->on_event(top, GUI_EVENT_MOUSE_SCROLL, scroll, 0);
        }
      }
    }

    if (gui.mouse_buttons & MOUSE_LEFT_BUTTON) {
      if (!(gui.last_mouse_buttons & MOUSE_LEFT_BUTTON)) {
        // Click en barra de tareas (Start Button)
        if (gui.mouse_x >= 4 && gui.mouse_x < 64 &&
            gui.mouse_y >= (int)g_screen_height - 28) {
          gui.start_menu_open = !gui.start_menu_open;
        } else if (gui.start_menu_open) {
          // Lógica Menú Inicio
          int menu_y = g_screen_height - 28 - 200;
          if (gui.mouse_x >= 0 && gui.mouse_x < 160 && gui.mouse_y >= menu_y &&
              gui.mouse_y < (int)g_screen_height - 28) {
            int item_idx = (gui.mouse_y - (menu_y + 5)) / 24;
            if (item_idx == 7) { // Shut Down
              shutdown();
            }
          }
          // Cerrar menú si se hace click fuera
          if (!(gui.mouse_x < 160 &&
                gui.mouse_y >= (int)g_screen_height - 28 - 200)) {
            gui.start_menu_open = false;
          }
        }

        // Click inicial: buscar ventana bajo el ratón (de arriba a abajo)
        for (int i = gui.window_count - 1; i >= 0; i--) {
          window_t *win = &gui.windows[i];

          // 1. Check Close button (X) PRIMERO
          if (gui.mouse_x >= (win->x + (int)win->w - 20) &&
              gui.mouse_x < (win->x + (int)win->w - 3) &&
              gui.mouse_y >= win->y + 4 && gui.mouse_y < win->y + 18) {
            gui.pressing_close_idx = i; // Solo marcar como presionado
            break;
          }

          // 2. Check resizing (bottom-right handle)
          if (win->resizable && gui.mouse_x >= (int)(win->x + win->w - 15) &&
              gui.mouse_x < (int)(win->x + win->w) &&
              gui.mouse_y >= (int)(win->y + win->h - 15) &&
              gui.mouse_y < (int)(win->y + win->h)) {
            win->resizing = true;
            win->drag_off_x = gui.mouse_x - win->w;
            win->drag_off_y = gui.mouse_y - win->h;

            // Traer al frente
            if (i != (int)gui.window_count - 1) {
              window_t tmp = *win;
              for (int j = i; j < (int)gui.window_count - 1; j++)
                gui.windows[j] = gui.windows[j + 1];
              gui.windows[gui.window_count - 1] = tmp;
            }
            break;
          }

          // 3. Check title bar or window area
          if (gui.mouse_x >= win->x && gui.mouse_x < win->x + (int)win->w &&
              gui.mouse_y >= win->y && gui.mouse_y < win->y + (int)win->h) {

            // Traer al frente
            if (i != (int)gui.window_count - 1) {
              window_t tmp = *win;
              for (int j = i; j < (int)gui.window_count - 1; j++)
                gui.windows[j] = gui.windows[j + 1];
              gui.windows[gui.window_count - 1] = tmp;
              win = &gui.windows[gui.window_count - 1];
            }

            win->active = true;
            // Solo drag si es en el title bar (y < y + 20)
            if (gui.mouse_y < win->y + 20) {
              win->dragging = true;
              win->drag_off_x = gui.mouse_x - win->x;
              win->drag_off_y = gui.mouse_y - win->y;
            }
            break;
          }
        }
      } else {
        // Continuar dragging o resizing
        for (uint32_t i = 0; i < gui.window_count; i++) {
          if (gui.windows[i].dragging) {
            gui.windows[i].x = gui.mouse_x - gui.windows[i].drag_off_x;
            gui.windows[i].y = gui.mouse_y - gui.windows[i].drag_off_y;
          } else if (gui.windows[i].resizing) {
            int32_t new_w = (int32_t)gui.mouse_x - gui.windows[i].drag_off_x;
            int32_t new_h = (int32_t)gui.mouse_y - gui.windows[i].drag_off_y;

            // Límites dinámicos: ancho mínimo basado en el título
            int32_t min_w = (strlen(gui.windows[i].title) * 9) + 60;
            if (new_w < min_w)
              new_w = min_w;
            if (new_h < 40)
              new_h = 40;

            gui.windows[i].w = (uint32_t)new_w;
            gui.windows[i].h = (uint32_t)new_h;
          }
        }
      }
    } else if (gui.last_mouse_buttons & MOUSE_LEFT_BUTTON) {
      // ✅ Mouse Up (Soltar botón)
      if (gui.pressing_close_idx != -1) {
        window_t *win = &gui.windows[gui.pressing_close_idx];
        // Verificar si seguimos sobre la X
        if (gui.mouse_x >= (win->x + (int)win->w - 20) &&
            gui.mouse_x < (win->x + (int)win->w - 3) &&
            gui.mouse_y >= win->y + 4 && gui.mouse_y < win->y + 18) {
          // Cerrar ahora sí
          for (uint32_t k = gui.pressing_close_idx; k < gui.window_count - 1;
               k++)
            gui.windows[k] = gui.windows[k + 1];
          gui.window_count--;
        }
        gui.pressing_close_idx = -1;
      }

      // Soltar drag/resize
      for (uint32_t i = 0; i < gui.window_count; i++) {
        if (gui.windows[i].resizing) {
          // ✅ Resize REAL ocurre aquí, al terminar el gesto
          if (gui.windows[i].on_event) {
            gui.windows[i].on_event(&gui.windows[i], GUI_EVENT_RESIZE,
                                    gui.windows[i].w, gui.windows[i].h);
          }
        }
        gui.windows[i].dragging = false;
        gui.windows[i].resizing = false;
      }
    }

    gui.last_mouse_buttons = gui.mouse_buttons;
    gui_render();
    task_sleep(2);
  }
}

window_t *gui_create_window(const char *title, int32_t x, int32_t y, uint32_t w,
                            uint32_t h) {
  if (gui.window_count >= MAX_WINDOWS)
    return NULL;

  window_t *win = &gui.windows[gui.window_count++];
  win->x = x;
  win->y = y;
  win->w = w;
  win->h = h;
  strncpy(win->title, title, WIN_TITLE_MAX - 1);
  win->active = true;
  win->dragging = false;
  win->resizing = false;
  win->minimized = false;
  win->resizable = true;
  win->user_data = NULL;
  win->on_draw = NULL;
  win->on_event = NULL;

  return win;
}

void gui_draw_image(int x, int y, const char *path) {
  int fd = vfs_open(path, VFS_O_RDONLY);
  if (fd < 0)
    return;

  uint8_t header[18];
  if (vfs_read(fd, header, 18) != 18) {
    vfs_close(fd);
    return;
  }

  // TGA Tipo 2 (Truecolor) o 3 (Grayscale) descomprimido
  if (header[2] != 2 && header[2] != 3) {
    vfs_close(fd);
    return;
  }

  uint16_t w = header[12] | (header[13] << 8);
  uint16_t h = header[14] | (header[15] << 8);
  uint8_t bpp = header[16];
  uint8_t descriptor = header[17];

  // Ignorar ID field si existe
  if (header[0] > 0) {
    uint8_t junk[256];
    vfs_read(fd, junk, header[0]);
  }

  uint32_t bytes_per_pixel = bpp / 8;
  static uint8_t row_buf[2048 * 4]; // Buffer estático para no saturar el stack
  uint32_t final_w = (w > 2048) ? 2048 : w;

  bool top_down = (descriptor & 0x20) != 0;

  for (uint32_t i = 0; i < h; i++) {
    int read_bytes = vfs_read(fd, row_buf, w * bytes_per_pixel);
    if (read_bytes <= 0)
      break;

    int32_t draw_y =
        top_down ? (y + (int32_t)i) : (y + (int32_t)h - 1 - (int32_t)i);

    for (uint32_t j = 0; j < final_w; j++) {
      uint8_t *p = &row_buf[j * bytes_per_pixel];
      uint32_t color;

      if (bpp == 32) {
        if (p[3] == 0)
          continue; // Alpha simple (transparente)
        color = (p[2] << 16) | (p[1] << 8) | p[0];
      } else if (bpp == 24) {
        color = (p[2] << 16) | (p[1] << 8) | p[0];
      } else if (bpp == 8) {
        color = (p[0] << 16) | (p[0] << 8) | p[0];
      } else {
        continue;
      }

      put_pixel(x + (int32_t)j, draw_y, color);
    }
  }

  vfs_close(fd);
}

void gui_draw_image_resource(int x, int y, gui_image_t *img, int cx, int cy,
                             int cw, int ch) {
  if (!img || !img->pixels)
    return;
  for (uint32_t i = 0; i < img->h; i++) {
    int32_t py = y + (int32_t)i;
    if (py < cy || py >= cy + ch)
      continue; // Clipping Y

    for (uint32_t j = 0; j < img->w; j++) {
      int32_t px = x + (int32_t)j;
      if (px < cx || px >= cx + cw)
        continue; // Clipping X

      uint32_t color = img->pixels[i * img->w + j];
      if ((color >> 24) != 0) {
        put_pixel(px, py, color);
      }
    }
  }
}

gui_image_t *gui_load_image(const char *path) {
  int fd = vfs_open(path, VFS_O_RDONLY);
  if (fd < 0)
    return NULL;

  uint8_t header[18];
  if (vfs_read(fd, header, 18) != 18) {
    vfs_close(fd);
    return NULL;
  }

  if (header[2] != 2 && header[2] != 3) {
    vfs_close(fd);
    return NULL;
  }

  uint16_t w = header[12] | (header[13] << 8);
  uint16_t h = header[14] | (header[15] << 8);
  uint8_t bpp = header[16];
  uint8_t descriptor = header[17];

  if (header[0] > 0) {
    uint8_t junk[256];
    vfs_read(fd, junk, header[0]);
  }

  gui_image_t *img = (gui_image_t *)kernel_malloc(sizeof(gui_image_t));
  if (!img) {
    vfs_close(fd);
    return NULL;
  }

  img->w = w;
  img->h = h;
  img->pixels = (uint32_t *)kernel_malloc(w * h * sizeof(uint32_t));
  if (!img->pixels) {
    kernel_free(img);
    vfs_close(fd);
    return NULL;
  }

  uint32_t bytes_per_pixel = bpp / 8;
  uint8_t *row_buf = (uint8_t *)kernel_malloc(w * bytes_per_pixel);
  bool top_down = (descriptor & 0x20) != 0;

  for (uint32_t i = 0; i < h; i++) {
    vfs_read(fd, row_buf, w * bytes_per_pixel);
    uint32_t draw_y = top_down ? i : (h - 1 - i);

    for (uint32_t j = 0; j < w; j++) {
      uint8_t *p = &row_buf[j * bytes_per_pixel];
      uint32_t color;
      if (bpp == 32)
        color = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
      else if (bpp == 24)
        color = (0xFF << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
      else if (bpp == 8)
        color = (0xFF << 24) | (p[0] << 16) | (p[0] << 8) | p[0];
      else
        color = 0;

      img->pixels[draw_y * w + j] = color;
    }
  }

  kernel_free(row_buf);
  vfs_close(fd);
  return img;
}

static void image_viewer_on_draw(window_t *self) {
  if (self->user_data) {
    gui_draw_image_resource(self->x + 3, self->y + 21,
                            (gui_image_t *)self->user_data, self->x + 3,
                            self->y + 21, self->w - 6, self->h - 24);
  }
}

void gui_open_image_viewer(const char *path) {
  gui_image_t *img = gui_load_image(path);
  if (!img)
    return;

  char title[128];
  snprintf(title, sizeof(title), "ImageViewer - %s", path);
  window_t *win = gui_create_window(title, 150, 150, img->w + 6, img->h + 24);
  if (win) {
    win->user_data = img;
    win->on_draw = image_viewer_on_draw;
    win->resizable = true;
  } else {
    kernel_free(img->pixels);
    kernel_free(img);
  }
}
