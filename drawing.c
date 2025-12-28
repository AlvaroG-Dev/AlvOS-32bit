#include "drawing.h"
#include "font.h"
#include "kernel.h"
#include "memutils.h"
#include "string.h"
#include "terminal.h"

// Variables globales del framebuffer
Framebuffer g_fb = {0};
uint32_t g_fg_color = COLOR_WHITE;
uint32_t g_bg_color = COLOR_BLACK;
uint32_t g_cursor_x = 0;
uint32_t g_cursor_y = 0;
FontInfo g_current_font = {font8x8_basic, 8, 8, 1};

// Inicializa el framebuffer
void fb_init(void *framebuffer, uint32_t width, uint32_t height,
             uint32_t pitch_bytes, uint32_t bpp) {
  g_fb.buffer32 = (uint32_t *)framebuffer;
  g_fb.buffer24 = (bpp == 24) ? (Pixel24 *)framebuffer : NULL;
  g_fb.width = width;
  g_fb.height = height;
  g_fb.pitch = pitch_bytes; // PITCH EN BYTES (no en píxeles)
  g_fb.bpp = bpp;

  // CRÍTICO: Calcular pitch en píxeles para acceso rápido
  if (bpp == 32) {
    g_pitch_pixels = pitch_bytes / 4;
  } else if (bpp == 24) {
    g_pitch_pixels = pitch_bytes / 3;
  } else {
    g_pitch_pixels = width; // Fallback
  }

  // Verificar alineación
  if (g_pitch_pixels < width) {
    g_pitch_pixels = width; // Forzar a width si es inválido
  }
}

// Establece la fuente actual
void set_font(FontType font_type) {
  switch (font_type) {
  case FONT_8x8_BASIC:
    g_current_font.glyphs = font8x8_basic;
    g_current_font.width = 8;
    g_current_font.height = 8;
    g_current_font.spacing = 1;
    g_current_font.bytes_per_glyph = 8;
    break;
  case FONT_8x8_BOLD:
    g_current_font.glyphs = font8x8_bold;
    g_current_font.width = 8;
    g_current_font.height = 8;
    g_current_font.spacing = 1;
    g_current_font.bytes_per_glyph = 8;
    break;
  case FONT_8x16_VGA:
    g_current_font.glyphs = font8x16_vga;
    g_current_font.width = 8;
    g_current_font.height = 16;
    g_current_font.spacing = 1;
    g_current_font.bytes_per_glyph = 16;
    break;
  case FONT_6x8_SLIM:
    g_current_font.glyphs = font6x8_slim;
    g_current_font.width = 6;
    g_current_font.height = 8;
    g_current_font.spacing = 1;
    g_current_font.bytes_per_glyph = 6;
    break;
  }
}

// Establece colores de primer plano y fondo
void set_colors(uint32_t fg, uint32_t bg) {
  g_fg_color = fg;
  g_bg_color = bg;
}

// Dibuja un píxel en la posición (x,y)
void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= g_fb.width || y >= g_fb.height)
    return;

  if (g_fb.bpp == 32) {
    uint32_t pixel_offset = y * (g_fb.pitch / 4) + x;
    g_fb.buffer32[pixel_offset] = color;
  } else if (g_fb.bpp == 24) {
    Pixel24 *pixel = &g_fb.buffer24[y * (g_fb.pitch / 3) + x];
    pixel->red = (color >> 16) & 0xFF;
    pixel->green = (color >> 8) & 0xFF;
    pixel->blue = color & 0xFF;
  }
}

// Rellena un rectángulo
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  // Asegurarse de que el rectángulo esté dentro de los límites
  if (x >= g_fb.width || y >= g_fb.height)
    return;
  if (x + w > g_fb.width)
    w = g_fb.width - x;
  if (y + h > g_fb.height)
    h = g_fb.height - y;

  if (g_fb.bpp == 32) {
    for (uint32_t dy = 0; dy < h; dy++) {
      uint32_t *row = g_fb.buffer32 + (y + dy) * (g_fb.pitch / 4) + x;
      for (uint32_t dx = 0; dx < w; dx++) {
        row[dx] = color;
      }
    }
  } else if (g_fb.bpp == 24) {
    Pixel24 pixel = {.red = (color >> 16) & 0xFF,
                     .green = (color >> 8) & 0xFF,
                     .blue = color & 0xFF};
    for (uint32_t dy = 0; dy < h; dy++) {
      Pixel24 *row = g_fb.buffer24 + (y + dy) * (g_fb.pitch / 3) + x;
      for (uint32_t dx = 0; dx < w; dx++) {
        row[dx] = pixel;
      }
    }
  }
}

