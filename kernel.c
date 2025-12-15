#include "kernel.h"
#include "acpi.h"
#include "apic.h"
#include "atapi.h"
#include "boot_log.h"
#include "disk.h"
#include "disk_io_daemon.h"
#include "dma.h"
#include "drawing.h"
#include "driver_system.h"
#include "fat32.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "log.h"
#include "mmu.h"
#include "module_loader.h"
#include "partition.h"
#include "partition_manager.h"
#include "pci.h"
#include "sata_disk.h"
#include "serial.h"
#include "task.h"
#include "task_utils.h"
#include "terminal.h"
#include "tmpfs.h"
#include "vfs.h"

// Global definition of BootInfo.
BootInfo boot_info;
uint32_t *g_framebuffer = NULL;
uint32_t g_pitch_pixels = 0;
uint32_t g_screen_width = 0;
uint32_t g_screen_height = 0;
Terminal main_terminal;
disk_t main_disk;
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

// Función de apagado del sistema operativo
void shutdown(void) {
  terminal_printf(&main_terminal, "\n\nSystem shutdown initiated\r\n");
  serial_write_string(COM1_BASE, "System shutdown initiated\r\n");
  boot_log_init();
  boot_log_start("Initializing System Shutdown");
  boot_log_info("Disabling Interrupts...");
  // 1. Deshabilitar interrupciones
  __asm__ volatile("cli");
  boot_log_ok();
  boot_log_info("Disabling Multitasking System...");
  // 2. Detener el scheduler
  if (scheduler.scheduler_enabled) {
    scheduler_stop();
    boot_log_ok();
  }
  boot_log_info("Terminating tasks");
  // 3. Terminar todas las tareas (excepto idle)
  task_t *current = scheduler.task_list;
  if (current) {
    do {
      task_t *next = current->next;
      if (current != scheduler.idle_task) {
        boot_log_warn("Terminating task %s (ID: %u)\r\n", current->name,
                      current->task_id);
        task_destroy(current);
      }
      current = next;
    } while (current != scheduler.task_list);
  }
  boot_log_ok();
  boot_log_info("Cleaning tasks");
  task_cleanup_zombies();
  boot_log_ok();
  // 4. Limpiar sistema de drivers
  boot_log_info("Cleaning drivers");
  driver_system_cleanup();
  boot_log_ok();
  // 5. Desmontar todos los sistemas de archivos
  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (mount_table[i] && mount_points[i][0]) {
      boot_log_warn("Unmounting %s\r\n", mount_points[i]);
      close_fds_for_mount(mount_table[i]);
      vfs_unmount(mount_points[i]);
    }
  }
  boot_log_ok();
  // 6. Limpiar módulos
  boot_log_info("Cleaning modules");
  module_loader_cleanup();
  boot_log_ok();
  boot_log_info("Disabling PICs");
  // 7. Deshabilitar PICs
  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);
  boot_log_ok();
  // 8. Reportar estadísticas finales del heap
  boot_log_info("Cleaning DMA buffers");
  dma_cleanup();
  boot_log_ok();
  heap_info_t heap_info = heap_stats();
  boot_log_info("Final heap stats: used=%u bytes, free=%u bytes\r\n",
                heap_info.used, heap_info.free);
  // 9. Intentar apagado ACPI
  boot_log_info("Trying ACPI Shutdown...");
  if (acpi_is_supported()) {
    boot_log_ok();
    acpi_power_off();
  }
  boot_log_error();
  boot_log_error();
  boot_log_error();
  boot_log_info("Failed to power off");
  boot_log_error();
  boot_log_error();
  boot_log_error();
  boot_log_warn("System halted. Safe to power off.\r\n");
  boot_log_finish();
  terminal_destroy(&main_terminal);
  // Halt final
  while (1) {
    __asm__ volatile("cli; hlt");
  }
}

