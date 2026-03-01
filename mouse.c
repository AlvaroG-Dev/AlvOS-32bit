#include "mouse.h"
#include "drawing.h"
#include "io.h"
#include "kernel.h"
#include "log.h"
#include "ps2.h"
#include "string.h"

static mouse_state_t mouse_state = {0};
static bool mouse_initialized = false;

// Cursor simple para modo terminal
static const uint8_t terminal_cursor[16] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
                                            0xFE, 0xFF, 0xF8, 0xD8, 0x8C, 0x0C,
                                            0x06, 0x06, 0x03, 0x00};

// Escribir al mouse usando el gestor PS/2
static void mouse_write(uint8_t value) { ps2_write_aux(value); }

// Leer del mouse usando el gestor PS/2
static uint8_t mouse_read(void) { return ps2_read_data(); }

// Configurar tasa de muestreo
static void mouse_set_sample_rate(uint8_t rate) {
  mouse_write(MOUSE_CMD_SET_SAMPLE_RATE);
  mouse_read();
  mouse_write(rate);
  mouse_read();
}

// Configurar resolución
static void mouse_set_resolution(uint8_t resolution) {
  mouse_write(MOUSE_CMD_SET_RESOLUTION);
  mouse_read();
  mouse_write(resolution);
  mouse_read();
}

static bool mouse_install(void) {
  uint8_t status;

  // Deshabilitar interrupciones durante inicialización crítica
  __asm__ volatile("cli");

  // Limpiar buffer de entrada por si acaso hay basura del teclado
  while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
    inb(PS2_DATA_PORT);
  }

  // Habilitar dispositivo mouse a través del comando PS/2 A8
  ps2_write_command(PS2_CMD_ENABLE_SCAN2);
  ps2_wait_write();

  // Leer configuración actual para no romper el teclado
  ps2_write_command(PS2_CMD_READ_CONFIG);
  status = ps2_read_data();

  // Habilitar IRQ12 y preservar IRQ1
  status |= 0x02;  // Habilitar mouse IRQ
  status |= 0x01;  // Asegurar teclado habilitado
  status &= ~0x20; // Habilitar clock del mouse (bit 5 es DISABLE_MOUSE_CLOCK)

  ps2_write_command(PS2_CMD_WRITE_CONFIG);
  ps2_write_data(status);
  ps2_wait_write();

  // Resetear el mouse
  mouse_write(MOUSE_CMD_RESET);
  uint8_t ack = mouse_read();
  if (ack != 0xFA) {
    __asm__ volatile("sti");
    log_message(LOG_WARN, "Mouse: No se pudo resetear (ACK: 0x%02X)", ack);
    return false;
  }

  // Leer resultado del test (BAT)
  uint8_t bat_result = mouse_read();
  if (bat_result != 0xAA) {
    __asm__ volatile("sti");
    log_message(LOG_WARN, "Mouse: Test BAT falló (0x%02X)", bat_result);
    return false;
  }

  // IMPORTANTE: Después del BAT el mouse envía su ID (0x00)
  // Si no se lee aquí, quedará en el buffer y romperá la secuencia de la rueda
  mouse_read();

  // CONFIGURACIÓN INICIAL: Set defaults antes de la secuencia de la rueda
  mouse_write(MOUSE_CMD_SET_DEFAULTS);
  mouse_read(); // ACK

  // Intentar habilitar rueda (IntelliMouse sequence)
  // Secuencia: Rate 200 -> Rate 100 -> Rate 80
  mouse_write(MOUSE_CMD_SET_SAMPLE_RATE);
  mouse_read();
  mouse_write(200);
  mouse_read();
  for (volatile int i = 0; i < 20000; i++)
    ;

  mouse_write(MOUSE_CMD_SET_SAMPLE_RATE);
  mouse_read();
  mouse_write(100);
  mouse_read();
  for (volatile int i = 0; i < 20000; i++)
    ;

  mouse_write(MOUSE_CMD_SET_SAMPLE_RATE);
  mouse_read();
  mouse_write(80);
  mouse_read();
  for (volatile int i = 0; i < 20000; i++)
    ;

  // Si devuelve ID 3 o 4, tiene rueda (4 es explorer mouse)
  mouse_write(MOUSE_CMD_GET_DEVICE_ID);
  mouse_read(); // ACK
  uint8_t mouse_id = mouse_read();
  mouse_state.has_wheel = (mouse_id == 3 || mouse_id == 4);

  // Configurar parámetros finales
  mouse_set_sample_rate(MOUSE_SAMPLE_RATE);
  mouse_set_resolution(MOUSE_RESOLUTION);

  // CONFIGURACIÓN FINAL: Habilitar reporting
  mouse_write(MOUSE_CMD_ENABLE_DATA_REP);
  mouse_read(); // ACK

  // Re-enable interrupts AFTER final config calls
  __asm__ volatile("sti");

  log_message(LOG_INFO, "Mouse: ID detected: %d (has_wheel: %d)", mouse_id,
              mouse_state.has_wheel);

  return true;
}