// Dibuja el contorno de un rectángulo
void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  // Líneas horizontales
  draw_line(x, y, x + w - 1, y, color);
  draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);

  // Líneas verticales
  draw_line(x, y, x, y + h - 1, color);
  draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

// Dibuja un rectángulo con bordes redondeados
void draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t radius, uint32_t color) {
  if (radius == 0) {
    draw_rect(x, y, w, h, color);
    return;
  }

  // Asegurar que el radio no sea demasiado grande
  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;

  // Dibujar las líneas rectas
  draw_line(x + radius, y, x + w - radius - 1, y, color); // Arriba
  draw_line(x + radius, y + h - 1, x + w - radius - 1, y + h - 1,
            color);                                       // Abajo
  draw_line(x, y + radius, x, y + h - radius - 1, color); // Izquierda
  draw_line(x + w - 1, y + radius, x + w - 1, y + h - radius - 1,
            color); // Derecha

  // Dibujar las esquinas redondeadas
  draw_circle_quarter(x + radius, y + radius, radius, 1,
                      color); // Superior izquierda
  draw_circle_quarter(x + w - radius - 1, y + radius, radius, 2,
                      color); // Superior derecha
  draw_circle_quarter(x + w - radius - 1, y + h - radius - 1, radius, 3,
                      color); // Inferior derecha
  draw_circle_quarter(x + radius, y + h - radius - 1, radius, 4,
                      color); // Inferior izquierda
}

// Rellena un rectángulo con bordes redondeados
void fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t radius, uint32_t color) {
  if (radius == 0) {
    fill_rect(x, y, w, h, color);
    return;
  }

  // Asegurar que el radio no sea demasiado grande
  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;

  // Rellenar el centro del rectángulo
  fill_rect(x, y + radius, w, h - 2 * radius, color);

  // Rellenar las partes superiores e inferiores
  for (uint32_t r = 0; r < radius; r++) {
    uint32_t width =
        (uint32_t)(sqrt(radius * radius - (radius - r) * (radius - r)) + 0.5);
    draw_line(x + radius - width, y + r, x + radius + width, y + r, color);
    draw_line(x + radius - width, y + h - 1 - r, x + radius + width,
              y + h - 1 - r, color);
  }
}

// Dibuja un cuarto de círculo (para bordes redondeados)
void draw_circle_quarter(uint32_t x0, uint32_t y0, uint32_t radius,
                         uint8_t quarter, uint32_t color) {
  int x = radius;
  int y = 0;
  int err = 0;

  while (x >= y) {
    switch (quarter) {
    case 1: // Superior izquierda
      put_pixel(x0 - y, y0 - x, color);
      put_pixel(x0 - x, y0 - y, color);
      break;
    case 2: // Superior derecha
      put_pixel(x0 + y, y0 - x, color);
      put_pixel(x0 + x, y0 - y, color);
      break;
    case 3: // Inferior derecha
      put_pixel(x0 + x, y0 + y, color);
      put_pixel(x0 + y, y0 + x, color);
      break;
    case 4: // Inferior izquierda
      put_pixel(x0 - x, y0 + y, color);
      put_pixel(x0 - y, y0 + x, color);
      break;
    }

    if (err <= 0) {
      y += 1;
      err += 2 * y + 1;
    }
    if (err > 0) {
      x -= 1;
      err -= 2 * x + 1;
    }
  }
}

// Algoritmo de Bresenham para líneas
void draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
               uint32_t color) {
  int dx = x1 > x0 ? (int)(x1 - x0) : (int)(x0 - x1);
  int dy = y1 > y0 ? (int)(y1 - y0) : (int)(y0 - y1);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = (dx > dy ? dx : -dy) / 2;
  int e2;

  while (1) {
    put_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    e2 = err;
    if (e2 > -dx) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dy) {
      err += dx;
      y0 += sy;
    }
  }
}

