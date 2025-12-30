#include "keyboard.h"
#include "driver_system.h"
#include "drivers/keyboard_driver.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "kernel.h"
#include "vfs.h"

// Forward declaration for irq_install_handler
void irq_install_handler(uint8_t irq, void (*handler)(void));

static KeyboardState kbd_state = {0};
static KeyboardCallback keyboard_callback = NULL;
#define KEYBOARD_BUFFER_SIZE 128
static int keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static uint32_t keyboard_buffer_head = 0;
static uint32_t keyboard_buffer_tail = 0;
static uint32_t keyboard_buffer_count = 0;

// Instancia global del driver de teclado
static driver_instance_t *keyboard_driver_instance = NULL;

// Función auxiliar para obtener el driver de teclado
static driver_instance_t *get_keyboard_driver(void) {
  if (!keyboard_driver_instance) {
    keyboard_driver_instance = driver_find_by_type(DRIVER_TYPE_KEYBOARD);
    if (!keyboard_driver_instance) {
      // Crear instancia si no existe
      keyboard_driver_instance = keyboard_driver_create("system-keyboard");
      if (keyboard_driver_instance) {
        driver_init(keyboard_driver_instance, NULL);
        driver_start(keyboard_driver_instance);
      }
    }
  }
  return keyboard_driver_instance;
}

unsigned char keyboard_map_scancode(uint8_t scancode, uint8_t shift,
                                    uint8_t altgr) {
  keyboard_layout_t *layout = keyboard_get_current_layout();
  if (!layout || scancode >= 128) {
    return 0;
  }

  if (altgr && layout->altgr[scancode] != 0) {
    return layout->altgr[scancode];
  } else if (shift) {
    return layout->shift[scancode];
  } else {
    return layout->normal[scancode];
  }
}

// Mapa de teclas extendidas (0xE0 prefijo)
static const int extended_keys[] = {KEY_UP,     KEY_DOWN,  KEY_LEFT, KEY_RIGHT,
                                    KEY_HOME,   KEY_END,   KEY_PGUP, KEY_PGDOWN,
                                    KEY_INSERT, KEY_DELETE};

// Verifica si un scancode es de tecla extendida
static int is_extended_key(uint8_t scancode) {
  for (uint32_t i = 0; i < sizeof(extended_keys) / sizeof(extended_keys[0]);
       i++) {
    if (extended_keys[i] == scancode) {
      return 1;
    }
  }
  return 0;
}

void keyboard_init() {}

uint8_t keyboard_read_scancode() {
  while (!(inb(KEYBOARD_STATUS_PORT) & 0x01))
    ;
  return inb(KEYBOARD_DATA_PORT);
}

int keyboard_process_scancode(uint8_t scancode, KeyboardState *state,
                              uint8_t *extended_flag) {
  uint8_t key_code = scancode & ~RELEASE_MASK;
  uint8_t is_release = scancode & RELEASE_MASK;

  // DEBUG: Print scancode for verification (remove after testing)
  // terminal_printf(&main_terminal, "Scancode: 0x%02x (extended=%d)\r\n",
  // scancode, *extended_flag);

  // Handle extended prefix
  if (scancode == 0xE0) {
    *extended_flag = 1;
    return 0;
  }

  // Handle AltGr (RIGHT_ALT) if extended
  if (*extended_flag && key_code == 0x38) { // RIGHT_ALT (0xE038)
    state->altgr = !is_release;
    *extended_flag = 0;
    return 0;
  }

  // Handle modifiers
  switch (key_code) {
  case LEFT_SHIFT:
    state->left_shift = !is_release;
    return 0;
  case RIGHT_SHIFT:
    state->right_shift = !is_release;
    return 0;
  case LEFT_CTRL:
    state->ctrl = !is_release;
    return 0;
  case LEFT_ALT:
    state->alt = !is_release;
    state->altgr = !is_release; // Fallback for non-extended AltGr
    return 0;
  case CAPS_LOCK:
    if (!is_release)
      state->caps_lock = !state->caps_lock;
    return 0;
  default:
    break;
  }

  // Ignore release for non-modifiers
  if (is_release) {
    *extended_flag = 0;
    return 0;
  }

  // Handle extended keys
  if (*extended_flag) {
    *extended_flag = 0;
    switch (key_code) {
    case 0x48:
      return KEY_UP; // Up arrow
    case 0x50:
      return KEY_DOWN; // Down arrow
    case 0x4B:
      return KEY_LEFT; // Left arrow
    case 0x4D:
      return KEY_RIGHT; // Right arrow
    case 0x47:
      return KEY_HOME;
    case 0x4F:
      return KEY_END;
    case 0x49:
      return KEY_PGUP;
    case 0x51:
      return KEY_PGDOWN;
    case 0x52:
      return KEY_INSERT;
    case 0x53:
      return KEY_DELETE;
    default:
      return 0;
    }
  }

  // Normal key mapping
  uint8_t shift_active = state->left_shift || state->right_shift;
  unsigned char c = keyboard_map_scancode(key_code, shift_active, state->altgr);

  // Caps Lock for letters and ñ
  if (state->caps_lock && !state->altgr &&
      ((c >= 'a' && c <= 'z') || c == 0xF1)) {
    c = keyboard_map_scancode(key_code, 1, 0);
  }

  // Ctrl combinations
  if (state->ctrl && c >= 'a' && c <= 'z') {
    return c - 'a' + 1; // Ctrl+A = 1, etc.
  }

  return (int)c; // Return as int for printable chars
}

