#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "kernel.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "vfs.h"

extern Terminal main_terminal;

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

void keyboard_init() {

  // Asegurar que el driver está cargado y activo
  get_keyboard_driver();
}

uint8_t keyboard_read_scancode() {
  while (!(inb(KEYBOARD_STATUS_PORT) & 0x01))
    ;
  return inb(KEYBOARD_DATA_PORT);
}

int keyboard_process_scancode(uint8_t scancode, KeyboardState *state,
                              uint8_t *extended_flag) {
  uint8_t key_code = scancode & ~RELEASE_MASK;
  uint8_t is_release = scancode & RELEASE_MASK;

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

void keyboard_inject_scancode(uint8_t scancode) {
  static uint8_t extended = 0;

  if (scancode == 0xE0) {
    extended = 1;
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
}

void keyboard_irq_handler() {
  uint8_t scancode = inb(0x60);
  keyboard_inject_scancode(scancode);
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

int keyboard_available(void) { return keyboard_buffer_count > 0; }

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

void keyboard_clear_buffer(void) {
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  keyboard_buffer_head = 0;
  keyboard_buffer_tail = 0;
  keyboard_buffer_count = 0;

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}

// ========================================================================
// DRIVER IMPLEMENTATION (Previously in drivers/keyboard_driver.c)
// ========================================================================

// US QWERTY layout por defecto (scancodes PS/2 Set 1)
static const uint8_t us_qwerty_normal[128] = {
    [0x00] = 0,          [0x01] = 27,         [0x02] = '1',
    [0x03] = '2',        [0x04] = '3',        [0x05] = '4',
    [0x06] = '5',        [0x07] = '6',        [0x08] = '7',
    [0x09] = '8',        [0x0A] = '9',        [0x0B] = '0',
    [0x0C] = '-',        [0x0D] = '=',        [0x0E] = '\b',
    [0x0F] = '\t',       [0x10] = 'q',        [0x11] = 'w',
    [0x12] = 'e',        [0x13] = 'r',        [0x14] = 't',
    [0x15] = 'y',        [0x16] = 'u',        [0x17] = 'i',
    [0x18] = 'o',        [0x19] = 'p',        [0x1A] = '[',
    [0x1B] = ']',        [0x1C] = '\n',       [0x1D] = 0,
    [0x1E] = 'a',        [0x1F] = 's',        [0x20] = 'd',
    [0x21] = 'f',        [0x22] = 'g',        [0x23] = 'h',
    [0x24] = 'j',        [0x25] = 'k',        [0x26] = 'l',
    [0x27] = ';',        [0x28] = '\'',       [0x29] = '`',
    [0x2A] = 0,          [0x2B] = '\\',       [0x2C] = 'z',
    [0x2D] = 'x',        [0x2E] = 'c',        [0x2F] = 'v',
    [0x30] = 'b',        [0x31] = 'n',        [0x32] = 'm',
    [0x33] = ',',        [0x34] = '.',        [0x35] = '/',
    [0x36] = 0,          [0x37] = '*',        [0x38] = 0,
    [0x39] = ' ',        [0x3A] = 0,          [0x3B] = 0,
    [0x3C] = 0,          [0x3D] = 0,          [0x3E] = 0,
    [0x3F] = 0,          [0x40] = 0,          [0x41] = 0,
    [0x42] = 0,          [0x43] = 0,          [0x44] = 0,
    [0x45] = 0,          [0x46] = 0,          [0x47] = KEY_HOME,
    [0x48] = KEY_UP,     [0x49] = KEY_PGUP,   [0x4A] = '-',
    [0x4B] = KEY_LEFT,   [0x4C] = 0,          [0x4D] = KEY_RIGHT,
    [0x4E] = 0,          [0x4F] = KEY_END,    [0x50] = KEY_DOWN,
    [0x51] = KEY_PGDOWN, [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = 0};

static const uint8_t us_qwerty_shift[128] = {
    [0x00] = 0,          [0x01] = 27,         [0x02] = '!',
    [0x03] = '@',        [0x04] = '#',        [0x05] = '$',
    [0x06] = '%',        [0x07] = '^',        [0x08] = '&',
    [0x09] = '*',        [0x0A] = '(',        [0x0B] = ')',
    [0x0C] = '_',        [0x0D] = '+',        [0x0E] = '\b',
    [0x0F] = '\t',       [0x10] = 'Q',        [0x11] = 'W',
    [0x12] = 'E',        [0x13] = 'R',        [0x14] = 'T',
    [0x15] = 'Y',        [0x16] = 'U',        [0x17] = 'I',
    [0x18] = 'O',        [0x19] = 'P',        [0x1A] = '{',
    [0x1B] = '}',        [0x1C] = '\n',       [0x1D] = 0,
    [0x1E] = 'A',        [0x1F] = 'S',        [0x20] = 'D',
    [0x21] = 'F',        [0x22] = 'G',        [0x23] = 'H',
    [0x24] = 'J',        [0x25] = 'K',        [0x26] = 'L',
    [0x27] = ':',        [0x28] = '\"',       [0x29] = '~',
    [0x2A] = 0,          [0x2B] = '|',        [0x2C] = 'Z',
    [0x2D] = 'X',        [0x2E] = 'C',        [0x2F] = 'V',
    [0x30] = 'B',        [0x31] = 'N',        [0x32] = 'M',
    [0x33] = '<',        [0x34] = '>',        [0x35] = '?',
    [0x36] = 0,          [0x37] = '*',        [0x38] = 0,
    [0x39] = ' ',        [0x3A] = 0,          [0x3B] = 0,
    [0x48] = KEY_UP,     [0x50] = KEY_DOWN,   [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,  [0x47] = KEY_HOME,   [0x4F] = KEY_END,
    [0x49] = KEY_PGUP,   [0x51] = KEY_PGDOWN, [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE, [0x54 ... 0x7F] = 0};

static const uint8_t us_qwerty_altgr[128] = {
    [0x02] = 0,          [0x03] = 0,          [0x04] = 0,
    [0x05] = 0,          [0x06] = 0,          [0x07] = 0,
    [0x08] = 0,          [0x09] = 0,          [0x0A] = 0,
    [0x0B] = 0,          [0x0C] = 0,          [0x0D] = 0,
    [0x10] = 0,          [0x11] = 0,          [0x12] = 0x80,
    [0x13] = 0,          [0x18] = 0,          [0x19] = 0,
    [0x1A] = 0,          [0x1B] = 0,          [0x1E] = 0,
    [0x1F] = 0,          [0x20] = 0,          [0x21] = 0,
    [0x27] = 0,          [0x28] = 0,          [0x29] = 0,
    [0x2B] = 0,          [0x48] = KEY_UP,     [0x50] = KEY_DOWN,
    [0x4B] = KEY_LEFT,   [0x4D] = KEY_RIGHT,  [0x47] = KEY_HOME,
    [0x4F] = KEY_END,    [0x49] = KEY_PGUP,   [0x51] = KEY_PGDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE, [0x54 ... 0x7F] = 0};

static keyboard_layout_t default_layout = {
    .name = "US-QWERTY", .normal = {0}, .shift = {0}, .altgr = {0}};

static int keyboard_driver_init(driver_instance_t *drv, void *config);
static int keyboard_driver_start(driver_instance_t *drv);
static int keyboard_driver_stop(driver_instance_t *drv);
static int keyboard_driver_cleanup(driver_instance_t *drv);
static int keyboard_driver_ioctl(driver_instance_t *drv, uint32_t cmd,
                                 void *arg);
static int keyboard_driver_load_data(driver_instance_t *drv, const void *data,
                                     size_t size);
static void keyboard_driver_print_info(const driver_instance_t *drv);

static driver_ops_t keyboard_driver_ops = {.init = keyboard_driver_init,
                                           .start = keyboard_driver_start,
                                           .stop = keyboard_driver_stop,
                                           .cleanup = keyboard_driver_cleanup,
                                           .ioctl = keyboard_driver_ioctl,
                                           .load_data =
                                               keyboard_driver_load_data};

static driver_type_info_t keyboard_driver_type = {
    .type = DRIVER_TYPE_KEYBOARD,
    .type_name = "keyboard",
    .version = "1.0.0",
    .priv_data_size = sizeof(keyboard_driver_data_t),
    .default_ops = &keyboard_driver_ops,
    .validate_data = NULL,
    .print_info = keyboard_driver_print_info};

static keyboard_layout_t *find_layout(keyboard_driver_data_t *data,
                                      const char *name);
static int add_layout(keyboard_driver_data_t *data, keyboard_layout_t *layout);
static int remove_layout(keyboard_driver_data_t *data, const char *name);

int keyboard_driver_register_type(void) {
  memcpy(default_layout.normal, us_qwerty_normal, sizeof(us_qwerty_normal));
  memcpy(default_layout.shift, us_qwerty_shift, sizeof(us_qwerty_shift));
  memcpy(default_layout.altgr, us_qwerty_altgr, sizeof(us_qwerty_altgr));
  return driver_register_type(&keyboard_driver_type);
}

driver_instance_t *keyboard_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_KEYBOARD, name);
}

int keyboard_driver_set_layout(driver_instance_t *drv,
                               const char *layout_name) {
  if (!drv || !drv->priv_data || !layout_name)
    return -1;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  keyboard_layout_t *layout = find_layout(data, layout_name);
  if (!layout)
    return -1;
  data->current_layout = layout;
  return 0;
}

keyboard_layout_t *keyboard_driver_get_current_layout(driver_instance_t *drv) {
  if (!drv || !drv->priv_data)
    return NULL;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  return data->current_layout ? data->current_layout : data->default_layout;
}

int keyboard_load_layout_from_data(driver_instance_t *drv,
                                   const void *file_data, size_t file_size) {
  if (!drv || !file_data || file_size < sizeof(keyboard_layout_t))
    return -1;
  const uint32_t *magic = (const uint32_t *)file_data;
  if (*magic != KEYBOARD_LAYOUT_MAGIC)
    return -1;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  if (!data)
    return -1;
  keyboard_layout_t *new_layout =
      (keyboard_layout_t *)kernel_malloc(sizeof(keyboard_layout_t));
  if (!new_layout)
    return -1;
  const uint8_t *layout_data = (const uint8_t *)file_data + sizeof(uint32_t);
  size_t layout_size = file_size - sizeof(uint32_t);
  if (layout_size >= sizeof(keyboard_layout_t)) {
    memcpy(new_layout, layout_data, sizeof(keyboard_layout_t));
  } else {
    memcpy(new_layout, layout_data, layout_size);
    memset((uint8_t *)new_layout + layout_size, 0,
           sizeof(keyboard_layout_t) - layout_size);
  }
  new_layout->name[sizeof(new_layout->name) - 1] = '\0';
  if (add_layout(data, new_layout) != 0) {
    kernel_free(new_layout);
    return -1;
  }
  return 0;
}

char keyboard_driver_map_scancode(const keyboard_layout_t *layout,
                                  uint8_t scancode, uint8_t shift,
                                  uint8_t altgr, uint8_t caps_lock) {
  if (!layout || scancode >= 128)
    return 0;
  uint8_t c =
      (altgr && layout->altgr[scancode] != 0)
          ? layout->altgr[scancode]
          : (shift ? layout->shift[scancode] : layout->normal[scancode]);
  if (c > 127)
    return (char)(-(int)(c - 127));
  if (caps_lock && !altgr) {
    if (c >= 'a' && c <= 'z')
      c = layout->shift[scancode];
    else if (c >= 'A' && c <= 'Z')
      c = layout->normal[scancode];
  }
  return (char)c;
}

static int keyboard_driver_init(driver_instance_t *drv, void *config) {
  if (!drv || !drv->priv_data)
    return -1;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  data->max_layouts = KEYBOARD_MAX_LAYOUTS;
  data->layout_count = 0;
  data->available_layouts = (keyboard_layout_t **)kernel_malloc(
      sizeof(keyboard_layout_t *) * data->max_layouts);
  if (!data->available_layouts)
    return -1;
  memset(data->available_layouts, 0,
         sizeof(keyboard_layout_t *) * data->max_layouts);
  data->default_layout = &default_layout;
  data->current_layout = data->default_layout;
  add_layout(data, data->default_layout);
  return 0;
}

static int keyboard_driver_start(driver_instance_t *drv) { return 0; }
static int keyboard_driver_stop(driver_instance_t *drv) { return 0; }
static int keyboard_driver_cleanup(driver_instance_t *drv) {
  if (!drv || !drv->priv_data)
    return -1;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  for (uint32_t i = 0; i < data->layout_count; i++) {
    if (data->available_layouts[i] &&
        data->available_layouts[i] != &default_layout)
      kernel_free(data->available_layouts[i]);
  }
  if (data->available_layouts)
    kernel_free(data->available_layouts);
  return 0;
}

static int keyboard_driver_ioctl(driver_instance_t *drv, uint32_t cmd,
                                 void *arg) {
  if (!drv || !drv->priv_data)
    return -1;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  switch (cmd) {
  case KBD_IOCTL_SET_LAYOUT: {
    kbd_ioctl_set_layout_t *set_arg = (kbd_ioctl_set_layout_t *)arg;
    return set_arg ? keyboard_driver_set_layout(drv, set_arg->layout_name) : -1;
  }
  case KBD_IOCTL_GET_LAYOUT: {
    char *name_arg = (char *)arg;
    keyboard_layout_t *current = keyboard_driver_get_current_layout(drv);
    if (!name_arg || !current)
      return -1;
    strncpy(name_arg, current->name, 31);
    name_arg[31] = '\0';
    return 0;
  }
  case KBD_IOCTL_LIST_LAYOUTS: {
    kbd_ioctl_list_layouts_t *list_arg = (kbd_ioctl_list_layouts_t *)arg;
    if (!list_arg)
      return -1;
    list_arg->max_layouts = data->max_layouts;
    list_arg->layout_count = data->layout_count;
    uint32_t max_copy = (list_arg->max_layouts < data->layout_count)
                            ? list_arg->max_layouts
                            : data->layout_count;
    for (uint32_t i = 0; i < max_copy; i++) {
      if (data->available_layouts[i]) {
        strncpy(list_arg->layout_names[i], data->available_layouts[i]->name,
                31);
        list_arg->layout_names[i][31] = '\0';
      }
    }
    return 0;
  }
  case KBD_IOCTL_LOAD_LAYOUT: {
    kbd_ioctl_load_layout_t *load_arg = (kbd_ioctl_load_layout_t *)arg;
    return load_arg ? driver_load_from_file(drv, load_arg->filename) : -1;
  }
  default:
    return -1;
  }
}

static int keyboard_driver_load_data(driver_instance_t *drv, const void *data,
                                     size_t size) {
  return keyboard_load_layout_from_data(drv, data, size);
}

static keyboard_layout_t *find_layout(keyboard_driver_data_t *data,
                                      const char *name) {
  if (!data || !name || !data->available_layouts)
    return NULL;
  for (uint32_t i = 0; i < data->layout_count; i++) {
    if (data->available_layouts[i] &&
        strcmp(data->available_layouts[i]->name, name) == 0)
      return data->available_layouts[i];
  }
  return NULL;
}

static int add_layout(keyboard_driver_data_t *data, keyboard_layout_t *layout) {
  if (!data || !layout || !data->available_layouts ||
      data->layout_count >= data->max_layouts)
    return -1;
  if (find_layout(data, layout->name))
    return -1;
  data->available_layouts[data->layout_count++] = layout;
  return 0;
}

static int remove_layout(keyboard_driver_data_t *data, const char *name) {
  if (!data || !name || !data->available_layouts)
    return -1;
  for (uint32_t i = 0; i < data->layout_count; i++) {
    if (data->available_layouts[i] &&
        strcmp(data->available_layouts[i]->name, name) == 0) {
      if (data->available_layouts[i] == data->default_layout)
        return -1;
      for (uint32_t j = i; j < data->layout_count - 1; j++)
        data->available_layouts[j] = data->available_layouts[j + 1];
      data->layout_count--;
      data->available_layouts[data->layout_count] = NULL;
      if (data->current_layout && strcmp(data->current_layout->name, name) == 0)
        data->current_layout = data->default_layout;
      return 0;
    }
  }
  return -1;
}

static void keyboard_driver_print_info(const driver_instance_t *drv) {
  if (!drv || !drv->priv_data)
    return;
  keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->priv_data;
  terminal_printf(&main_terminal, "  Current layout: %s\r\n",
                  (data->current_layout && data->current_layout->name[0])
                      ? data->current_layout->name
                      : "None");
}