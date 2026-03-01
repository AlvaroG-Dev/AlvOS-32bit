#include "ps2.h"
#include "io.h"
#include "log.h"

void ps2_wait_read(void) {
  uint32_t timeout = 1000000;
  while (timeout--) {
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
      return;
    }
  }
}

void ps2_wait_write(void) {
  uint32_t timeout = 1000000;
  while (timeout--) {
    if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
      return;
    }
  }
}

uint8_t ps2_read_status(void) { return inb(PS2_STATUS_PORT); }

uint8_t ps2_read_data(void) {
  ps2_wait_read();
  return inb(PS2_DATA_PORT);
}

void ps2_write_data(uint8_t data) {
  ps2_wait_write();
  outb(PS2_DATA_PORT, data);
}

void ps2_write_command(uint8_t cmd) {
  ps2_wait_write();
  outb(PS2_COMMAND_PORT, cmd);
}

void ps2_write_aux(uint8_t data) {
  ps2_write_command(PS2_CMD_WRITE_AUX);
  ps2_write_data(data);
}

bool ps2_is_mouse_data(uint8_t status) {
  return (status & PS2_STATUS_AUX_OUTPUT) != 0;
}

bool ps2_is_keyboard_data(uint8_t status) {
  return (status & PS2_STATUS_AUX_OUTPUT) == 0;
}

void ps2_init(void) {
  // Deshabilitar dispositivos durante la inicialización
  ps2_write_command(PS2_CMD_DISABLE_SCAN1);
  ps2_write_command(PS2_CMD_DISABLE_SCAN2);

  // Limpiar buffer de salida
  while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
    inb(PS2_DATA_PORT);
  }

  // Leer el configuration byte
  ps2_write_command(PS2_CMD_READ_CONFIG);
  uint8_t config = ps2_read_data();

  // Habilitar IRQ1 e IRQ12, habilitar traductores si es necesario
  config |= 0x03;  // Habilitar interrupciones para ambos puertos
  config &= ~0x30; // Habilitar clocks para ambos puertos

  ps2_write_command(PS2_CMD_WRITE_CONFIG);
  ps2_write_data(config);

  // Habilitar dispositivos
  ps2_write_command(PS2_CMD_ENABLE_SCAN1);
  ps2_write_command(PS2_CMD_ENABLE_SCAN2);

  log_message(LOG_INFO, "Controlador PS/2 inicializado correctamente");
}