uint8_t keyboard_get_modifiers(KeyboardState *state) {
  return (state->left_shift << 0) | (state->right_shift << 1) |
         (state->ctrl << 2) | (state->caps_lock << 3) | (state->altgr << 4);
}

void keyboard_irq_handler() {
  static uint8_t extended = 0;
  uint8_t scancode = inb(0x60);

  if (scancode == 0xE0) {
    extended = 1;
    pic_send_eoi(1);
    return;
  }

  int key = keyboard_process_scancode(scancode, &kbd_state, &extended);

  // Buffer only printable chars (positive, <128)
  if (key > 0 && key < 128 && keyboard_buffer_count < KEYBOARD_BUFFER_SIZE) {
    keyboard_buffer[keyboard_buffer_tail] = key;
    keyboard_buffer_tail = (keyboard_buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
    keyboard_buffer_count++;
  }

  // Call callback with key (including specials)
  if (keyboard_callback && key != 0) {
    keyboard_callback(key);
  }

  pic_send_eoi(1);
}

void keyboard_set_handler(KeyboardCallback handler) {
  keyboard_callback = handler;
}

void keyboard_read_line(char *buffer, uint32_t max_length) {
  KeyboardState state = {0};
  uint32_t index = 0;
  buffer[0] = '\0';

  while (1) {
    uint8_t scancode = keyboard_read_scancode();
    int key = keyboard_process_scancode(scancode, &state, &(uint8_t){0});

    if (key == '\n') {
      buffer[index] = '\0';
      return;
    } else if (key == '\b') {
      if (index > 0) {
        index--;
        buffer[index] = '\0';
      }
    } else if (key > 0 && key < 128 && index < max_length - 1) {
      buffer[index++] = (char)key;
      buffer[index] = '\0';
    }
  }
}

keyboard_layout_t *keyboard_get_current_layout(void) {
  driver_instance_t *drv = get_keyboard_driver();
  if (!drv) {
    return NULL;
  }
  return keyboard_driver_get_current_layout(drv);
}

int keyboard_set_layout(const char *layout_name) {
  driver_instance_t *drv = get_keyboard_driver();
  if (!drv) {
    return -1;
  }
  return keyboard_driver_set_layout(drv, layout_name);
}

int keyboard_load_layout(const char *filename, const char *layout_name) {
  driver_instance_t *drv = get_keyboard_driver();
  if (!drv) {
    return -1;
  }

  // Cargar el archivo usando el sistema de drivers
  size_t file_size;
  void *file_data = driver_load_binary_file(filename, &file_size);
  if (!file_data) {
    return -1;
  }

  // Cargar el layout desde los datos
  int result = keyboard_load_layout_from_data(drv, file_data, file_size);

  // Liberar los datos del archivo
  driver_unload_binary_file(file_data, file_size);

  if (result == 0 && layout_name) {
    // Cambiar al nuevo layout si se especificó un nombre
    result = keyboard_driver_set_layout(drv, layout_name);
  }

  return result;
}

// Verificar si hay teclas disponibles
int keyboard_available(void) { return keyboard_buffer_count > 0; }

// Leer tecla sin bloquear (retorna -1 si no hay teclas)
int keyboard_getkey_nonblock(void) {
  if (keyboard_buffer_count == 0) {
    return -1;
  }

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  int key = keyboard_buffer[keyboard_buffer_head];
  keyboard_buffer_head = (keyboard_buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
  keyboard_buffer_count--;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));

  return key;
}

// Limpiar buffer del teclado
void keyboard_clear_buffer(void) {
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  keyboard_buffer_head = 0;
  keyboard_buffer_tail = 0;
  keyboard_buffer_count = 0;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}