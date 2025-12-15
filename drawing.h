#ifndef DRAWING_H
#define DRAWING_H

#include <stddef.h>
#include <stdint.h>
#include "math_utils.h"

// Definiciones de colores (formato 0xRRGGBB)
#define COLOR_BLACK         0x000000
#define COLOR_WHITE         0xFFFFFF
#define COLOR_RED           0xFF0000
#define COLOR_GREEN         0x00FF00
#define COLOR_BLUE          0x0000FF
#define COLOR_YELLOW        0xFFFF00
#define COLOR_CYAN          0x00FFFF
#define COLOR_MAGENTA       0xFF00FF
#define COLOR_GRAY          0x808080
#define COLOR_DARK_GRAY     0x404040
#define COLOR_ORANGE        0xFFA500
#define COLOR_TRANSPARENT   0xFFFFFFFF
#define COLOR_DARK_BLUE     0x000080

// Estructura para píxeles en formato 24bpp
typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} __attribute__((packed)) Pixel24;

// Estructura del framebuffer
typedef struct {
    uint32_t* buffer32;    // Para 32bpp
    Pixel24* buffer24;     // Para 24bpp
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        // En bytes
    uint32_t bpp;          // Bits por píxel (24 o 32)
} Framebuffer;

// Tipos de fuente disponibles
typedef enum {
    FONT_8x8_BASIC,
    FONT_8x8_BOLD,
    FONT_8x16_VGA,
    FONT_6x8_SLIM,
    FONT_8x16_TERMINUS,
    FONT_12x16_SANS,
    FONT_16x32_LARGE
} FontType;

typedef struct {
    const void* glyphs;  // Puntero genérico a los glifos
    uint8_t width;
    uint8_t height;
    uint8_t spacing;
    uint8_t bytes_per_glyph;  // Bytes por glifo (para saber cómo acceder)
} FontInfo;

// Variables globales del framebuffer
extern Framebuffer g_fb;
extern uint32_t g_fg_color;
extern uint32_t g_bg_color;
extern uint32_t g_cursor_x;
extern uint32_t g_cursor_y;
extern FontInfo g_current_font;

// Funciones de inicialización
void fb_init(void* framebuffer, uint32_t width, uint32_t height, uint32_t pitch_bytes, uint32_t bpp);
void set_font(FontType font_type);
void set_colors(uint32_t fg, uint32_t bg);

// Funciones básicas de dibujo
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color);
void fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color);
void draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void draw_thick_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t thickness, uint32_t color);
void draw_circle(uint32_t x0, uint32_t y0, uint32_t radius, uint32_t color);
void fill_circle(uint32_t x0, uint32_t y0, uint32_t radius, uint32_t color);
void draw_circle_quarter(uint32_t x0, uint32_t y0, uint32_t radius, uint8_t quarter, uint32_t color);
void draw_arc(uint32_t x0, uint32_t y0, uint32_t radius, float start_angle, float end_angle, uint32_t color);
void draw_triangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color);
void fill_triangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color);
void draw_scanline(int32_t x1, int32_t x2, int32_t y, uint32_t color);

// Funciones de texto
void put_char(char c);
void put_string(const char* str);
void draw_string(uint32_t x, uint32_t y, const char* str, uint32_t fg_color, uint32_t bg_color);
void set_cursor_pos(uint32_t x, uint32_t y);
void clear_screen();
void scroll_screen();

// Funciones de dibujo avanzado
void draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* bitmap, uint32_t color);
void draw_color_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* bitmap);
void draw_rounded_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, const char* text, uint32_t color);
void draw_big_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t fg_color, uint32_t bg_color);
void draw_big_string(uint32_t x, uint32_t y, const char* str, uint32_t scale, uint32_t fg_color, uint32_t bg_color);
void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg_color, uint32_t bg_color);
void draw_char_with_shadow(uint32_t x, uint32_t y, char c, uint32_t fg_color, uint32_t bg_color, uint32_t shadow_color, uint8_t shadow_offset);
void draw_string_with_shadow(uint32_t x, uint32_t y, const char* str, uint32_t fg_color, uint32_t bg_color, uint32_t shadow_color, uint8_t shadow_offset);
int printf(const char *format, ...);

#endif // DRAWING_H