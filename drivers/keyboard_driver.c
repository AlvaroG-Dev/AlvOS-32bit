// keyboard_driver.c - Keyboard driver implementation (corregido)
#include "keyboard_driver.h"
#include "../driver_system.h"
#include "../memory.h"
#include "../string.h"
#include "../terminal.h"
#include "../serial.h"

extern Terminal main_terminal;

// US QWERTY layout por defecto (scancodes PS/2 Set 1) - CORREGIDO
static const uint8_t us_qwerty_normal[128] = {
    [0x00] = 0,      [0x01] = 27,     [0x02] = '1',    [0x03] = '2',
    [0x04] = '3',    [0x05] = '4',    [0x06] = '5',    [0x07] = '6',
    [0x08] = '7',    [0x09] = '8',    [0x0A] = '9',    [0x0B] = '0',
    [0x0C] = '-',    [0x0D] = '=',    [0x0E] = '\b',   [0x0F] = '\t',
    [0x10] = 'q',    [0x11] = 'w',    [0x12] = 'e',    [0x13] = 'r',
    [0x14] = 't',    [0x15] = 'y',    [0x16] = 'u',    [0x17] = 'i',
    [0x18] = 'o',    [0x19] = 'p',    [0x1A] = '[',    [0x1B] = ']',
    [0x1C] = '\n',   [0x1D] = 0,      [0x1E] = 'a',    [0x1F] = 's',
    [0x20] = 'd',    [0x21] = 'f',    [0x22] = 'g',    [0x23] = 'h',
    [0x24] = 'j',    [0x25] = 'k',    [0x26] = 'l',    [0x27] = ';',
    [0x28] = '\'',   [0x29] = '`',    [0x2A] = 0,      [0x2B] = '\\',
    [0x2C] = 'z',    [0x2D] = 'x',    [0x2E] = 'c',    [0x2F] = 'v',
    [0x30] = 'b',    [0x31] = 'n',    [0x32] = 'm',    [0x33] = ',',
    [0x34] = '.',    [0x35] = '/',    [0x36] = 0,      [0x37] = '*',
    [0x38] = 0,      [0x39] = ' ',    [0x3A] = 0,      [0x3B] = 0,
    [0x3C] = 0,      [0x3D] = 0,      [0x3E] = 0,      [0x3F] = 0,
    [0x40] = 0,      [0x41] = 0,      [0x42] = 0,      [0x43] = 0,
    [0x44] = 0,      [0x45] = 0,      [0x46] = 0,      [0x47] = 0,
    [0x48] = 0,      [0x49] = 0,      [0x4A] = '-',    [0x4B] = 0,
    [0x4C] = 0,      [0x4D] = '+',    [0x4E] = 0,      [0x4F] = 0,
    [0x50] = 0,      [0x51] = 0,      [0x52] = 0,      [0x53] = 0,
    // Scancodes extendidos (0xE0 prefijo) - NUEVO: mapeados a códigos negativos
    [0x48] = KEY_UP,     // Up arrow (E0 48)
    [0x50] = KEY_DOWN,   // Down arrow (E0 50)  
    [0x4B] = KEY_LEFT,   // Left arrow (E0 4B)
    [0x4D] = KEY_RIGHT,  // Right arrow (E0 4D)
    [0x47] = KEY_HOME,   // Home (E0 47)
    [0x4F] = KEY_END,    // End (E0 4F)
    [0x49] = KEY_PGUP,   // Page Up (E0 49)
    [0x51] = KEY_PGDOWN, // Page Down (E0 51)
    [0x52] = KEY_INSERT, // Insert (E0 52)
    [0x53] = KEY_DELETE, // Delete (E0 53)
    [0x54 ... 0x7F] = 0
};

