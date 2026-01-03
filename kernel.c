#include "kernel.h"
#include "acpi.h"
#include "apic.h"
#include "atapi.h"
#include "chardev.h"
#include "chardev_vfs.h"
#include "cpuid.h"
#include "disk.h"
#include "disk_io_daemon.h"
#include "dma.h"
#include "drawing.h"
#include "driver_system.h"
#include "e1000.h"
#include "fat32.h"
#include "gdt.h"
#include "ide.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "log.h"
#include "mmu.h"
#include "module_loader.h"
#include "network.h"
#include "partition.h"
#include "partition_manager.h"
#include "pci.h"
#include "pmm.h"
#include "sata_disk.h"
#include "serial.h"
#include "syscalls.h"
#include "task.h"
#include "task_utils.h"
#include "terminal.h"
#include "tmpfs.h"
#include "vfs.h"

// Global definition of BootInfo.
BootInfo boot_info;

// Definición global del heap del kernel (16MB)
uint8_t kernel_heap[STATIC_HEAP_SIZE] __attribute__((aligned(PAGE_SIZE)));

uint32_t *g_framebuffer = NULL;
uint32_t g_pitch_pixels = 0;
uint32_t g_screen_width = 0;
uint32_t g_screen_height = 0;
Terminal main_terminal;
disk_t main_disk;
extern vfs_fs_type_t fat32_fs_type;
extern vfs_fs_type_t sysfs_type;
extern vfs_file_t *fd_table[VFS_MAX_FDS];
extern vfs_superblock_t *mount_table[VFS_MAX_MOUNTS];
extern char mount_points[VFS_MAX_MOUNTS][VFS_PATH_MAX];
extern int mount_count;
extern char _start;
extern char _end;
extern char _stack_top;
extern disk_err_t disk_init_from_partition(disk_t *partition_disk,
                                           disk_t *physical_disk,
                                           partition_info_t *partition);
extern void task_start_first(cpu_context_t *context);
install_options_t options = {.mode = INSTALL_MODE_FULL,
                             .force = true,
                             .verify = true,
                             .backup_mbr = true,
                             .set_bootable = true,
                             .target_partition = 0};
void keyboard_terminal_handler(int key);
static void main_loop_task(void *arg);

// Necesitamos una función para listar montajes - usar vfs_list_mounts
// Primero creamos un callback para desmontar
struct unmount_callback_data {
  int count;
  int errors;
} unmount_data = {0, 0};

// Función auxiliar para callback
void unmount_callback(const char *mountpoint, const char *fs_name, void *arg) {
  struct unmount_callback_data *data = (struct unmount_callback_data *)arg;
  if (vfs_unmount(mountpoint) != VFS_OK) {
    data->errors++;
  }
  data->count++;
}

// Función de apagado del sistema operativo
void shutdown(void) {
  terminal_printf(&main_terminal, "\n\nSystem shutdown initiated\r\n");
  serial_write_string(COM1_BASE, "System shutdown initiated\r\n");
  terminal_destroy(&main_terminal);
  // 1. Deshabilitar interrupciones
  __asm__ volatile("cli");

  // 2. Detener el scheduler
  if (scheduler.scheduler_enabled) {
    scheduler_stop();
  }
  // 3. Terminar todas las tareas (excepto idle)
  task_t *current = scheduler.task_list;
  if (current) {
    do {
      task_t *next = current->next;
      if (current != scheduler.idle_task) {
        task_destroy(current);
      }
      current = next;
    } while (current != scheduler.task_list);
  }

  task_cleanup_zombies();

  // 4. Desmontar sistemas de archivos (DEBE ser antes de limpiar drivers para
  // poder flashear caches)
  vfs_list_mounts(unmount_callback, &unmount_data);

  // 5. Limpiar sistema de drivers
  driver_system_cleanup();

  // 6. Limpiar módulos
  module_loader_cleanup();

  // 7. Deshabilitar PICs
  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);

  // 8. Reportar estadísticas finales del heap
  dma_cleanup();

  heap_info_t heap_info = heap_stats();
  // 9. Intentar apagado ACPI
  if (acpi_is_supported()) {
    acpi_power_off();
  }
  // Halt final
  while (1) {
    __asm__ volatile("cli; hlt");
  }
}