// Dibuja una línea con grosor
void draw_thick_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                     uint32_t thickness, uint32_t color) {
  if (thickness == 1) {
    draw_line(x0, y0, x1, y1, color);
    return;
  }

  int dx = x1 > x0 ? (int)(x1 - x0) : (int)(x0 - x1);
  int dy = y1 > y0 ? (int)(y1 - y0) : (int)(y0 - y1);

  if (dx > dy) {
    // Línea más horizontal
    for (uint32_t t = 0; t < thickness; t++) {
      int offset = t - thickness / 2;
      draw_line(x0, y0 + offset, x1, y1 + offset, color);
    }
  } else {
    // Línea más vertical
    for (uint32_t t = 0; t < thickness; t++) {
      int offset = t - thickness / 2;
      draw_line(x0 + offset, y0, x1 + offset, y1, color);
    }
  }
}

// Dibuja un círculo usando el algoritmo del punto medio
void draw_circle(uint32_t x0, uint32_t y0, uint32_t radius, uint32_t color) {
  int x = radius;
  int y = 0;
  int err = 0;

  while (x >= y) {
    put_pixel(x0 + x, y0 + y, color);
    put_pixel(x0 + y, y0 + x, color);
    put_pixel(x0 - y, y0 + x, color);
    put_pixel(x0 - x, y0 + y, color);
    put_pixel(x0 - x, y0 - y, color);
    put_pixel(x0 - y, y0 - x, color);
    put_pixel(x0 + y, y0 - x, color);
    put_pixel(x0 + x, y0 - y, color);

    if (err <= 0) {
      y += 1;
      err += 2 * y + 1;
    }
    if (err > 0) {
      x -= 1;
      err -= 2 * x + 1;
    }
  }
}

// Rellena un círculo
void fill_circle(uint32_t x0, uint32_t y0, uint32_t radius, uint32_t color) {
  int x = radius;
  int y = 0;
  int err = 0;

  while (x >= y) {
    draw_line(x0 - x, y0 + y, x0 + x, y0 + y, color);
    draw_line(x0 - y, y0 + x, x0 + y, y0 + x, color);
    draw_line(x0 - x, y0 - y, x0 + x, y0 - y, color);
    draw_line(x0 - y, y0 - x, x0 + y, y0 - x, color);

    if (err <= 0) {
      y += 1;
      err += 2 * y + 1;
    }
    if (err > 0) {
      x -= 1;
      err -= 2 * x + 1;
    }
  }
}

// Dibuja un arco de círculo
void draw_arc(uint32_t x0, uint32_t y0, uint32_t radius, float start_angle,
              float end_angle, uint32_t color) {
  float angle = start_angle;
  float angle_step = 1.0f / radius; // Paso angular basado en el radio

  while (angle <= end_angle) {
    int x = x0 + (int)(radius * cos(angle));
    int y = y0 + (int)(radius * sin(angle));
    put_pixel(x, y, color);
    angle += angle_step;
  }
}

// Dibuja un triángulo
void draw_triangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                   uint32_t x2, uint32_t y2, uint32_t color) {
  draw_line(x0, y0, x1, y1, color);
  draw_line(x1, y1, x2, y2, color);
  draw_line(x2, y2, x0, y0, color);
}

void draw_scanline(int32_t x1, int32_t x2, int32_t y, uint32_t color) {
  if (x1 > x2) {
    int32_t tmp = x1;
    x1 = x2;
    x2 = tmp;
  }
  for (int32_t x = x1; x <= x2; x++) {
    put_pixel(x, y, color);
  }
}