static const uint8_t us_qwerty_shift[128] = {
    [0x00] = 0,      [0x01] = 27,     [0x02] = '!',    [0x03] = '@',
    [0x04] = '#',    [0x05] = '$',    [0x06] = '%',    [0x07] = '^',
    [0x08] = '&',    [0x09] = '*',    [0x0A] = '(',    [0x0B] = ')',
    [0x0C] = '_',    [0x0D] = '+',    [0x0E] = '\b',   [0x0F] = '\t',
    [0x10] = 'Q',    [0x11] = 'W',    [0x12] = 'E',    [0x13] = 'R',
    [0x14] = 'T',    [0x15] = 'Y',    [0x16] = 'U',    [0x17] = 'I',
    [0x18] = 'O',    [0x19] = 'P',    [0x1A] = '{',    [0x1B] = '}',
    [0x1C] = '\n',   [0x1D] = 0,      [0x1E] = 'A',    [0x1F] = 'S',
    [0x20] = 'D',    [0x21] = 'F',    [0x22] = 'G',    [0x23] = 'H',
    [0x24] = 'J',    [0x25] = 'K',    [0x26] = 'L',    [0x27] = ':',
    [0x28] = '\"',   [0x29] = '~',    [0x2A] = 0,      [0x2B] = '|',
    [0x2C] = 'Z',    [0x2D] = 'X',    [0x2E] = 'C',    [0x2F] = 'V',
    [0x30] = 'B',    [0x31] = 'N',    [0x32] = 'M',    [0x33] = '<',
    [0x34] = '>',    [0x35] = '?',    [0x36] = 0,      [0x37] = '*',
    [0x38] = 0,      [0x39] = ' ',    [0x3A] = 0,      [0x3B] = 0,
    // Scancodes extendidos igual que en normal (las flechas no cambian con shift)
    [0x48] = KEY_UP,     [0x50] = KEY_DOWN,   [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,  [0x47] = KEY_HOME,   [0x4F] = KEY_END,
    [0x49] = KEY_PGUP,   [0x51] = KEY_PGDOWN, [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x3C ... 0x7F] = 0
};

static const uint8_t us_qwerty_altgr[128] = {
    // AltGr mappings para US QWERTY
    [0x02] = 0,      [0x03] = 0,      [0x04] = 0,      [0x05] = 0,
    [0x06] = 0,      [0x07] = 0,      [0x08] = 0,      [0x09] = 0,
    [0x0A] = 0,      [0x0B] = 0,      [0x0C] = 0,      [0x0D] = 0,
    [0x10] = 0,      [0x11] = 0,      [0x12] = 0x80,   [0x13] = 0,
    [0x18] = 0,      [0x19] = 0,      [0x1A] = 0,      [0x1B] = 0,
    [0x1E] = 0,      [0x1F] = 0,      [0x20] = 0,      [0x21] = 0,
    [0x27] = 0,      [0x28] = 0,      [0x29] = 0,      [0x2B] = 0,
    // Scancodes extendidos igual que en normal
    [0x48] = KEY_UP,     [0x50] = KEY_DOWN,   [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,  [0x47] = KEY_HOME,   [0x4F] = KEY_END,
    [0x49] = KEY_PGUP,   [0x51] = KEY_PGDOWN, [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x00 ... 0x7F] = 0
};

// Layout por defecto (US QWERTY)
static keyboard_layout_t default_layout = {
    .name = "US-QWERTY",
    .normal = {0},
    .shift = {0},
    .altgr = {0}
};

// Driver operations
static int keyboard_driver_init(driver_instance_t *drv, void *config);
static int keyboard_driver_start(driver_instance_t *drv);
static int keyboard_driver_stop(driver_instance_t *drv);
static int keyboard_driver_cleanup(driver_instance_t *drv);
static int keyboard_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg);
static int keyboard_driver_load_data(driver_instance_t *drv, const void *data, size_t size);
static void keyboard_driver_print_info(const driver_instance_t *drv);

static driver_ops_t keyboard_driver_ops = {
    .init = keyboard_driver_init,
    .start = keyboard_driver_start,
    .stop = keyboard_driver_stop,
    .cleanup = keyboard_driver_cleanup,
    .ioctl = keyboard_driver_ioctl,
    .load_data = keyboard_driver_load_data
};

// Type information
static driver_type_info_t keyboard_driver_type = {
    .type = DRIVER_TYPE_KEYBOARD,
    .type_name = "keyboard",
    .version = "1.0.0",
    .private_data_size = sizeof(keyboard_driver_data_t),
    .default_ops = &keyboard_driver_ops,
    .validate_data = NULL, // No validation for now
    .print_info = keyboard_driver_print_info
};

// Internal helper functions
static keyboard_layout_t *find_layout(keyboard_driver_data_t *data, const char *name);
static int add_layout(keyboard_driver_data_t *data, keyboard_layout_t *layout);
static int remove_layout(keyboard_driver_data_t *data, const char *name);

// Public API implementation
int keyboard_driver_register_type(void) {
    // Initialize default layout
    memcpy(default_layout.normal, us_qwerty_normal, sizeof(us_qwerty_normal));
    memcpy(default_layout.shift, us_qwerty_shift, sizeof(us_qwerty_shift));
    memcpy(default_layout.altgr, us_qwerty_altgr, sizeof(us_qwerty_altgr));
    
    return driver_register_type(&keyboard_driver_type);
}

driver_instance_t *keyboard_driver_create(const char *name) {
    return driver_create(DRIVER_TYPE_KEYBOARD, name);
}

int keyboard_driver_set_layout(driver_instance_t *drv, const char *layout_name) {
    if (!drv || !drv->private_data || !layout_name) {
        return -1;
    }
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    keyboard_layout_t *layout = find_layout(data, layout_name);
    
    if (!layout) {
        terminal_printf(&main_terminal, "Keyboard driver: Layout '%s' not found\r\n", layout_name);
        return -1;
    }
    
    data->current_layout = layout;
    terminal_printf(&main_terminal, "Keyboard driver: Switched to layout '%s'\r\n", layout_name);
    return 0;
}

keyboard_layout_t *keyboard_driver_get_current_layout(driver_instance_t *drv) {
    if (!drv || !drv->private_data) {
        return NULL;
    }
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    return data->current_layout ? data->current_layout : data->default_layout;
}

int keyboard_load_layout_from_data(driver_instance_t *drv, const void *file_data, size_t file_size) {
    if (!drv || !file_data || file_size < sizeof(keyboard_layout_t)) {
        return -1;
    }
    
    // Validate magic number
    const uint32_t *magic = (const uint32_t *)file_data;
    if (*magic != KEYBOARD_LAYOUT_MAGIC) {
        terminal_printf(&main_terminal, "Keyboard driver: Invalid layout magic number\r\n");
        return -1;
    }
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    if (!data) return -1;
    
    // Allocate new layout
    keyboard_layout_t *new_layout = (keyboard_layout_t *)kernel_malloc(sizeof(keyboard_layout_t));
    if (!new_layout) {
        terminal_printf(&main_terminal, "Keyboard driver: Failed to allocate layout memory\r\n");
        return -1;
    }
    
    // Copy layout data (skip magic number)
    const uint8_t *layout_data = (const uint8_t *)file_data + sizeof(uint32_t);
    size_t layout_size = file_size - sizeof(uint32_t);
    
    if (layout_size >= sizeof(keyboard_layout_t)) {
        memcpy(new_layout, layout_data, sizeof(keyboard_layout_t));
    } else {
        // Partial copy if file is smaller than expected
        memcpy(new_layout, layout_data, layout_size);
        memset((uint8_t*)new_layout + layout_size, 0, sizeof(keyboard_layout_t) - layout_size);
    }
    
    // Ensure null termination of name
    new_layout->name[sizeof(new_layout->name) - 1] = '\0';
    
    // Add to available layouts
    if (add_layout(data, new_layout) != 0) {
        kernel_free(new_layout);
        return -1;
    }
    
    terminal_printf(&main_terminal, "Keyboard driver: Loaded layout '%s'\r\n", new_layout->name);
    return 0;
}

char keyboard_driver_map_scancode(const keyboard_layout_t *layout, uint8_t scancode, 
                          uint8_t shift, uint8_t altgr, uint8_t caps_lock) {
    if (!layout || scancode >= 128) return 0;
    
    uint8_t c = 0;
    
    if (altgr && layout->altgr[scancode] != 0) {
        c = layout->altgr[scancode];
    } else if (shift) {
        c = layout->shift[scancode];
    } else {
        c = layout->normal[scancode];
    }
    
    // Si es una tecla especial (código negativo), retornarla directamente
    if (c > 127) {
        // Convertir a código negativo para teclas especiales
        return (char)(-(int)(c - 127));
    }
    
    // Apply caps lock (only affects letters)
    if (caps_lock && !altgr) {
        if (c >= 'a' && c <= 'z') {
            c = layout->shift[scancode]; // Use shift mapping for uppercase
        } else if (c >= 'A' && c <= 'Z') {
            c = layout->normal[scancode]; // Use normal mapping for lowercase
        }
    }
    
    return (char)c;
}

// Driver operations implementation
static int keyboard_driver_init(driver_instance_t *drv, void *config) {
    (void)config; // Unused for now
    
    if (!drv || !drv->private_data) return -1;
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    
    // Initialize driver data
    data->max_layouts = KEYBOARD_MAX_LAYOUTS;
    data->layout_count = 0;
    
    // Allocate array for available layouts
    data->available_layouts = (keyboard_layout_t **)kernel_malloc(
        sizeof(keyboard_layout_t *) * data->max_layouts);
    
    if (!data->available_layouts) {
        terminal_printf(&main_terminal, "Keyboard driver: Failed to allocate layouts array\r\n");
        return -1;
    }
    
    memset(data->available_layouts, 0, sizeof(keyboard_layout_t *) * data->max_layouts);
    
    // Set default layout
    data->default_layout = &default_layout;
    data->current_layout = data->default_layout;
    
    // Add default layout to available layouts
    if (add_layout(data, data->default_layout) != 0) {
        kernel_free(data->available_layouts);
        data->available_layouts = NULL;
        return -1;
    }
    
    terminal_printf(&main_terminal, "Keyboard driver: Initialized with layout '%s'\r\n", 
                   data->default_layout->name);
    return 0;
}

static int keyboard_driver_start(driver_instance_t *drv) {
    if (!drv) return -1;
    
    terminal_printf(&main_terminal, "Keyboard driver: Started\r\n");
    return 0;
}

static int keyboard_driver_stop(driver_instance_t *drv) {
    if (!drv) return -1;
    
    return 0;
}

static int keyboard_driver_cleanup(driver_instance_t *drv) {
    if (!drv || !drv->private_data) return -1;
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    
    // Free allocated layouts (except default which is static)
    for (uint32_t i = 0; i < data->layout_count; i++) {
        if (data->available_layouts[i] && data->available_layouts[i] != &default_layout) {
            kernel_free(data->available_layouts[i]);
        }
    }
    
    // Free layouts array
    if (data->available_layouts) {
        kernel_free(data->available_layouts);
        data->available_layouts = NULL;
    }
    
    terminal_printf(&main_terminal, "Keyboard driver: Cleaned up\r\n");
    return 0;
}

static int keyboard_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
    if (!drv || !drv->private_data) return -1;
    
    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    
    switch (cmd) {
        case KBD_IOCTL_SET_LAYOUT: {
            kbd_ioctl_set_layout_t *set_arg = (kbd_ioctl_set_layout_t *)arg;
            if (!set_arg) return -1;
            return keyboard_driver_set_layout(drv, set_arg->layout_name); // Use driver-specific function
        }
        
        case KBD_IOCTL_GET_LAYOUT: {
            char *name_arg = (char *)arg;
            if (!name_arg) return -1;
            
            keyboard_layout_t *current = keyboard_driver_get_current_layout(drv); // Use driver-specific function
            if (!current) return -1;
            
            strncpy(name_arg, current->name, 31);
            name_arg[31] = '\0';
            return 0;
        }
        
        case KBD_IOCTL_LIST_LAYOUTS: {
            kbd_ioctl_list_layouts_t *list_arg = (kbd_ioctl_list_layouts_t *)arg;
            if (!list_arg) return -1;
            
            list_arg->max_layouts = data->max_layouts;
            list_arg->layout_count = data->layout_count;
            
            // Copy layout names (up to the available space)
            uint32_t max_copy = (list_arg->max_layouts < data->layout_count) ? 
                               list_arg->max_layouts : data->layout_count;
            
            for (uint32_t i = 0; i < max_copy; i++) {
                if (data->available_layouts[i]) {
                    strncpy(list_arg->layout_names[i], data->available_layouts[i]->name, 31);
                    list_arg->layout_names[i][31] = '\0';
                }
            }
            
            return 0;
        }
        
        case KBD_IOCTL_LOAD_LAYOUT: {
            kbd_ioctl_load_layout_t *load_arg = (kbd_ioctl_load_layout_t *)arg;
            if (!load_arg) return -1;
            
            // Load layout from file using driver_load_from_file
            terminal_printf(&main_terminal, "Keyboard driver: Load layout from '%s'\r\n", 
                           load_arg->filename);
            return driver_load_from_file(drv, load_arg->filename);
        }
        
        default:
            terminal_printf(&main_terminal, "Keyboard driver: Unknown IOCTL command 0x%x\r\n", cmd);
            return -1;
    }
}