void initialize_acpi_pci(void) {
  // Mapear regiones críticas para ACPI
  if (!mmu_is_mapped(0x040E)) {
    mmu_map_region(0x0400, 0x0400, 0x100, PAGE_PRESENT | PAGE_RW);
  }
  if (!mmu_is_mapped(0x80000)) {
    mmu_map_region(0x80000, 0x80000, 0x20000, PAGE_PRESENT | PAGE_RW);
  }
  if (!mmu_is_mapped(0xE0000)) {
    mmu_map_region(0xE0000, 0xE0000, 0x20000, PAGE_PRESENT | PAGE_RW);
  }
  // Inicializar PCI
  pci_init();
  driver_instance_t *pci_drv = pci_driver_create("pci_bus");
  if (pci_drv) {
    driver_init(pci_drv, NULL);
    driver_start(pci_drv);
  }

  // Inicializar ACPI
  acpi_init();
  if (acpi_is_supported()) {
    if (acpi_enable()) {
    }
  }
  // NUEVO: Inicializar APIC si está disponible
  if (apic_init()) {
  }
}

void cmain(uint32_t magic, struct multiboot_tag *mb_info) {
  // Verify Multiboot2
  if (magic != 0x36d76289) {
    while (1) {
    }
  }
  boot_info.magic = magic;
  boot_info.multiboot_info_ptr = mb_info;
  // Parse Multiboot2 tags
  struct multiboot_tag *tag = (struct multiboot_tag *)((uint8_t *)mb_info + 8);
  while (tag->type != MULTIBOOT_TAG_TYPE_END) {
    switch (tag->type) {
    case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
      boot_info.framebuffer = (struct multiboot_tag_framebuffer *)tag;
      break;
    case MULTIBOOT_TAG_TYPE_MMAP:
      boot_info.mmap = (struct multiboot_tag_mmap *)tag;
      break;
    }
    tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
  }
  if (!boot_info.framebuffer) {
    while (1) {
    }
  }
  // Configurar framebuffer
  uint64_t fb_addr = boot_info.framebuffer->common.framebuffer_addr;
  uint32_t width = boot_info.framebuffer->common.framebuffer_width;
  uint32_t height = boot_info.framebuffer->common.framebuffer_height;
  uint32_t pitch = boot_info.framebuffer->common.framebuffer_pitch;
  uint32_t bpp = boot_info.framebuffer->common.framebuffer_bpp;
  uint32_t pitch_pixels = pitch / 4;
  uint32_t *screen = (uint32_t *)(uintptr_t)fb_addr;
  g_framebuffer = screen;
  g_pitch_pixels = pitch_pixels;
  g_screen_width = width;
  g_screen_height = height;

  // ============================================
  // FASE DE BOOT CON MENSAJES FORMATEADOS
  // ============================================

  // 1. Inicializar memoria
  if (boot_info.mmap) {
    pmm_init(boot_info.mmap);
  } else {
    while (1) {
    }
  }

  mmu_init();

  size_t heap_size = STATIC_HEAP_SIZE;
  heap_init(kernel_heap, heap_size);
  pmm_exclude_kernel_heap(kernel_heap, heap_size);

  vmm_init();

  // Inicializar framebuffer básico
  fb_init(g_framebuffer, width, height, pitch, bpp);
  // 2. Inicializar GDT, IDT
  gdt_init();

  idt_init();

  cpuid_init();

  // Inicializar PIT a 100Hz temporalmente
  uint32_t divisor = 1193180 / 100;
  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
  terminal_init(&main_terminal);
  // 13. Inicializar drivers
  driver_system_init();

  // 14. Cargar layout de teclado
  if (keyboard_load_layout("/dev/ES-KBD.KBD", "ES-QWERTY") == 0) {
    keyboard_set_layout("ES-QWERTY");
  }

  initialize_acpi_pci();

  // NUEVO: Ahora sí inicializar el timer (usará APIC si está disponible)
  __asm__ volatile("cli");
  pit_init(100); // Esto usará APIC timer si está disponible

  // 7. Inicializar ACPI/PCI/APIC (MODIFICADO)
  irq_setup_apic();

  // 3. Inicializar teclado
  keyboard_init();

  chardev_init();

  // 5. Inicializar serial
  serial_init();

  driver_instance_t *serial_drv = serial_driver_create("com_ports");
  if (serial_drv) {
    driver_init(serial_drv, NULL);
    driver_start(serial_drv);
  }

  // Registrar tipo de driver E1000
  if (e1000_driver_register_type() != 0) {
    terminal_puts(&main_terminal,
                  "WARNING: Failed to register E1000 driver type\r\n");
  }

  // Inicializar capa de red
  network_init();

  // Crear instancia del driver
  driver_instance_t *net_drv = e1000_driver_create("eth0");
  if (net_drv) {
    driver_init(net_drv, NULL);
    driver_start(net_drv);
  }

  // 6. Inicializar VFS
  vfs_init();
  vfs_register_fs(&tmpfs_type);
  vfs_register_fs(&fat32_fs_type);
  vfs_register_fs(&sysfs_type);
  vfs_register_fs(&devfs_type);

  // 8. Inicializar SATA/AHCI
  bool sata_available = false;
  if (sata_disk_init()) {
    sata_available = true;
  }

  // 9. Inicializar ATAPI
  bool atapi_available = false;
  if (atapi_init()) {
    atapi_available = true;
  }

  if (ide_driver_register_type() == 0) {
    driver_instance_t *ide_drv = ide_driver_create("ide0");
    if (ide_drv) {
      driver_init(ide_drv, NULL);
      driver_start(ide_drv);
    }
  }

  // 10. Montar sistemas de archivos
  bool home_mounted = false;

  vfs_mount("/", "tmpfs", NULL);
  vfs_mount("/dev", "devfs", NULL);
  vfs_mount("/ramfs", "tmpfs", NULL);
  vfs_mount("/sys", "sysfs", NULL);
  vfs_mount("/sys", "sysfs", NULL);

  // Intentar montar disco persistente
  bool disk_hardware_initialized = false;

  // Probar SATA con soporte multi-disco
  if (sata_available) {
    disk_t *sata_disk = (disk_t *)kernel_malloc(sizeof(disk_t));
    if (sata_disk && sata_to_legacy_disk_init(sata_disk, 0) == DISK_ERR_NONE) {
      disk_hardware_initialized = true;
      memcpy(&main_disk, sata_disk, sizeof(disk_t));
      kernel_free(sata_disk);
    }
  }

  if (!home_mounted && !disk_hardware_initialized) {
    disk_err_t ide_init = disk_init(&main_disk, 0);
    if (ide_init == DISK_ERR_NONE && main_disk.initialized) {
      disk_hardware_initialized = true;
      uint8_t *test_buffer = (uint8_t *)kernel_malloc(512);
      if (test_buffer &&
          disk_read_dispatch(&main_disk, 0, 1, test_buffer) == DISK_ERR_NONE) {
        kernel_free(test_buffer);
      }
    }
  }

  // Fallback a ATAPI
  if (!home_mounted && !disk_hardware_initialized && atapi_available &&
      atapi_get_device_count() > 0) {
    if (disk_init_atapi(&main_disk, 0) == DISK_ERR_NONE &&
        main_disk.initialized) {
      disk_hardware_initialized = true;
      if (disk_atapi_media_present(&main_disk)) {
        uint8_t *test_buffer = (uint8_t *)kernel_malloc(512);
        if (test_buffer && disk_read_dispatch(&main_disk, 0, 1, test_buffer) ==
                               DISK_ERR_NONE) {
          kernel_free(test_buffer);
        }
      }
    }
  }

  // Fallback final a tmpfs
  if (!home_mounted) {
    vfs_mount("/home", "tmpfs", NULL);
  }

  sata_disk_debug_port(4);

  // 11. Inicializar logging
  log_init();

  // Inicializar gestor de particiones
  partition_manager_init();
  disk_scan_all_buses();
  disk_list_detected_devices();

  // Escanear disco principal al inicio
  partition_manager_scan_disk(&main_disk, 0);
  partition_manager_auto_mount_all();

  syscall_init();

  // 15. Inicializar mouse
  // mouse_init(g_screen_width, g_screen_height);
  //

  // 12. Inicializar multitarea
  serial_write_string(COM1_BASE, "MicroKernel OS\r\n");
  task_init();

  // ============================================
  // INICIAR TERMINAL NORMAL
  // ============================================
  // Limpiar pantalla
  set_colors(COLOR_WHITE, COLOR_BLACK);

  terminal_puts(&main_terminal, "Starting scheduler...\n");

  task_profiling_enable();

  message_system_init();

  // disk_io_daemon_init();
  task_t *mem_defrag =
      task_create("Memory Defrag", memory_defrag_task, NULL, TASK_PRIORITY_LOW);
  task_t *cleanup =
      task_create("cleanupd", cleanup_task, NULL, TASK_PRIORITY_LOW);
  // Crear tarea principal del loop
  task_t *main_loop =
      task_create("main_loop", main_loop_task, NULL, TASK_PRIORITY_HIGH);
  if (!main_loop) {
    terminal_puts(&main_terminal, "FATAL: Failed to create main loop task\n");
    while (1)
      __asm__("hlt");
  }
  // PASO 1: Marcar TODAS las tareas como READY
  terminal_puts(&main_terminal, "Setting all tasks to READY...\n");
  if (scheduler.task_list) {
    task_t *t = scheduler.task_list;
    do {
      t->state = TASK_READY;
      t->time_slice = scheduler.quantum_ticks;
      terminal_printf(&main_terminal, "  %s -> READY\n", t->name);
      t = t->next;
    } while (t != scheduler.task_list);
  }
  // PASO 2: Seleccionar primera tarea (no idle)
  terminal_puts(&main_terminal, "Selecting first task...\n");
  task_t *first = NULL;
  if (scheduler.task_list) {
    task_t *t = scheduler.task_list;
    do {
      if (t != scheduler.idle_task) {
        first = t;
        break;
      }
      t = t->next;
    } while (t != scheduler.task_list);
  }
  // Si no hay tareas, usar idle
  if (!first) {
    first = scheduler.idle_task;
  }
  terminal_printf(&main_terminal, "First task: %s\n",
                  first ? first->name : "NULL");
  // PASO 3: Configurar primera tarea como RUNNING
  if (first) {
    first->state = TASK_RUNNING;
    scheduler.current_task = first;
    first->time_slice = scheduler.quantum_ticks;
    terminal_printf(&main_terminal, "  EIP: 0x%08x\n", first->context.eip);
    terminal_printf(&main_terminal, "  ESP: 0x%08x\n", first->context.esp);
    terminal_printf(&main_terminal, "  EFLAGS: 0x%08x\n",
                    first->context.eflags);
  }
  // PASO 4: Habilitar scheduler
  scheduler.scheduler_enabled = true;
  terminal_printf(&main_terminal, "Scheduler enabled: %d\n",
                  scheduler.scheduler_enabled);
  // PASO 5: Mostrar estado final
  terminal_puts(&main_terminal, "\nFinal task states:\n");
  if (scheduler.task_list) {
    task_t *t = scheduler.task_list;
    do {
      const char *state_str = (t == first) ? "RUNNING" : "READY";
      terminal_printf(&main_terminal, "  %s: %s\n", t->name, state_str);
      t = t->next;
    } while (t != scheduler.task_list);
  }
  terminal_clear(&main_terminal);
  terminal_puts(&main_terminal, "\nJumping to first task...\n");
  terminal_puts(&main_terminal,
                "===============================================\n");
  // PASO 6: Verificación final antes del salto
  // Verificar que el contexto es válido
  if (!first || !first->context.eip || !first->context.esp) {
    terminal_puts(&main_terminal, "FATAL: Invalid first task context!\n");
    terminal_printf(&main_terminal, "  Task: %s\n",
                    first ? first->name : "NULL");
    terminal_printf(&main_terminal, "  EIP: 0x%08x\n",
                    first ? first->context.eip : 0);
    terminal_printf(&main_terminal, "  ESP: 0x%08x\n",
                    first ? first->context.esp : 0);
    while (1)
      __asm__ volatile("cli; hlt");
  }
  // Verificar segmentos (deben ser kernel: CS=0x08, DS/SS=0x10)
  if (first->context.cs != 0x08) {
    terminal_printf(&main_terminal,
                    "FATAL: Invalid CS: 0x%04x (expected 0x08)\n",
                    first->context.cs);
    while (1)
      __asm__ volatile("cli; hlt");
  }
  if (first->context.ds != 0x10 || first->context.ss != 0x10) {
    terminal_printf(
        &main_terminal,
        "FATAL: Invalid DS/SS: DS=0x%04x SS=0x%04x (expected 0x10)\n",
        first->context.ds, first->context.ss);
    while (1)
      __asm__ volatile("cli; hlt");
  }
  // Verificar alineación del stack (16 bytes)
  if (first->context.esp & 0xF) {
    terminal_printf(&main_terminal,
                    "WARN: Stack not aligned: 0x%08x, fixing...\n",
                    first->context.esp);
    first->context.esp &= ~0xF; // Forzar alineación
    terminal_printf(&main_terminal, "  New ESP: 0x%08x\n", first->context.esp);
  }
  // Limpiar EFLAGS de bits peligrosos
  uint32_t old_eflags = first->context.eflags;
  first->context.eflags = (first->context.eflags & 0x00000CD5) | 0x00000202;
  if (old_eflags != first->context.eflags) {
    terminal_printf(&main_terminal, "WARN: EFLAGS cleaned: 0x%08x -> 0x%08x\n",
                    old_eflags, first->context.eflags);
  }
  // Mostrar resumen final
  terminal_puts(&main_terminal, "\nFinal context:\n");
  terminal_printf(&main_terminal, "  Task: %s (ID: %u)\n", first->name,
                  first->task_id);
  terminal_printf(&main_terminal, "  EIP: 0x%08x\n", first->context.eip);
  terminal_printf(&main_terminal, "  ESP: 0x%08x (aligned: %s)\n",
                  first->context.esp,
                  (first->context.esp & 0xF) == 0 ? "YES" : "NO");
  terminal_printf(&main_terminal, "  EBP: 0x%08x\n", first->context.ebp);
  terminal_printf(&main_terminal, "  CS:  0x%04x  DS:  0x%04x\n",
                  first->context.cs, first->context.ds);
  terminal_printf(&main_terminal, "  SS:  0x%04x  ES:  0x%04x\n",
                  first->context.ss, first->context.es);
  terminal_printf(&main_terminal, "  EFLAGS: 0x%08x (IF=%d)\n",
                  first->context.eflags,
                  (first->context.eflags & 0x200) ? 1 : 0);
  terminal_puts(&main_terminal, "\n");
  // Pequeña pausa para que el terminal se actualice
  for (volatile int i = 0; i < 1000000; i++)
    ;
  terminal_clear(&main_terminal);
  // PASO 7: Deshabilitar interrupciones y saltar
  __asm__ volatile("cli");
  // Saltar a primera tarea (NUNCA RETORNA)
  task_start_first(&first->context);
  // NUNCA debería llegar aquí
  terminal_puts(&main_terminal, "FATAL: Returned from task_start_first!\n");
  while (1)
    __asm__ volatile("cli; hlt");
}