void initialize_acpi_pci(void) {
  boot_log_start("Initializing PCI subsystem");
  // Mapear regiones críticas para ACPI
  if (!mmu_is_mapped(0x040E)) {
    boot_log_info("Mapping EBDA pointer region");
    mmu_map_region(0x0400, 0x0400, 0x100, PAGE_PRESENT | PAGE_RW);
  }
  if (!mmu_is_mapped(0x80000)) {
    boot_log_info("Mapping EBDA region");
    mmu_map_region(0x80000, 0x80000, 0x20000, PAGE_PRESENT | PAGE_RW);
  }
  if (!mmu_is_mapped(0xE0000)) {
    boot_log_info("Mapping BIOS ROM region");
    mmu_map_region(0xE0000, 0xE0000, 0x20000, PAGE_PRESENT | PAGE_RW);
  }
  // Inicializar PCI
  pci_init();
  boot_log_ok();
  // Inicializar ACPI
  boot_log_start("Initializing ACPI subsystem");
  acpi_init();
  if (acpi_is_supported()) {
    if (acpi_enable()) {
      boot_log_info("ACPI enabled successfully");
      boot_log_ok();
    } else {
      boot_log_info("ACPI available but not enabled");
      boot_log_error();
    }
  } else {
    boot_log_info("ACPI not supported");
    boot_log_error();
  }
  // NUEVO: Inicializar APIC si está disponible
  boot_log_start("Initializing APIC subsystem");
  if (apic_init()) {
    boot_log_info("APIC initialized successfully");
    // Configurar IRQs del I/O APIC
    irq_setup_apic();
    boot_log_ok();
  } else {
    boot_log_info("APIC not available, using legacy PIC");
    boot_log_error();
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
  // 1. Inicializar memoria
  if (boot_info.mmap) {
    pmem_init(boot_info.mmap);
    exclude_kernel_heap_from_regions(kernel_heap, STATIC_HEAP_SIZE);
    boot_log_ok();
  } else {
    boot_log_error();
  }
  mmu_init();
  // ============================================
  // FASE DE BOOT CON MENSAJES FORMATEADOS
  // ============================================
  size_t heap_size = STATIC_HEAP_SIZE;
  heap_init(kernel_heap, heap_size);
  // Inicializar framebuffer básico
  fb_init(g_framebuffer, width, height, pitch, bpp);
  terminal_init(&main_terminal);
  // boot_log_init();
  // 2. Inicializar GDT, IDT
  boot_log_start("Initializing GDT");
  gdt_init();
  boot_log_ok();
  boot_log_start("Initializing IDT");
  idt_init();
  boot_log_ok();
  boot_log_ok();
  boot_log_start("Initializing PIT timer (for calibration)");
  // Inicializar PIT a 100Hz temporalmente
  uint32_t divisor = 1193180 / 100;
  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
  boot_log_ok();
  // 3. Inicializar teclado
  boot_log_start("Initializing keyboard");
  keyboard_init();
  boot_log_ok();
  // 4. Habilitar interrupciones
  boot_log_start("Enabling interrupts");
  asm volatile("sti");
  boot_log_ok();
  // 5. Inicializar serial
  boot_log_start("Initializing serial ports");
  serial_init();
  boot_log_ok();
  // 6. Inicializar VFS
  boot_log_start("Initializing VFS");
  vfs_init();
  vfs_register_fs(&tmpfs_type);
  vfs_register_fs(&fat32_fs_type);
  boot_log_ok();
  // 7. Inicializar ACPI/PCI/APIC (MODIFICADO)
  initialize_acpi_pci();
  // NUEVO: Ahora sí inicializar el timer (usará APIC si está disponible)
  boot_log_start("Initializing timer");
  pit_init(100); // Esto usará APIC timer si está disponible
  boot_log_ok();
  // disk_scan_all_buses(); // Moved to later in boot sequence
  // 8. Inicializar SATA/AHCI
  boot_log_start("Initializing SATA/AHCI subsystem");
  bool sata_available = false;
  if (sata_disk_init()) {
    sata_available = true;
    boot_log_info("Found %u SATA disk(s)", sata_disk_get_count());
    boot_log_ok();
  } else {
    boot_log_info("SATA/AHCI not available");
    boot_log_error();
  }
  // 9. Inicializar ATAPI
  boot_log_start("Initializing ATAPI subsystem");
  bool atapi_available = false;
  if (atapi_init()) {
    atapi_available = true;
    boot_log_info("Found %u ATAPI device(s)", atapi_get_device_count());
    boot_log_ok();
  } else {
    boot_log_info("ATAPI not available");
    boot_log_error();
  }
  // 10. Montar sistemas de archivos
  bool home_mounted = false;
  boot_log_start("Mounting root filesystem");
  if (vfs_mount("/", "tmpfs", NULL) == VFS_OK) {
    boot_log_ok();
  } else {
    boot_log_error();
  }
  boot_log_start("Mounting /dev filesystem");
  if (vfs_mount("/dev", "tmpfs", NULL) == VFS_OK) {
    boot_log_ok();
  } else {
    boot_log_error();
  }
  boot_log_start("Mounting /ramfs filesystem");
  if (vfs_mount("/ramfs", "tmpfs", NULL) == VFS_OK) {
    boot_log_ok();
  } else {
    boot_log_error();
  }
  // Intentar montar disco persistente
  // Intentar montar disco persistente
  boot_log_start("Searching for persistent storage");
  bool disk_hardware_initialized = false;
  // Probar SATA con soporte multi-disco
  if (sata_available) {
    disk_t *sata_disk = (disk_t *)kernel_malloc(sizeof(disk_t));
    if (sata_disk && sata_to_legacy_disk_init(sata_disk, 0) == DISK_ERR_NONE) {
      disk_hardware_initialized = true;
      memcpy(&main_disk, sata_disk, sizeof(disk_t));
      boot_log_info("SATA disk 0 initialized as main disk");
      if (vfs_mount("/home", "fat32", &main_disk) == VFS_OK) {
        boot_log_info("Mounted FAT32 (SATA)");
        home_mounted = true;
      } else {
        // Try identifying partitions
        partition_table_t pt;
        if (partition_read_table(&main_disk, &pt) == PART_OK) {
          for (int i = 0; i < 4; i++) {
            if (partition_is_fat(pt.partitions[i].type)) {
              disk_t *part_disk = (disk_t *)kernel_malloc(sizeof(disk_t));
              if (disk_init_from_partition(part_disk, &main_disk,
                                           &pt.partitions[i]) ==
                  DISK_ERR_NONE) {
                if (vfs_mount("/home", "fat32", part_disk) == VFS_OK) {
                  boot_log_info("Mounted FAT32 partition %d", i);
                  home_mounted = true;
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
  if (!home_mounted && !disk_hardware_initialized) {
    boot_log_info("Trying IDE disk");
    disk_err_t ide_init = disk_init(&main_disk, 0);
    if (ide_init == DISK_ERR_NONE && main_disk.initialized) {
      disk_hardware_initialized = true;
      boot_log_info("IDE disk hardware initialized (drive_number=0x%02x, "
                    "sector_count=%llu)",
                    main_disk.drive_number, main_disk.sector_count);
      uint8_t *test_buffer = (uint8_t *)kernel_malloc(512);
      if (test_buffer &&
          disk_read_dispatch(&main_disk, 0, 1, test_buffer) == DISK_ERR_NONE) {
        if (check_fat32_signature(test_buffer)) {
          if (vfs_mount("/home", "fat32", &main_disk) == VFS_OK) {
            boot_log_info("Mounted FAT32 (IDE)");
            home_mounted = true;
          }
        } else {
          boot_log_warn("No FAT32 signature on IDE disk, but hardware ready "
                        "for formatting.");
        }
        kernel_free(test_buffer);
      } else {
        boot_log_warn("Failed to read IDE disk boot sector.");
      }
    } else {
      boot_log_warn("IDE disk init failed (error %d).", ide_init);
    }
  }
  // Fallback a ATAPI
  if (!home_mounted && !disk_hardware_initialized && atapi_available &&
      atapi_get_device_count() > 0) {
    boot_log_info("Trying ATAPI device");
    if (disk_init_atapi(&main_disk, 0) == DISK_ERR_NONE &&
        main_disk.initialized) {
      disk_hardware_initialized = true;
      boot_log_info(
          "ATAPI device initialized (drive_number=0x%02x, sector_count=%llu)",
          main_disk.drive_number, main_disk.sector_count);
      if (disk_atapi_media_present(&main_disk)) {
        uint8_t *test_buffer = (uint8_t *)kernel_malloc(512);
        if (test_buffer && disk_read_dispatch(&main_disk, 0, 1, test_buffer) ==
                               DISK_ERR_NONE) {
          if (check_fat32_signature(test_buffer)) {
            if (vfs_mount("/home", "fat32", &main_disk) == VFS_OK) {
              boot_log_info("Mounted FAT32 (ATAPI, read-only)");
              home_mounted = true;
            }
          } else {
            boot_log_warn("No FAT32 signature on ATAPI device, but hardware "
                          "ready (read-only).");
          }
          kernel_free(test_buffer);
        } else {
          boot_log_warn("Failed to read ATAPI boot sector.");
        }
      } else {
        boot_log_warn("No media present in ATAPI device.");
      }
    } else {
      boot_log_warn("ATAPI init failed.");
    }
  }
  // Fallback final a tmpfs
  if (!home_mounted) {
    boot_log_info("Using tmpfs for /home");
    vfs_mount("/home", "tmpfs", NULL);
    if (disk_hardware_initialized) {
      boot_log_info("Disk hardware available but not formatted. Use 'format' "
                    "command to initialize.");
    } else {
      boot_log_warn("No disk hardware detected.");
    }
  }
  sata_disk_debug_port(4);
  // 11. Inicializar logging
  boot_log_start("Initializing logging system");
  log_init();
  boot_log_ok();
  // 13. Inicializar drivers
  boot_log_start("Initializing driver system");
  keyboard_set_handler(keyboard_terminal_handler);
  driver_system_init();
  boot_log_ok();
  // 14. Cargar layout de teclado
  boot_log_start("Loading keyboard layout");
  if (keyboard_load_layout("/dev/ES-KBD.KBD", "ES-QWERTY") == 0) {
    keyboard_set_layout("ES-QWERTY");
    boot_log_info("Loaded ES-QWERTY layout");
    boot_log_ok();
  } else {
    boot_log_warn("Using default layout");
  }
  // Inicializar gestor de particiones
  partition_manager_init();
  disk_scan_all_buses();
  disk_list_detected_devices();
  // Escanear disco principal al inicio
  partition_manager_scan_disk(&main_disk, 0);
  // Verificar tabla de particiones
  if (partition_manager_verify_partition_table(0)) {
    terminal_puts(&main_terminal, "Partition table verification: PASSED\r\n");
  } else {
    terminal_puts(&main_terminal, "Partition table verification: FAILED\r\n");
  }
  boot_log_start("Auto-mounting partitions");
  partition_manager_auto_mount_all();
  boot_log_ok();
  // 15. Inicializar mouse
  // boot_log_start("Initializing PS/2 mouse");
  // mouse_init(g_screen_width, g_screen_height);
  // boot_log_ok();
  // Finalizar boot y mostrar mensaje
  boot_log_finish();
  // 12. Inicializar multitarea
  task_init();
  task_t *cleanup =
      task_create("cleanupd", cleanup_task, NULL, TASK_PRIORITY_LOW);
  if (cleanup) {
    boot_log_info("Created cleanup daemon");
  }
  // Crear tarea principal del loop
  task_t *main_loop =
      task_create("main_loop", main_loop_task, NULL, TASK_PRIORITY_HIGH);
  if (!main_loop) {
    terminal_puts(&main_terminal, "FATAL: Failed to create main loop task\n");
    while (1)
      __asm__("hlt");
  }
  // ============================================
  // INICIAR TERMINAL NORMAL
  // ============================================
  // Limpiar pantalla
  set_colors(COLOR_WHITE, COLOR_BLACK);
  // Inicializar terminal
  terminal_puts(&main_terminal, "\n");
  terminal_puts(&main_terminal,
                "===============================================\n");
  terminal_puts(&main_terminal, "       MicroKernel OS - Terminal Mode\n");
  terminal_puts(&main_terminal,
                "===============================================\n");
  terminal_puts(&main_terminal, "\n");
  terminal_printf(&main_terminal, "System ready. Type 'help' for commands.\n");
  terminal_puts(&main_terminal, "\r\n=== Partition Management Demo ===\r\n");
  terminal_puts(&main_terminal, "Starting scheduler...\n");
  task_profiling_enable();
  message_system_init();
  disk_io_daemon_init();
  task_t *mem_defrag =
      task_create("Memory Defrag", memory_defrag_task, NULL, TASK_PRIORITY_LOW);
  if (mem_defrag) {
    boot_log_info("Memory Defrag task created (ID: %u)", mem_defrag->task_id);
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