static int keyboard_driver_load_data(driver_instance_t *drv, const void *data, size_t size) {
    return keyboard_load_layout_from_data(drv, data, size);
}

// Internal helper functions
static keyboard_layout_t *find_layout(keyboard_driver_data_t *data, const char *name) {
    if (!data || !name || !data->available_layouts) return NULL;
    
    for (uint32_t i = 0; i < data->layout_count; i++) {
        if (data->available_layouts[i] && 
            strcmp(data->available_layouts[i]->name, name) == 0) {
            return data->available_layouts[i];
        }
    }
    
    return NULL;
}

static int add_layout(keyboard_driver_data_t *data, keyboard_layout_t *layout) {
    if (!data || !layout || !data->available_layouts) return -1;
    
    // Check if layout already exists
    if (find_layout(data, layout->name)) {
        terminal_printf(&main_terminal, "Keyboard driver: Layout '%s' already exists\r\n", 
                       layout->name);
        return -1;
    }
    
    // Check if we have space
    if (data->layout_count >= data->max_layouts) {
        terminal_printf(&main_terminal, "Keyboard driver: No space for more layouts\r\n");
        return -1;
    }
    
    // Add layout
    data->available_layouts[data->layout_count++] = layout;
    return 0;
}

static int remove_layout(keyboard_driver_data_t *data, const char *name) {
    if (!data || !name || !data->available_layouts) return -1;
    
    for (uint32_t i = 0; i < data->layout_count; i++) {
        if (data->available_layouts[i] && 
            strcmp(data->available_layouts[i]->name, name) == 0) {
            
            // Don't remove the default layout
            if (data->available_layouts[i] == data->default_layout) {
                terminal_printf(&main_terminal, "Keyboard driver: Cannot remove default layout\r\n");
                return -1;
            }
            
            // Shift remaining layouts
            for (uint32_t j = i; j < data->layout_count - 1; j++) {
                data->available_layouts[j] = data->available_layouts[j + 1];
            }
            
            data->layout_count--;
            data->available_layouts[data->layout_count] = NULL;
            
            // If we removed the current layout, fall back to default
            if (data->current_layout && strcmp(data->current_layout->name, name) == 0) {
                data->current_layout = data->default_layout;
                terminal_printf(&main_terminal, "Keyboard driver: Current layout removed, switched to default\r\n");
            }
            
            return 0;
        }
    }
    
    return -1; // Layout not found
}

static void keyboard_driver_print_info(const driver_instance_t *drv) {
    if (!drv || !drv->private_data) {
        terminal_printf(&main_terminal, "  No private data available\r\n");
        return;
    }

    keyboard_driver_data_t *data = (keyboard_driver_data_t *)drv->private_data;
    
    // Print current layout
    const char *current_layout_name = (data->current_layout && data->current_layout->name[0]) 
                                     ? data->current_layout->name 
                                     : "None";
    terminal_printf(&main_terminal, "  Current layout: %s\r\n", current_layout_name);
    
    // Print number of available layouts
    terminal_printf(&main_terminal, "  Available layouts: %u/%u\r\n", 
                   data->layout_count, data->max_layouts);
    
    // List all available layouts
    if (data->layout_count > 0) {
        terminal_printf(&main_terminal, "  Layouts:\r\n");
        for (uint32_t i = 0; i < data->layout_count; i++) {
            if (data->available_layouts[i]) {
                terminal_printf(&main_terminal, "    - %s%s\r\n", 
                               data->available_layouts[i]->name,
                               (data->available_layouts[i] == data->current_layout) ? " (current)" : "");
            }
        }
    }
}