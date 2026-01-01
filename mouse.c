// mouse.c - Versión standalone sin Window Manager
#include "mouse.h"
#include "drawing.h"
#include "io.h"
#include "irq.h"
#include "kernel.h"
#include "string.h"

static mouse_state_t mouse_state = {0};
static bool mouse_initialized = false;

// Cursor simple para modo terminal
static const uint8_t terminal_cursor[16] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
                                            0xFE, 0xFF, 0xF8, 0xD8, 0x8C, 0x0C,
                                            0x06, 0x06, 0x03, 0x00};

// Esperar al estado del puerto PS/2
static void mouse_wait(bool is_input) {
  uint32_t timeout = 100000;
  if (is_input) {
    while (timeout--) {
      if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
        return;
      }
    }
  } else {
    while (timeout--) {
      if ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
        return;
      }
    }
  }
}

// Escribir al mouse
static void mouse_write(uint8_t value) {
  mouse_wait(false);
  outb(PS2_COMMAND_PORT, 0xD4);
  mouse_wait(false);
  outb(PS2_DATA_PORT, value);
}

// Leer del mouse
static uint8_t mouse_read(void) {
  mouse_wait(true);
  return inb(PS2_DATA_PORT);
}

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

  // Habilitar el aux device (mouse)
  mouse_wait(false);
  outb(PS2_COMMAND_PORT, 0xA8);

  // Leer configuration byte
  mouse_wait(false);
  outb(PS2_COMMAND_PORT, 0x20);
  mouse_wait(true);
  status = inb(PS2_DATA_PORT);

  // Habilitar IRQ12 (mouse) y mantener IRQ1 (teclado) habilitado
  status |= 0x02; // IRQ12
  status |= 0x01; // IRQ1 (teclado)

  // Escribir configuration byte de vuelta
  mouse_wait(false);
  outb(PS2_COMMAND_PORT, 0x60);
  mouse_wait(false);
  outb(PS2_DATA_PORT, status);

  // Resetear el mouse
  mouse_write(MOUSE_CMD_RESET);
  uint8_t ack = mouse_read();
  if (ack != 0xFA) {
    return false;
  }

  // Leer BAT result
  uint8_t bat_result = mouse_read();
  if (bat_result != 0xAA) {
    return false;
  }

  // Leer device ID
  mouse_read(); // device_id no usado en modo standalone

  // Configurar defaults
  mouse_write(MOUSE_CMD_SET_DEFAULTS);
  mouse_read();

  // Habilitar data reporting
  mouse_write(MOUSE_CMD_ENABLE_DATA_REP);
  mouse_read();

  // Configurar sample rate y resolución
  mouse_set_sample_rate(MOUSE_SAMPLE_RATE);
  mouse_set_resolution(MOUSE_RESOLUTION);

  return true;
}

// Inicializar el mouse
void mouse_init(uint32_t screen_width, uint32_t screen_height) {
  if (mouse_initialized) return;

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

// Manejador de IRQ del mouse
void mouse_handle_irq(void) {
  if (!mouse_state.enabled)
    return;

  uint8_t status = inb(PS2_STATUS_PORT);
  if (!(status & PS2_STATUS_OUTPUT_FULL))
    return;

  uint8_t data = inb(PS2_DATA_PORT);

  // Manejar paquetes de 3 bytes (mouse estándar)
  if (mouse_state.packet_index == 0 && (data & 0x08)) {
    mouse_state.packet[0] = data;
    mouse_state.packet_index = 1;
  } else if (mouse_state.packet_index == 1) {
    mouse_state.packet[1] = data;
    mouse_state.packet_index = 2;
  } else if (mouse_state.packet_index == 2) {
    mouse_state.packet[2] = data;
    mouse_state.packet_index = 0;
    mouse_state.packet_ready = true;
    mouse_process_packet();
  } else {
    mouse_state.packet_index = 0;
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