// Inicializar el mouse
void mouse_init(uint32_t screen_width, uint32_t screen_height) {
  if (mouse_initialized)
    return;

  memset(&mouse_state, 0, sizeof(mouse_state_t));

  mouse_state.screen_width = screen_width;
  mouse_state.screen_height = screen_height;
  mouse_state.max_x = screen_width - 1;
  mouse_state.max_y = screen_height - 1;

  mouse_state.x = screen_width / 2;
  mouse_state.y = screen_height / 2;
  mouse_state.cursor_visible = true;
  mouse_state.enabled = false;

  // Intentar inicialización múltiple veces para hardware real
  for (int attempts = 0; attempts < 3; attempts++) {
    if (mouse_install()) {
      mouse_state.enabled = true;
      break;
    }

    // Esperar antes de reintentar
    for (volatile int i = 0; i < 100000; i++)
      ;
  }

  mouse_initialized = true;
}

void mouse_handle_irq(void) {
  uint8_t status = ps2_read_status();

  // CRÍTICO: Solo leer el puerto de datos si el bit 5 (AUX_OUTPUT) está activo.
  // Si no está activo, el dato es del teclado y debemos dejar que su IRQ lo
  // maneje.
  if ((status & PS2_STATUS_OUTPUT_FULL) && (status & PS2_STATUS_AUX_OUTPUT)) {
    uint8_t data = inb(PS2_DATA_PORT);

    if (!mouse_state.enabled)
      return;

    // Manejar paquetes. Bit 3 del primer byte debe ser 1 (sync)
    if (mouse_state.packet_index == 0 && (data & 0x08)) {
      mouse_state.packet[0] = data;
      mouse_state.packet_index = 1;
    } else if (mouse_state.packet_index == 1) {
      mouse_state.packet[1] = data;
      mouse_state.packet_index = 2;
    } else if (mouse_state.packet_index == 2) {
      mouse_state.packet[2] = data;
      if (mouse_state.has_wheel) {
        mouse_state.packet_index = 3;
      } else {
        mouse_state.packet_index = 0;
        mouse_state.packet_ready = true;
        mouse_process_packet();
      }
    } else if (mouse_state.packet_index == 3) {
      mouse_state.packet[3] = data;
      mouse_state.packet_index = 0;
      mouse_state.packet_ready = true;
      mouse_process_packet();
    } else {
      mouse_state.packet_index = 0;
    }
  }
}

// Procesar paquete del mouse
void mouse_process_packet(void) {
  if (!mouse_state.packet_ready || !mouse_state.enabled)
    return;

  uint8_t *packet = mouse_state.packet;
  uint8_t flags = packet[0];

  // Validar paquete
  if (!(flags & 0x08)) {
    mouse_state.packet_index = 0;
    mouse_state.packet_ready = false;
    return;
  }

  // Guardar estado anterior
  mouse_state.last_buttons = mouse_state.buttons;
  mouse_state.last_x = mouse_state.x;
  mouse_state.last_y = mouse_state.y;

  // Actualizar botones
  mouse_state.buttons = flags & 0x07;

  // Calcular movimiento delta
  int32_t delta_x = packet[1];
  int32_t delta_y = packet[2];

  // Aplicar signo
  if (flags & MOUSE_X_SIGN) {
    delta_x |= 0xFFFFFF00;
  }
  if (flags & MOUSE_Y_SIGN) {
    delta_y |= 0xFFFFFF00;
  }

  // Scroll wheel (4to byte) si está habilitada
  if (mouse_state.has_wheel) {
    // El scroll está en los 4 bits bajos del 4to byte (complemento a 2)
    int8_t scroll_z = packet[3] & 0x0F;
    if (scroll_z & 0x08) {
      scroll_z |= 0xF0; // Extender signo de 4 bits a 8 bits
    }

    if (scroll_z != 0) {
      // Acumular el delta en lugar de sobrescribir para no perder eventos
      // entre frames de la GUI si el mouse se mueve simultáneamente.
      mouse_state.scroll_delta += scroll_z;
    }
  }

  // Limitar delta máximo
  if (delta_x > 100)
    delta_x = 100;
  if (delta_x < -100)
    delta_x = -100;
  if (delta_y > 100)
    delta_y = 100;
  if (delta_y < -100)
    delta_y = -100;

  // Invertir Y
  delta_y = -delta_y;

  // Borrar cursor anterior
  if (mouse_state.cursor_visible) {
    mouse_erase_cursor();
  }

  // Actualizar posición
  mouse_state.x += delta_x;
  mouse_state.y += delta_y;

  // Aplicar límites de pantalla
  if (mouse_state.x < mouse_state.min_x)
    mouse_state.x = mouse_state.min_x;
  if (mouse_state.x > mouse_state.max_x)
    mouse_state.x = mouse_state.max_x;
  if (mouse_state.y < mouse_state.min_y)
    mouse_state.y = mouse_state.min_y;
  if (mouse_state.y > mouse_state.max_y)
    mouse_state.y = mouse_state.max_y;

  // Dibujar cursor en nueva posición
  if (mouse_state.cursor_visible) {
    mouse_draw_cursor();
  }

  mouse_state.packet_ready = false;
}

