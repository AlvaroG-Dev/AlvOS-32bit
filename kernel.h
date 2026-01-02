#ifndef KERNEL_H
#define KERNEL_H

#include "disk.h"
#include "installer.h"
#include "memory.h"
#include "memutils.h"
#include "multiboot2.h" // Asegúrate de tener este archivo con las definiciones de Multiboot2
#include "string.h"
#include "terminal.h"
#include <stddef.h>
#include <stdint.h>

// Heap estático de reserva
// Heap estático de reserva
#define STATIC_HEAP_SIZE 0x1000000 // 16 MB
extern uint8_t kernel_heap[STATIC_HEAP_SIZE];

extern uint32_t *g_framebuffer;
extern uint32_t g_pitch_pixels;
extern uint32_t g_screen_width;
extern uint32_t g_screen_height;
extern disk_t main_disk;
extern install_options_t options;
extern bool graphical_mode;

// Información que pasa el bootloader
typedef struct {
  uint32_t magic;
  struct multiboot_tag *multiboot_info_ptr;
  struct multiboot_tag_framebuffer *framebuffer;
  struct multiboot_tag_mmap *mmap;
} BootInfo;

extern BootInfo boot_info;
extern Terminal main_terminal;
extern struct unmount_callback_data unmount_data;

// Punto de entrada del kernel (llamado desde el bootloader)
void cmain(uint32_t magic, struct multiboot_tag *mb_info);
void shutdown(void);
void test_simple_messages(void);
void keyboard_terminal_handler(int key);
void unmount_callback(const char *mountpoint, const char *fs_name, void *arg);

#endif
