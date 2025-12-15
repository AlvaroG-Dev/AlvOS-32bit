#include "boot_log.h"
#include "drawing.h"
#include "string.h"
#include "font.h"
#include <stdarg.h>

boot_state_t boot_state = {
    .boot_phase = true,
    .current_line = 2,
    .max_lines = 0,
    .step_count = 0
};

static char current_step_message[256] = {0};

// Helper de bajo nivel: dibujar carácter sin modificar cursor global
static void boot_draw_char_direct(uint32_t x, uint32_t y, char c, uint32_t fg_color) {
    if (c < 0 || c > 127) return;
    
    const uint8_t* glyph = font8x8_basic[(uint8_t)c];
    
    for (uint32_t dy = 0; dy < 8; dy++) {
        uint8_t row = glyph[dy];
        for (uint32_t dx = 0; dx < 8; dx++) {
            if (row & (1 << dx)) {
                put_pixel(x + dx, y + dy, fg_color);
            }
        }
    }
}

// Helper de bajo nivel: dibujar string sin modificar cursor global
static void boot_draw_string_direct(uint32_t x, uint32_t y, const char* str, uint32_t fg_color) {
    uint32_t current_x = x;
    while (*str) {
        boot_draw_char_direct(current_x, y, *str, fg_color);
        current_x += 9; // 8 pixels de ancho + 1 de espaciado
        str++;
    }
}

void boot_log_init(void) {
    boot_state.boot_phase = true;
    boot_state.current_line = 2;
    boot_state.step_count = 0;
    boot_state.max_lines = (g_fb.height / 8) - 4; // Fuente 8x8
    
    // Limpiar pantalla con fondo negro
    for (uint32_t y = 0; y < g_fb.height; y++) {
        for (uint32_t x = 0; x < g_fb.width; x++) {
            put_pixel(x, y, COLOR_BLACK);
        }
    }
    
    // Dibujar banner superior
    uint32_t banner_y = 20;
    
    // Título principal (centrado)
    const char* title = "MicroKernel OS v1.0";
    uint32_t title_width = strlen(title) * 9;
    boot_draw_string_direct((g_fb.width - title_width) / 2, banner_y, title, BOOT_COLOR_INFO);
    
    // Subtítulo
    banner_y += 24;
    const char* subtitle = "32-bit x86 Kernel";
    uint32_t subtitle_width = strlen(subtitle) * 9;
    boot_draw_string_direct((g_fb.width - subtitle_width) / 2, banner_y, subtitle, BOOT_COLOR_TEXT);
    
    // Línea separadora superior
    banner_y += 24;
    for (uint32_t x = 50; x < g_fb.width - 50; x++) {
        put_pixel(x, banner_y, BOOT_COLOR_INFO);
        put_pixel(x, banner_y + 1, BOOT_COLOR_INFO);
    }
    
    // Mensaje de inicio de boot
    banner_y += 24;
    boot_draw_string_direct(50, banner_y, "Starting boot sequence...", BOOT_COLOR_TEXT);
    
    boot_state.current_line = (banner_y / 8) + 4;
    for (volatile int i = 0; i < 1000000; i++);
}

void boot_log_start(const char* message) {
    if (!boot_state.boot_phase) return;
    
    strncpy(current_step_message, message, sizeof(current_step_message) - 1);
    current_step_message[sizeof(current_step_message) - 1] = '\0';
    
    // Calcular posiciones
    uint32_t y_pos = boot_state.current_line * 8;
    uint32_t msg_x = 50;
    
    // Limpiar línea completa
    fill_rect(0, y_pos, g_fb.width, 8, COLOR_BLACK);
    
    // Dibujar bullet point
    boot_draw_char_direct(msg_x, y_pos, '*', BOOT_COLOR_INFO);
    
    // Dibujar mensaje
    boot_draw_string_direct(msg_x + 18, y_pos, current_step_message, BOOT_COLOR_TEXT);
    
    // Dibujar "..."
    uint32_t dots_x = msg_x + 18 + strlen(current_step_message) * 9;
    boot_draw_string_direct(dots_x, y_pos, " ...", BOOT_COLOR_WARN);
    for (volatile int i = 0; i < 75000000; i++);
}

void boot_log_ok(void) {
    if (!boot_state.boot_phase) return;
    
    uint32_t y_pos = boot_state.current_line * 8;
    uint32_t status_x = g_fb.width - 100;
    
    // Limpiar área del estado
    fill_rect(status_x - 60, y_pos, 100, 8, COLOR_BLACK);
    
    // Dibujar "[ OK ]"
    boot_draw_string_direct(status_x, y_pos, "[ OK ]", BOOT_COLOR_OK);
    
    boot_state.current_line++;
    boot_state.step_count++;
    for (volatile int i = 0; i < 1000000; i++);
}

