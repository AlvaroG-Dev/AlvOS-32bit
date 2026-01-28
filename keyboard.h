#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "driver_system.h"
#include <stddef.h>
#include <stdint.h>

// Definición del layout de teclado
#define KEYBOARD_LAYOUT_MAGIC 0x4B4244 // 'KBD'
#define KEYBOARD_MAX_LAYOUTS 8

typedef struct keyboard_layout {
  char name[32];
  uint8_t normal[128];
  uint8_t shift[128];
  uint8_t altgr[128];
} keyboard_layout_t;

// Keyboard driver private data
typedef struct keyboard_driver_data {
  keyboard_layout_t *current_layout;
  keyboard_layout_t *default_layout;
  keyboard_layout_t **available_layouts;
  uint32_t layout_count;
  uint8_t max_layouts;
} keyboard_driver_data_t;

// Keyboard driver IOCTL commands
#define KBD_IOCTL_SET_LAYOUT 0x1001
#define KBD_IOCTL_GET_LAYOUT 0x1002
#define KBD_IOCTL_LIST_LAYOUTS 0x1003
#define KBD_IOCTL_LOAD_LAYOUT 0x1004

// IOCTL argument structures
typedef struct {
  char layout_name[32];
} kbd_ioctl_set_layout_t;

typedef struct {
  char layout_name[32];
  char filename[256];
} kbd_ioctl_load_layout_t;

typedef struct {
  uint32_t max_layouts;
  uint32_t layout_count;
  char layout_names[][32]; // Variable length array
} kbd_ioctl_list_layouts_t;

typedef void (*EditorKeyHandler)(int key, void *context);

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define RELEASE_MASK 0x80

// Códigos de teclas especiales (mapeados a negativos para distinguirlos)
#define KEY_UP (-1)
#define KEY_DOWN (-2)
#define KEY_LEFT (-3)
#define KEY_RIGHT (-4)
#define KEY_HOME (-5)
#define KEY_END (-6)
#define KEY_PGUP (-7)
#define KEY_PGDOWN (-8)
#define KEY_INSERT (-9)
#define KEY_DELETE (-10)

// Teclas de función
#define KEY_F1 0x3B
#define KEY_F2 0x3C
#define KEY_F3 0x3D
#define KEY_F4 0x3E
#define KEY_F5 0x3F
#define KEY_F6 0x40
#define KEY_F7 0x41
#define KEY_F8 0x42
#define KEY_F9 0x43
#define KEY_F10 0x44
#define KEY_F11 0x57
#define KEY_F12 0x58

// Teclas modificadoras
#define LEFT_SHIFT 0x2A
#define RIGHT_SHIFT 0x36
#define LEFT_CTRL 0x1D
#define LEFT_ALT 0x38
#define CAPS_LOCK 0x3A
#define RIGHT_ALT 0xE038 // Extended scancode for Right Alt (AltGr)

typedef struct {
  uint8_t left_shift;
  uint8_t right_shift;
  uint8_t ctrl;
  uint8_t alt;
  uint8_t altgr;
  uint8_t caps_lock;
  uint8_t last_key_processed;
} KeyboardState;

// High-level API
void keyboard_init();
uint8_t keyboard_read_scancode();
int keyboard_process_scancode(uint8_t scancode, KeyboardState *state,
                              uint8_t *extended_flag);
uint8_t keyboard_get_modifiers(KeyboardState *state);
void keyboard_irq_handler(void);
typedef void (*KeyboardCallback)(int key);
void keyboard_set_handler(KeyboardCallback handler);
int keyboard_get_char(void);
int keyboard_wait_char(void);
void keyboard_inject_scancode(uint8_t scancode);

// Layout management functions
keyboard_layout_t *keyboard_get_current_layout(void);
int keyboard_set_layout(const char *layout_name);
int keyboard_load_layout(const char *filename, const char *layout_name);

int keyboard_available(void);
int keyboard_getkey_nonblock(void);
void keyboard_clear_buffer(void);

// Driver registration functions (previously in keyboard_driver.h)
int keyboard_driver_register_type(void);
driver_instance_t *keyboard_driver_create(const char *name);
int keyboard_driver_set_layout(driver_instance_t *drv, const char *layout_name);
keyboard_layout_t *keyboard_driver_get_current_layout(driver_instance_t *drv);
int keyboard_load_layout_from_data(driver_instance_t *drv,
                                   const void *file_data, size_t file_size);
char keyboard_driver_map_scancode(const keyboard_layout_t *layout,
                                  uint8_t scancode, uint8_t shift,
                                  uint8_t altgr, uint8_t caps_lock);

#endif