int8_t mouse_get_scroll(void) {
  int8_t delta = mouse_state.scroll_delta;
  mouse_state.scroll_delta = 0; // Consumir delta
  return delta;
}

void mouse_inject_event(int dx, int dy, uint8_t buttons) {
  if (!mouse_state.enabled)
    return;

  // Guardar estado anterior
  mouse_state.last_buttons = mouse_state.buttons;
  mouse_state.last_x = mouse_state.x;
  mouse_state.last_y = mouse_state.y;

  // Actualizar botones
  mouse_state.buttons = buttons;

  // Limitar delta máximo (similar a process_packet)
  if (dx > 100)
    dx = 100;
  if (dx < -100)
    dx = -100;
  if (dy > 100)
    dy = 100;
  if (dy < -100)
    dy = -100;

  // Borrar cursor anterior
  if (mouse_state.cursor_visible) {
    mouse_erase_cursor();
  }

  // Actualizar posición
  mouse_state.x += dx;
  mouse_state.y += dy;

  // Aplicar límites de pantalla
  if (mouse_state.x < mouse_state.min_x)
    mouse_state.x = mouse_state.min_x;
  if (mouse_state.x > mouse_state.max_x)
    mouse_state.x = mouse_state.max_x;
  if (mouse_state.y < mouse_state.min_y)
    mouse_state.y = mouse_state.min_y;
  if (mouse_state.y > mouse_state.max_y)
    mouse_state.y = mouse_state.max_y;

  // Dibujar cursor en nueva posición
  if (mouse_state.cursor_visible) {
    mouse_draw_cursor();
  }
}

// Dibujar cursor simple
void mouse_draw_cursor(void) {
  if (!mouse_state.cursor_visible)
    return;

  // Guardar área bajo el cursor (8x16 pixels)
  for (uint32_t y = 0; y < 16 && (mouse_state.y + y) < g_fb.height; y++) {
    for (uint32_t x = 0; x < 8 && (mouse_state.x + x) < g_fb.width; x++) {
      uint32_t px = mouse_state.x + x;
      uint32_t py = mouse_state.y + y;

      if (px < g_fb.width && py < g_fb.height) {
        // Obtener pixel actual
        uint32_t offset = py * (g_fb.pitch / 4) + px;
        mouse_state.saved_background[y * 8 + x] = g_fb.buffer32[offset];

        // Dibujar cursor si el bit está activo
        if (terminal_cursor[y] & (0x80 >> x)) {
          g_fb.buffer32[offset] = COLOR_WHITE;
        }
      }
    }
  }
}

// Borrar cursor restaurando el fondo
void mouse_erase_cursor(void) {
  if (!mouse_state.cursor_visible)
    return;

  for (uint32_t y = 0; y < 16 && (mouse_state.last_y + y) < g_fb.height; y++) {
    for (uint32_t x = 0; x < 8 && (mouse_state.last_x + x) < g_fb.width; x++) {
      uint32_t px = mouse_state.last_x + x;
      uint32_t py = mouse_state.last_y + y;

      if (px < g_fb.width && py < g_fb.height) {
        uint32_t offset = py * (g_fb.pitch / 4) + px;
        g_fb.buffer32[offset] = mouse_state.saved_background[y * 8 + x];
      }
    }
  }
}

// Actualizar límites de pantalla
void mouse_update_bounds(uint32_t new_width, uint32_t new_height) {
  mouse_state.screen_width = new_width;
  mouse_state.screen_height = new_height;
  mouse_state.max_x = new_width - 1;
  mouse_state.max_y = new_height - 1;

  // Ajustar posición actual si es necesario
  if (mouse_state.x > mouse_state.max_x)
    mouse_state.x = mouse_state.max_x;
  if (mouse_state.y > mouse_state.max_y)
    mouse_state.y = mouse_state.max_y;
}