// Rellena un triángulo
void fill_triangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                   uint32_t x2, uint32_t y2, uint32_t color) {
  // Algoritmo de relleno de triángulo usando scanlines

  int32_t temp;
  int32_t x, y;

  // Ordenar los vértices por y
  if (y0 > y1) {
    temp = y0;
    y0 = y1;
    y1 = temp;
    temp = x0;
    x0 = x1;
    x1 = temp;
  }
  if (y0 > y2) {
    temp = y0;
    y0 = y2;
    y2 = temp;
    temp = x0;
    x0 = x2;
    x2 = temp;
  }
  if (y1 > y2) {
    temp = y1;
    y1 = y2;
    y2 = temp;
    temp = x1;
    x1 = x2;
    x2 = temp;
  }

  if (y1 == y2) {
    // Triángulo inferior plano
    for (y = y0; y <= y1; y++) {
      int32_t xa = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
      int32_t xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
      draw_scanline(xa, xb, y, color);
    }
  } else if (y0 == y1) {
    // Triángulo superior plano
    for (y = y0; y <= y2; y++) {
      int32_t xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
      int32_t xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
      draw_scanline(xa, xb, y, color);
    }
  } else {
    // Triángulo general (parte superior e inferior)
    // Parte superior
    for (y = y0; y <= y1; y++) {
      int32_t xa = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
      int32_t xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
      draw_scanline(xa, xb, y, color);
    }
    // Parte inferior
    for (y = y1; y <= y2; y++) {
      int32_t xa = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
      int32_t xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
      draw_scanline(xa, xb, y, color);
    }
  }
}

void put_char(char c) {
  if (c == '\n') {
    g_cursor_x = 0;
    g_cursor_y += g_current_font.height;
    if (g_cursor_y >= g_fb.height - g_current_font.height) {
      scroll_screen();
      g_cursor_y -= g_current_font.height;
    }
    return;
  }

  if (c == '\r') {
    g_cursor_x = 0;
    return;
  }

  if (c == '\t') {
    g_cursor_x = (g_cursor_x + 4 * g_current_font.width) &
                 ~(4 * g_current_font.width - 1);
    if (g_cursor_x >= g_fb.width - g_current_font.width) {
      g_cursor_x = 0;
      g_cursor_y += g_current_font.height;
      if (g_cursor_y >= g_fb.height - g_current_font.height) {
        scroll_screen();
        g_cursor_y -= g_current_font.height;
      }
    }
    return;
  }

  // Obtener el glifo del carácter
  const uint8_t *glyph = NULL;
  if (g_current_font.bytes_per_glyph == 8) {
    glyph = ((const uint8_t (*)[8])g_current_font.glyphs)[(uint8_t)c];
  } else if (g_current_font.bytes_per_glyph == 16) {
    glyph = ((const uint8_t (*)[16])g_current_font.glyphs)[(uint8_t)c];
  } else if (g_current_font.bytes_per_glyph == 6) {
    glyph = ((const uint8_t (*)[6])g_current_font.glyphs)[(uint8_t)c];
  }

  if (!glyph)
    return; // Carácter no soportado

  // Dibujar el glifo
  for (uint32_t dy = 0; dy < g_current_font.height; dy++) {
    uint8_t row = glyph[dy];
    for (uint32_t dx = 0; dx < g_current_font.width; dx++) {
      // Comprobar si el bit está activo para este píxel
      // Modificado para manejar diferentes órdenes de bits según la fuente
      uint32_t bit_mask;
      if (g_current_font.glyphs == font8x16_vga) {
        // Para VGA, el bit más significativo está a la izquierda
        bit_mask = 1 << (g_current_font.width - 1 - dx);
      } else {
        // Para otras fuentes, el bit más significativo está a la derecha
        bit_mask = 1 << dx;
      }

      if (row & bit_mask) {
        put_pixel(g_cursor_x + dx, g_cursor_y + dy, g_fg_color);
      } else if (g_bg_color != COLOR_TRANSPARENT) {
        put_pixel(g_cursor_x + dx, g_cursor_y + dy, g_bg_color);
      }
    }
  }

  // Mover el cursor
  g_cursor_x += g_current_font.width + g_current_font.spacing;
  if (g_cursor_x >= g_fb.width - g_current_font.width) {
    g_cursor_x = 0;
    g_cursor_y += g_current_font.height;
    if (g_cursor_y >= g_fb.height - g_current_font.height) {
      scroll_screen();
      g_cursor_y -= g_current_font.height;
    }
  }
}

// Dibuja una cadena de texto
void put_string(const char *str) {
  while (*str) {
    put_char(*str++);
  }
}

