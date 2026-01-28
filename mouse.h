#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <stdint.h>

// Comandos del mouse PS/2
#define MOUSE_CMD_RESET 0xFF
#define MOUSE_CMD_RESEND 0xFE
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_DISABLE_DATA_REP 0xF5
#define MOUSE_CMD_ENABLE_DATA_REP 0xF4
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_GET_DEVICE_ID 0xF2
#define MOUSE_CMD_SET_REMOTE_MODE 0xF0
#define MOUSE_CMD_SET_WRAP_MODE 0xEE
#define MOUSE_CMD_RESET_WRAP_MODE 0xEC
#define MOUSE_CMD_READ_DATA 0xEB
#define MOUSE_CMD_SET_STREAM_MODE 0xEA
#define MOUSE_CMD_STATUS_REQUEST 0xE9
#define MOUSE_CMD_SET_RESOLUTION 0xE8
#define MOUSE_CMD_SET_SCALING_2_1 0xE7
#define MOUSE_CMD_SET_SCALING_1_1 0xE6

// Estados del mouse
#define MOUSE_LEFT_BUTTON 0x01
#define MOUSE_RIGHT_BUTTON 0x02
#define MOUSE_MIDDLE_BUTTON 0x04
#define MOUSE_X_SIGN 0x10
#define MOUSE_Y_SIGN 0x20
#define MOUSE_X_OVERFLOW 0x40
#define MOUSE_Y_OVERFLOW 0x80

// Puerto PS/2
#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

// Estados del puerto PS/2
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_STATUS_SYSTEM_FLAG 0x04
#define PS2_STATUS_COMMAND_DATA 0x08
#define PS2_STATUS_TIMEOUT_ERR 0x40
#define PS2_STATUS_PARITY_ERR 0x80

// Configuración del mouse
#define MOUSE_SAMPLE_RATE 100 // 100 muestras/segundo
#define MOUSE_RESOLUTION 3    // 8 counts/mm
#define MOUSE_SCALING 1       // 1:1 scaling

// Estructura del estado del mouse
typedef struct {
  int32_t x, y;           // Posición actual
  int32_t last_x, last_y; // Posición anterior
  uint8_t buttons; // Estado de botones (bit 0: izquierdo, bit 1: derecho, bit
                   // 2: medio)
  uint8_t last_buttons; // Estado anterior de botones
  bool enabled;         // Mouse habilitado
  bool packet_ready;    // Paquete listo para procesar
  uint8_t packet[4];    // Buffer del paquete
  uint8_t packet_index; // Índice del paquete actual
  int32_t min_x, max_x; // Límites de pantalla
  int32_t min_y, max_y;
  uint32_t screen_width;
  uint32_t screen_height;
  bool cursor_visible; // Visibilidad del cursor
  uint32_t
      saved_background[16 * 8]; // Buffer para guardar fondo bajo cursor (8x16)
} mouse_state_t;

// Funciones públicas
void mouse_init(uint32_t screen_width, uint32_t screen_height);
void mouse_handle_irq(void);
void mouse_update_bounds(uint32_t new_width, uint32_t new_height);
void mouse_set_position(int32_t x, int32_t y);
void mouse_get_position(int32_t *x, int32_t *y);
uint8_t mouse_get_buttons(void);
bool mouse_is_moved(void);
bool mouse_is_clicked(uint8_t button);
bool mouse_is_pressed(uint8_t button);
bool mouse_is_released(uint8_t button);
void mouse_set_cursor_visible(bool visible);
bool mouse_get_cursor_visible(void);
void mouse_inject_event(int dx, int dy, uint8_t buttons);
void mouse_draw_cursor(void);
void mouse_erase_cursor(void);
void mouse_process_packet(void);

// Driver registration functions
int mouse_driver_register_type(void);
struct driver_instance *mouse_driver_create(const char *name);

#endif // MOUSE_H