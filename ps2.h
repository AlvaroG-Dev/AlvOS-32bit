#ifndef PS2_H
#define PS2_H

#include <stdint.h>
#include <stdbool.h>

// Puertos del controlador PS/2
#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_COMMAND_PORT    0x64

// Bits del registro de estado
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_SYSTEM      0x04
#define PS2_STATUS_CMD_DATA    0x08
#define PS2_STATUS_KEYLOCK     0x10
#define PS2_STATUS_AUX_OUTPUT  0x20
#define PS2_STATUS_TIMEOUT     0x40
#define PS2_STATUS_PARITY      0x80

// Comandos del controlador PS/2
#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_SCAN2  0xA7
#define PS2_CMD_ENABLE_SCAN2   0xA8
#define PS2_CMD_TEST_SCAN2     0xA9
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_TEST_SCAN1     0xAB
#define PS2_CMD_DISABLE_SCAN1  0xAD
#define PS2_CMD_ENABLE_SCAN1   0xAE
#define PS2_CMD_WRITE_AUX      0xD4

// Inicialización y utilidades
void ps2_init(void);
uint8_t ps2_read_status(void);
void ps2_wait_read(void);
void ps2_wait_write(void);
uint8_t ps2_read_data(void);
void ps2_write_data(uint8_t data);
void ps2_write_command(uint8_t cmd);
void ps2_write_aux(uint8_t data);

// Funciones para drivers
bool ps2_is_mouse_data(uint8_t status);
bool ps2_is_keyboard_data(uint8_t status);

#endif
