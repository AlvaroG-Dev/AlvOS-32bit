#ifndef KEYBOARD_DRIVER_H
#define KEYBOARD_DRIVER_H

#include "../driver_system.h"
#include "../keyboard.h"  // Include keyboard.h for keyboard_layout_t
#include <stdint.h>

#define KEYBOARD_LAYOUT_MAGIC 0x4B4244  // 'KBD'
#define KEYBOARD_MAX_LAYOUTS 8

// Keyboard driver private data
typedef struct keyboard_driver_data {
    keyboard_layout_t *current_layout;
    keyboard_layout_t *default_layout;
    keyboard_layout_t **available_layouts;
    uint32_t layout_count;
    uint8_t max_layouts;
} keyboard_driver_data_t;

// Keyboard driver IOCTL commands
#define KBD_IOCTL_SET_LAYOUT     0x1001
#define KBD_IOCTL_GET_LAYOUT     0x1002
#define KBD_IOCTL_LIST_LAYOUTS   0x1003
#define KBD_IOCTL_LOAD_LAYOUT    0x1004

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
    char layout_names[][32];  // Variable length array
} kbd_ioctl_list_layouts_t;

// Public functions
int keyboard_driver_register_type(void);
driver_instance_t *keyboard_driver_create(const char *name);

// Layout management functions (used via IOCTL)
int keyboard_driver_set_layout(driver_instance_t *drv, const char *layout_name);
keyboard_layout_t *keyboard_driver_get_current_layout(driver_instance_t *drv);
int keyboard_load_layout_from_data(driver_instance_t *drv, const void *file_data, size_t file_size);

// Scancode mapping function
char keyboard_driver_map_scancode(const keyboard_layout_t *layout, uint8_t scancode, 
                                 uint8_t shift, uint8_t altgr, uint8_t caps_lock);

#endif // KEYBOARD_DRIVER_H