// Dibuja una cadena de texto en una posición específica
void draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg_color,
                 uint32_t bg_color) {
  uint32_t old_fg = g_fg_color;
  uint32_t old_bg = g_bg_color;
  uint32_t old_x = g_cursor_x;
  uint32_t old_y = g_cursor_y;

  g_fg_color = fg_color;
  g_bg_color = bg_color;
  g_cursor_x = x;
  g_cursor_y = y;

  while (*str) {
    put_char(*str++);
  }

  g_fg_color = old_fg;
  g_bg_color = old_bg;
  g_cursor_x = old_x;
  g_cursor_y = old_y;
}

// Establece la posición del cursor
void set_cursor_pos(uint32_t x, uint32_t y) {
  g_cursor_x = x * (g_current_font.width + g_current_font.spacing);
  g_cursor_y = y * g_current_font.height;
}

// Limpia la pantalla con el color de fondo
void clear_screen() {
  if (g_fb.bpp == 32) {
    for (uint32_t y = 0; y < g_fb.height; y++) {
      uint32_t *row = g_fb.buffer32 + y * (g_fb.pitch / 4);
      for (uint32_t x = 0; x < g_fb.width; x++) {
        row[x] = g_bg_color;
      }
    }
  } else if (g_fb.bpp == 24) {
    Pixel24 bg_pixel = {.red = (g_bg_color >> 16) & 0xFF,
                        .green = (g_bg_color >> 8) & 0xFF,
                        .blue = g_bg_color & 0xFF};

    for (uint32_t y = 0; y < g_fb.height; y++) {
      Pixel24 *row = g_fb.buffer24 + y * (g_fb.pitch / 3);
      for (uint32_t x = 0; x < g_fb.width; x++) {
        row[x] = bg_pixel;
      }
    }
  }

  g_cursor_x = 0;
  g_cursor_y = 0;
}

// Desplaza la pantalla hacia arriba
void scroll_screen() {
  if (g_fb.bpp == 32) {
    uint32_t bytes_per_row = g_fb.pitch;
    uint32_t rows_to_scroll = g_current_font.height;

    // Mover los datos de la pantalla hacia arriba
    memmove(g_fb.buffer32, g_fb.buffer32 + rows_to_scroll * (bytes_per_row / 4),
            (g_fb.height - rows_to_scroll) * bytes_per_row);

    // Limpiar la última fila
    fill_rect(0, g_fb.height - rows_to_scroll, g_fb.width, rows_to_scroll,
              g_bg_color);
  } else if (g_fb.bpp == 24) {
    uint32_t bytes_per_row = g_fb.pitch;
    uint32_t rows_to_scroll = g_current_font.height;

    // Mover los datos de la pantalla hacia arriba
    memmove(g_fb.buffer24, g_fb.buffer24 + rows_to_scroll * (bytes_per_row / 3),
            (g_fb.height - rows_to_scroll) * bytes_per_row);

    // Limpiar la última fila
    fill_rect(0, g_fb.height - rows_to_scroll, g_fb.width, rows_to_scroll,
              g_bg_color);
  }
}

// Dibuja un bitmap en la posición (x,y)
void draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint8_t *bitmap, uint32_t color) {
  for (uint32_t dy = 0; dy < h; dy++) {
    for (uint32_t dx = 0; dx < w; dx++) {
      if (bitmap[dy * ((w + 7) / 8) + dx / 8] & (1 << (7 - (dx % 8)))) {
        put_pixel(x + dx, y + dy, color);
      }
    }
  }
}

// Dibuja un bitmap con colores
void draw_color_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const uint32_t *bitmap) {
  for (uint32_t dy = 0; dy < h; dy++) {
    for (uint32_t dx = 0; dx < w; dx++) {
      put_pixel(x + dx, y + dy, bitmap[dy * w + dx]);
    }
  }
}

// Dibuja un botón con bordes redondeados
void draw_rounded_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t radius, const char *text, uint32_t color) {
  // Dibujar el fondo del botón
  fill_rounded_rect(x, y, w, h, radius, g_bg_color);

  // Dibujar el borde
  draw_rounded_rect(x, y, w, h, radius, color);

  // Centrar el texto
  uint32_t text_width =
      strlen(text) * (g_current_font.width + g_current_font.spacing);
  uint32_t text_x = x + (w - text_width) / 2;
  uint32_t text_y = y + (h - g_current_font.height) / 2;

  // Dibujar el texto
  draw_string(text_x, text_y, text, color, g_bg_color);
}