void boot_log_error(void) {
    if (!boot_state.boot_phase) return;
    
    uint32_t y_pos = boot_state.current_line * 8;
    uint32_t status_x = g_fb.width - 100;
    
    // Limpiar área del estado
    fill_rect(status_x - 60, y_pos, 100, 8, COLOR_BLACK);
    
    // Dibujar "[ERROR]"
    boot_draw_string_direct(status_x, y_pos, "[ERROR]", BOOT_COLOR_ERROR);
    
    boot_state.current_line++;
    boot_state.step_count++;
    for (volatile int i = 0; i < 10000000; i++);
}

void boot_log_info(const char* format, ...) {
    if (!boot_state.boot_phase) return;
    
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    uint32_t y_pos = boot_state.current_line * 8;
    
    // Limpiar línea
    fill_rect(0, y_pos, g_fb.width, 8, COLOR_BLACK);
    
    // Dibujar flecha
    boot_draw_string_direct(80, y_pos, "->", BOOT_COLOR_INFO);
    
    // Dibujar mensaje
    boot_draw_string_direct(80 + 27, y_pos, buffer, 0x80FFFF);
    
    boot_state.current_line++;
    for (volatile int i = 0; i < 1000000; i++);
}

void boot_log_warn(const char* format, ...) {
    if (!boot_state.boot_phase) return;
    
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    uint32_t y_pos = boot_state.current_line * 8;
    
    // Limpiar línea
    fill_rect(0, y_pos, g_fb.width, 8, COLOR_BLACK);
    
    // Dibujar warning
    boot_draw_string_direct(80, y_pos, "!", BOOT_COLOR_WARN);
    boot_draw_string_direct(80 + 18, y_pos, buffer, BOOT_COLOR_WARN);
    
    boot_state.current_line++;
    for (volatile int i = 0; i < 1000000; i++);
}

void boot_log_finish(void) {
    boot_state.boot_phase = false;
    
    // Añadir espacio
    boot_state.current_line += 2;
    uint32_t y_pos = boot_state.current_line * 8;
    
    // Línea separadora inferior
    for (uint32_t x = 50; x < g_fb.width - 50; x++) {
        put_pixel(x, y_pos, BOOT_COLOR_INFO);
        put_pixel(x, y_pos + 1, BOOT_COLOR_INFO);
    }
    
    y_pos += 24;
    
    // Mensaje de éxito centrado
    const char* success_msg = "Boot completed!";
    uint32_t msg_len = strlen(success_msg) * 9;
    boot_draw_string_direct((g_fb.width - msg_len) / 2, y_pos, success_msg, BOOT_COLOR_OK);
    
    // Estadísticas de boot
    y_pos += 24;
    char stats[128];
    snprintf(stats, sizeof(stats), "Initialized %u subsystems", boot_state.step_count);
    uint32_t stats_len = strlen(stats) * 9;
    boot_draw_string_direct((g_fb.width - stats_len) / 2, y_pos, stats, BOOT_COLOR_TEXT);
    
    // Mensaje de espera
    y_pos += 24;
    const char* wait_msg = "Starting terminal...";
    uint32_t wait_len = strlen(wait_msg) * 9;
    boot_draw_string_direct((g_fb.width - wait_len) / 2, y_pos, wait_msg, 0x808080);
    
    // Esperar 3 segundos
    for (volatile int i = 0; i < 10000000; i++);
    
    // CRÍTICO: Resetear COMPLETAMENTE el estado de drawing.c
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_fg_color = COLOR_WHITE;
    g_bg_color = COLOR_BLACK;
}

bool boot_is_active(void) {
    return boot_state.boot_phase;
}

void boot_log_clear_screen(void) {
    for (uint32_t y = 0; y < g_fb.height; y++) {
        for (uint32_t x = 0; x < g_fb.width; x++) {
            put_pixel(x, y, COLOR_BLACK);
        }
    }
}

void boot_log_draw_progress_bar(uint32_t current, uint32_t total) {
    if (!boot_state.boot_phase) return;
    
    uint32_t bar_width = 400;
    uint32_t bar_height = 20;
    uint32_t bar_x = (g_fb.width - bar_width) / 2;
    uint32_t bar_y = g_fb.height - 100;
    
    // Borde de la barra
    draw_rect(bar_x, bar_y, bar_width, bar_height, BOOT_COLOR_TEXT);
    
    // Fondo de la barra
    fill_rect(bar_x + 2, bar_y + 2, bar_width - 4, bar_height - 4, COLOR_DARK_GRAY);
    
    // Progreso
    uint32_t progress_width = ((bar_width - 4) * current) / total;
    fill_rect(bar_x + 2, bar_y + 2, progress_width, bar_height - 4, BOOT_COLOR_INFO);
    
    // Porcentaje
    char percent[16];
    snprintf(percent, sizeof(percent), "%u%%", (current * 100) / total);
    uint32_t percent_len = strlen(percent) * 9;
    boot_draw_string_direct(bar_x + (bar_width - percent_len) / 2, 
                           bar_y + (bar_height - 8) / 2, 
                           percent, COLOR_WHITE);
}