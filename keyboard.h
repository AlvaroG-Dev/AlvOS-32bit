#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

// Definición del layout de teclado
typedef struct keyboard_layout {
    char name[32];
    uint8_t normal[128];
    uint8_t shift[128];
    uint8_t altgr[128];
} keyboard_layout_t;

typedef void (*EditorKeyHandler)(int key, void* context);

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define RELEASE_MASK 0x80

// Códigos de teclas especiales
#define KEY_UP     (-1)
#define KEY_DOWN   (-2)
#define KEY_LEFT   (-3)
#define KEY_RIGHT  (-4)
#define KEY_HOME   (-5)
#define KEY_END    (-6)
#define KEY_PGUP   (-7)
#define KEY_PGDOWN (-8)
#define KEY_INSERT (-9)
#define KEY_DELETE (-10)

// Teclas de función
#define KEY_F1     0x3B
#define KEY_F2     0x3C
#define KEY_F3     0x3D
#define KEY_F4     0x3E
#define KEY_F5     0x3F
#define KEY_F6     0x40
#define KEY_F7     0x41
#define KEY_F8     0x42
#define KEY_F9     0x43
#define KEY_F10    0x44
#define KEY_F11    0x57
#define KEY_F12    0x58

// Teclas modificadoras
#define LEFT_SHIFT  0x2A
#define RIGHT_SHIFT 0x36
#define LEFT_CTRL   0x1D
#define LEFT_ALT    0x38
#define CAPS_LOCK   0x3A
#define RIGHT_ALT   0xE038  // Extended scancode for Right Alt (AltGr)

typedef struct {
    uint8_t left_shift;
    uint8_t right_shift;
    uint8_t ctrl;
    uint8_t alt;
    uint8_t altgr;  // Nuevo: Para Right Alt (AltGr)
    uint8_t caps_lock;
    uint8_t last_key_processed;
} KeyboardState;

void keyboard_init();
uint8_t keyboard_read_scancode();
int keyboard_process_scancode(uint8_t scancode, KeyboardState* state, uint8_t* extended_flag);
uint8_t keyboard_get_modifiers(KeyboardState* state);
void keyboard_irq_handler(void);
typedef void (*KeyboardCallback)(int key);
void keyboard_set_handler(KeyboardCallback handler);
int keyboard_get_char(void);
int keyboard_wait_char(void);

// Nuevas funciones para integración con el sistema de drivers
keyboard_layout_t *keyboard_get_current_layout(void);
int keyboard_set_layout(const char *layout_name);
int keyboard_load_layout(const char *filename, const char *layout_name);

#endif