// Dibuja un carácter grande (2x o más)
void draw_big_char(uint32_t x, uint32_t y, char c, uint32_t scale,
                   uint32_t fg_color, uint32_t bg_color) {
  const uint8_t *glyph;
  if (g_current_font.bytes_per_glyph == 8) {
    glyph = ((const uint8_t (*)[8])g_current_font.glyphs)[(uint8_t)c];
  } else if (g_current_font.bytes_per_glyph == 16) {
    glyph = ((const uint8_t (*)[16])g_current_font.glyphs)[(uint8_t)c];
  } else if (g_current_font.bytes_per_glyph == 6) {
    glyph = ((const uint8_t (*)[6])g_current_font.glyphs)[(uint8_t)c];
  } else {
    return; // Tipo de fuente no soportado
  }

  for (uint32_t dy = 0; dy < g_current_font.height; dy++) {
    for (uint32_t dx = 0; dx < g_current_font.width; dx++) {
      uint32_t pixel_color =
          (glyph[dy] & (1 << (g_current_font.width - 1 - dx))) ? fg_color
                                                               : bg_color;
      if (pixel_color != COLOR_TRANSPARENT) {
        fill_rect(x + dx * scale, y + dy * scale, scale, scale, pixel_color);
      }
    }
  }
}

// Dibuja una cadena de texto grande
void draw_big_string(uint32_t x, uint32_t y, const char *str, uint32_t scale,
                     uint32_t fg_color, uint32_t bg_color) {
  uint32_t current_x = x;
  while (*str) {
    draw_big_char(current_x, y, *str, scale, fg_color, bg_color);
    current_x += (g_current_font.width + g_current_font.spacing) * scale;
    str++;
  }
}

void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg_color,
               uint32_t bg_color) {
  draw_char_with_shadow(x, y, c, fg_color, bg_color, bg_color, 0);
}

// Dibuja un carácter con sombra
void draw_char_with_shadow(uint32_t x, uint32_t y, char c, uint32_t fg_color,
                           uint32_t bg_color, uint32_t shadow_color,
                           uint8_t shadow_offset) {
  // Dibujar sombra primero
  if (shadow_offset > 0) {
    uint32_t old_fg = g_fg_color;
    uint32_t old_bg = g_bg_color;

    g_fg_color = shadow_color;
    g_bg_color = COLOR_TRANSPARENT;

    uint32_t old_x = g_cursor_x;
    uint32_t old_y = g_cursor_y;

    g_cursor_x = x + shadow_offset;
    g_cursor_y = y + shadow_offset;
    put_char(c);

    g_cursor_x = old_x;
    g_cursor_y = old_y;
    g_fg_color = old_fg;
    g_bg_color = old_bg;
  }

  // Dibujar carácter principal
  uint32_t old_fg = g_fg_color;
  uint32_t old_bg = g_bg_color;

  g_fg_color = fg_color;
  g_bg_color = bg_color;

  uint32_t old_x = g_cursor_x;
  uint32_t old_y = g_cursor_y;

  g_cursor_x = x;
  g_cursor_y = y;
  put_char(c);

  g_cursor_x = old_x;
  g_cursor_y = old_y;
  g_fg_color = old_fg;
  g_bg_color = old_bg;
}

// Dibuja una cadena con sombra
void draw_string_with_shadow(uint32_t x, uint32_t y, const char *str,
                             uint32_t fg_color, uint32_t bg_color,
                             uint32_t shadow_color, uint8_t shadow_offset) {
  uint32_t current_x = x;
  while (*str) {
    draw_char_with_shadow(current_x, y, *str, fg_color, bg_color, shadow_color,
                          shadow_offset);
    current_x += g_current_font.width + g_current_font.spacing;
    str++;
  }
}

int printf(const char *format, ...) {
  va_list args;
  va_start(args, format);

  char buffer[256];
  int len = vsnprintf(buffer, sizeof(buffer), format, args);

  put_string(buffer);

  va_end(args);
  return len;
}