static void main_loop_task(void *arg) {
  (void)arg;
  uint32_t last_update = 0;
  terminal_printf(&main_terminal, "[MAIN_LOOP] Task started\r\n");
  keyboard_set_handler(keyboard_terminal_handler);
  while (1) {
    uint32_t current_time = ticks_since_boot * 10; // 10ms per tick a 100Hz
    // Actualizar cada 50ms (5 ticks)
    if (current_time - last_update >= 50) {
      // Actualizar cursor de terminal
      terminal_update_cursor_blink(&main_terminal, current_time);
      static uint8_t last_cursor_visible = 1;
      static uint32_t last_cursor_x = 0, last_cursor_y = 0;
      if (main_terminal.cursor_state_changed ||
          main_terminal.cursor_visible != last_cursor_visible ||
          main_terminal.cursor_x != last_cursor_x ||
          main_terminal.cursor_y != last_cursor_y) {
        terminal_draw(&main_terminal);
        last_cursor_visible = main_terminal.cursor_visible;
        last_cursor_x = main_terminal.cursor_x;
        last_cursor_y = main_terminal.cursor_y;
      }
      last_update = current_time;
    }
    // CRÍTICO: Dormir en lugar de yield inmediato
    // Esto permite que otras tareas se ejecuten
    task_sleep(1); // Dormir 10ms, luego el scheduler lo despertará
  }
}

void keyboard_terminal_handler(int key) {
  terminal_handle_key(&main_terminal, key);
}