void mouse_set_position(int32_t x, int32_t y) {
  if (mouse_state.cursor_visible) {
    mouse_erase_cursor();
  }

  mouse_state.last_x = mouse_state.x;
  mouse_state.last_y = mouse_state.y;
  mouse_state.x = x;
  mouse_state.y = y;

  // Aplicar límites
  if (mouse_state.x < mouse_state.min_x)
    mouse_state.x = mouse_state.min_x;
  if (mouse_state.x > mouse_state.max_x)
    mouse_state.x = mouse_state.max_x;
  if (mouse_state.y < mouse_state.min_y)
    mouse_state.y = mouse_state.min_y;
  if (mouse_state.y > mouse_state.max_y)
    mouse_state.y = mouse_state.max_y;

  if (mouse_state.cursor_visible) {
    mouse_draw_cursor();
  }
}

// Obtener posición del mouse
void mouse_get_position(int32_t *x, int32_t *y) {
  if (x)
    *x = mouse_state.x;
  if (y)
    *y = mouse_state.y;
}

// Obtener estado de botones
uint8_t mouse_get_buttons(void) { return mouse_state.buttons; }

// Verificar si el mouse se movió
bool mouse_is_moved(void) {
  return (mouse_state.x != mouse_state.last_x ||
          mouse_state.y != mouse_state.last_y);
}

// Verificar si se hizo click
bool mouse_is_clicked(uint8_t button) {
  return ((mouse_state.last_buttons & button) &&
          !(mouse_state.buttons & button));
}

// Verificar si está presionado
bool mouse_is_pressed(uint8_t button) { return (mouse_state.buttons & button); }

// Verificar si se liberó
bool mouse_is_released(uint8_t button) {
  return (!(mouse_state.buttons & button) &&
          (mouse_state.last_buttons & button));
}

// Establecer visibilidad del cursor
void mouse_set_cursor_visible(bool visible) {
  if (visible && !mouse_state.cursor_visible) {
    mouse_state.cursor_visible = true;
    mouse_draw_cursor();
  } else if (!visible && mouse_state.cursor_visible) {
    mouse_erase_cursor();
    mouse_state.cursor_visible = false;
  }
}

// Obtener visibilidad del cursor
bool mouse_get_cursor_visible(void) { return mouse_state.cursor_visible; }

// ========================================================================
// DRIVER SYSTEM INTEGRATION
// ========================================================================
#include "driver_system.h"
#include "terminal.h"

extern Terminal main_terminal;

static int mouse_driver_init(driver_instance_t *drv, void *config) {
  (void)config;
  if (!drv)
    return -1;

  // El mouse se inicializa con dimensiones por defecto si no se especifican
  // Idealmente se pasaría el FB info en config
  mouse_init(640, 480); // Default, se puede actualizar vía IOCTL

  return 0;
}

static int mouse_driver_start(driver_instance_t *drv) {
  if (!drv)
    return -1;
  terminal_printf(&main_terminal, "Mouse driver: Started\r\n");
  return 0;
}

static int mouse_driver_stop(driver_instance_t *drv) {
  if (!drv)
    return -1;
  mouse_state.enabled = false;
  return 0;
}

static int mouse_driver_cleanup(driver_instance_t *drv) {
  if (!drv)
    return -1;
  return 0;
}

static int mouse_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
  if (!drv)
    return -1;

  switch (cmd) {
  case 0x1001: { // Update bounds
    uint32_t *bounds = (uint32_t *)arg;
    if (!bounds)
      return -1;
    mouse_update_bounds(bounds[0], bounds[1]);
    return 0;
  }
  case 0x1002: { // Get state
    mouse_state_t **state_ptr = (mouse_state_t **)arg;
    if (!state_ptr)
      return -1;
    *state_ptr = &mouse_state;
    return 0;
  }
  default:
    return -1;
  }
}

static driver_ops_t mouse_driver_ops = {.init = mouse_driver_init,
                                        .start = mouse_driver_start,
                                        .stop = mouse_driver_stop,
                                        .cleanup = mouse_driver_cleanup,
                                        .ioctl = mouse_driver_ioctl,
                                        .load_data = NULL};

static driver_type_info_t mouse_driver_type = {
    .type = DRIVER_TYPE_MOUSE,
    .type_name = "mouse",
    .version = "1.0.0",
    .priv_data_size = 0, // El estado es global estático en este archivo
    .default_ops = &mouse_driver_ops,
    .validate_data = NULL,
    .print_info = NULL};

int mouse_driver_register_type(void) {
  return driver_register_type(&mouse_driver_type);
}

driver_instance_t *mouse_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_MOUSE, name);
}