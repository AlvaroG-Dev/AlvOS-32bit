
#include "acpi.h"
#include "ahci.h"
#include "apic.h"
#include "cpuid.h"
#include "disk.h"
#include "disk_io_daemon.h"
#include "dma.h"
#include "driver_system.h"
#include "fat32.h"
#include "idt.h"
#include "installer.h"
#include "io.h"
#include "irq.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "mini_parser.h"
#include "mmu.h"
#include "module_loader.h"
#include "partition.h"
#include "partition_manager.h"
#include "pci.h"
#include "sata_disk.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "task_utils.h"
#include "text_editor.h"
#include "vfs.h"

extern vfs_file_t *fd_table[VFS_MAX_FDS];
extern vfs_superblock_t *mount_table[VFS_MAX_MOUNTS];
extern char mount_points[VFS_MAX_MOUNTS][VFS_PATH_MAX];
extern ahci_controller_t ahci_controller;

// Tabla de colores ANSI (índice 0-15)
static const uint32_t ansi_colors[16] = {
    COLOR_BLACK,     // 0
    COLOR_RED,       // 1
    COLOR_GREEN,     // 2
    COLOR_YELLOW,    // 3
    COLOR_BLUE,      // 4
    COLOR_MAGENTA,   // 5
    COLOR_CYAN,      // 6
    COLOR_WHITE,     // 7
    COLOR_DARK_GRAY, // 8
    COLOR_RED,       // 9 (bright red)
    COLOR_GREEN,     // 10 (bright green)
    COLOR_YELLOW,    // 11 (bright yellow)
    COLOR_BLUE,      // 12 (bright blue)
    COLOR_MAGENTA,   // 13 (bright magenta)
    COLOR_CYAN,      // 14 (bright cyan)
    COLOR_WHITE      // 15 (bright white)
};

// =================================================================
// FUNCIONES DE CÁLCULO DE DIMENSIONES DE TERMINAL
// =================================================================

uint32_t terminal_calculate_width(void) {
  // Calcular el ancho basado en el framebuffer y la fuente actual
  uint32_t char_width = g_current_font.width + g_current_font.spacing;
  return g_fb.width / char_width;
}

uint32_t terminal_calculate_height(void) {
  // Calcular la altura basada en el framebuffer y la fuente actual
  return g_fb.height / g_current_font.height;
}

void terminal_recalculate_dimensions(Terminal *term) {
  if (!term)
    return;

  term->width = terminal_calculate_width();
  term->height = terminal_calculate_height();
}

int terminal_resize(Terminal *term) {
  if (!term)
    return 0;

  // Calcular nuevas dimensiones
  uint32_t new_width = terminal_calculate_width();
  uint32_t new_height = terminal_calculate_height();

  // Solo redimensionar si hay cambios significativos
  if (new_width == term->width && new_height == term->height) {
    return 1; // Sin cambios
  }

  // Liberar el array de dirty_lines anterior si existe
  if (term->dirty_lines) {
    kernel_free(term->dirty_lines);
  }

  // Crear nuevo array de dirty_lines
  term->dirty_lines = (uint8_t *)kernel_malloc(new_height * sizeof(uint8_t));
  if (!term->dirty_lines) {
    return 0;
  }

  // Calcular nuevas líneas del buffer
  uint32_t new_buffer_lines = new_height * BUFFER_LINE_MULTIPLIER;
  if (new_buffer_lines < MIN_BUFFER_LINES) {
    new_buffer_lines = MIN_BUFFER_LINES;
  } else if (new_buffer_lines > MAX_BUFFER_LINES) {
    new_buffer_lines = MAX_BUFFER_LINES;
  }

  // Redimensionar el buffer circular
  if (!circular_buffer_resize(&term->buffer, new_width, new_buffer_lines)) {
    kernel_free(term->dirty_lines);
    term->dirty_lines = NULL;
    return 0;
  }

  // Actualizar dimensiones
  term->width = new_width;
  term->height = new_height;

  // Inicializar dirty_lines
  memset(term->dirty_lines, 1, new_height);

  // Ajustar cursor si está fuera de límites
  if (term->cursor_x >= term->width) {
    term->cursor_x = term->width - 1;
  }
  if (term->cursor_y >= term->height) {
    term->cursor_y = term->height - 1;
  }

  // Ajustar view_start_line si es necesario
  if (term->view_start_line + term->height > term->buffer.count) {
    if (term->buffer.count > term->height) {
      term->view_start_line = term->buffer.count - term->height;
    } else {
      term->view_start_line = 0;
    }
  }

  return 1;
}

// =================================================================
// FUNCIONES DE VERIFICACIÓN Y SEGURIDAD
// =================================================================

int terminal_verify_memory_access(Terminal *term, uint32_t line_offset) {
  if (!term || !term->buffer.data) {
    return 0;
  }

  if (line_offset >= term->buffer.lines) {
    return 0;
  }

  uint32_t byte_offset = line_offset * term->buffer.width;
  if (byte_offset >= term->buffer.size) {
    return 0;
  }

  // Verificar que la memoria esté mapeada usando la MMU
  uint32_t physical_addr = mmu_virtual_to_physical(
      (uint32_t)((uintptr_t)(term->buffer.data + byte_offset)));
  if (physical_addr == 0) {
    term->page_faults_avoided++;
    return 0;
  }

  return 1;
}

void terminal_safe_memset(char *ptr, char value, size_t size) {
  if (!ptr || size == 0)
    return;

  // Verificar acceso página por página
  char *current = ptr;
  char *end = ptr + size;

  while (current < end) {
    uint32_t physical = mmu_virtual_to_physical((uint32_t)((uintptr_t)current));
    if (physical == 0) {
      // Saltar esta página
      current = (char *)((uintptr_t)((((uintptr_t)current) + PAGE_SIZE) &
                                     ~(PAGE_SIZE - 1)));
      continue;
    }

    // Calcular cuánto podemos escribir en esta página
    uint32_t page_boundary =
        ((uintptr_t)current + PAGE_SIZE) & ~(PAGE_SIZE - 1);
    size_t chunk_size = (size_t)(page_boundary - (uintptr_t)current);
    if (current + chunk_size > end) {
      chunk_size = end - current;
    }

    // Escribir de forma segura
    for (size_t i = 0; i < chunk_size; i++) {
      current[i] = value;
    }

    current += chunk_size;
  }
}

void terminal_safe_memcpy(char *dst, const char *src, size_t size) {
  if (!dst || !src || size == 0)
    return;

  for (size_t i = 0; i < size; i++) {
    // Verificar acceso para cada byte crítico
    if (i % PAGE_SIZE == 0) {
      if (mmu_virtual_to_physical((uint32_t)((uintptr_t)(dst + i))) == 0 ||
          mmu_virtual_to_physical((uint32_t)((uintptr_t)(src + i))) == 0) {
        break;
      }
    }
    dst[i] = src[i];
  }
}

// =================================================================
// FUNCIONES DEL BUFFER CIRCULAR
// =================================================================

int circular_buffer_init(CircularBuffer *cb, uint32_t width,
                         uint32_t buffer_lines) {
  if (!cb)
    return 0;

  // Asignar memoria para el buffer de caracteres
  cb->size = buffer_lines * width;
  cb->data = (char *)kernel_malloc(cb->size);
  if (!cb->data) {
    return 0;
  }

  // Asignar memoria para los atributos de línea
  cb->line_attrs = (uint32_t *)kernel_malloc(buffer_lines * sizeof(uint32_t));
  if (!cb->line_attrs) {
    kernel_free(cb->data);
    cb->data = NULL;
    return 0;
  }

  // Inicializar el buffer con espacios
  terminal_safe_memset(cb->data, ' ', cb->size);
  memset(cb->line_attrs, 0, buffer_lines * sizeof(uint32_t));

  cb->lines = buffer_lines;
  cb->width = width;
  cb->head = 0;
  cb->tail = 0;
  cb->count = 0;
  cb->wrapped = 0;

  return 1;
}

void circular_buffer_destroy(CircularBuffer *cb) {
  if (!cb)
    return;

  if (cb->data) {
    kernel_free(cb->data);
    cb->data = NULL;
  }

  if (cb->line_attrs) {
    kernel_free(cb->line_attrs);
    cb->line_attrs = NULL;
  }

  cb->size = 0;
  cb->lines = 0;
  cb->width = 0;
  cb->head = 0;
  cb->tail = 0;
  cb->count = 0;
  cb->wrapped = 0;
}

int circular_buffer_resize(CircularBuffer *cb, uint32_t new_width,
                           uint32_t new_buffer_lines) {
  if (!cb)
    return 0;

  // Salvar datos existentes si los hay
  char *old_data = cb->data;
  uint32_t *old_line_attrs = cb->line_attrs;
  uint32_t old_width = cb->width;
  uint32_t old_lines = cb->lines;
  uint32_t old_count = cb->count;
  uint32_t old_head = cb->head;
  uint32_t old_tail = cb->tail;
  uint8_t old_wrapped = cb->wrapped;

  // Crear nuevo buffer
  uint32_t new_size = new_buffer_lines * new_width;
  char *new_data = (char *)kernel_malloc(new_size);
  if (!new_data) {
    return 0;
  }

  uint32_t *new_line_attrs =
      (uint32_t *)kernel_malloc(new_buffer_lines * sizeof(uint32_t));
  if (!new_line_attrs) {
    kernel_free(new_data);
    return 0;
  }

  // Inicializar nuevo buffer
  terminal_safe_memset(new_data, ' ', new_size);
  memset(new_line_attrs, 0, new_buffer_lines * sizeof(uint32_t));

  // Copiar datos existentes si hay alguno
  if (old_data && old_count > 0) {
    uint32_t lines_to_copy =
        (old_count > new_buffer_lines) ? new_buffer_lines : old_count;
    uint32_t chars_to_copy_per_line =
        (old_width > new_width) ? new_width : old_width;

    for (uint32_t i = 0; i < lines_to_copy; i++) {
      // Calcular índice de línea original
      uint32_t old_line_idx;
      if (old_wrapped) {
        old_line_idx = (old_tail + (old_count - lines_to_copy + i)) % old_lines;
      } else {
        old_line_idx = old_count - lines_to_copy + i;
      }

      // Copiar datos de línea
      char *old_line_start = old_data + (old_line_idx * old_width);
      char *new_line_start = new_data + (i * new_width);
      terminal_safe_memcpy(new_line_start, old_line_start,
                           chars_to_copy_per_line);

      // Copiar atributos
      new_line_attrs[i] = old_line_attrs[old_line_idx];
    }

    // Actualizar estado del buffer
    cb->count = lines_to_copy;
    cb->head = lines_to_copy % new_buffer_lines;
    cb->tail = 0;
    cb->wrapped = (lines_to_copy == new_buffer_lines) ? 1 : 0;
  } else {
    cb->count = 0;
    cb->head = 0;
    cb->tail = 0;
    cb->wrapped = 0;
  }

  // Actualizar buffer
  cb->data = new_data;
  cb->line_attrs = new_line_attrs;
  cb->size = new_size;
  cb->lines = new_buffer_lines;
  cb->width = new_width;

  // Liberar memoria anterior
  if (old_data) {
    kernel_free(old_data);
  }
  if (old_line_attrs) {
    kernel_free(old_line_attrs);
  }

  return 1;
}

int circular_buffer_add_line(CircularBuffer *cb) {
  if (!cb || !cb->data)
    return 0;

  // Verificar acceso a la memoria antes de usarla
  if (!terminal_verify_memory_access(
          (Terminal *)((char *)cb - offsetof(Terminal, buffer)), cb->head)) {
    return 0;
  }

  // Limpiar la nueva línea
  uint32_t line_start = cb->head * cb->width;
  terminal_safe_memset(&cb->data[line_start], ' ', cb->width);
  cb->line_attrs[cb->head] = 0;

  // Avanzar head
  cb->head = (cb->head + 1) % cb->lines;

  // Si el buffer está lleno, avanzar tail
  if (cb->count == cb->lines) {
    cb->tail = (cb->tail + 1) % cb->lines;
    cb->wrapped = 1;
  } else {
    cb->count++;
  }

  return 1;
}

char *circular_buffer_get_line(CircularBuffer *cb, uint32_t line_offset) {
  if (!cb || !cb->data)
    return NULL;

  if (line_offset >= cb->count)
    return NULL;

  uint32_t actual_line;
  if (cb->wrapped) {
    actual_line = (cb->tail + line_offset) % cb->lines;
  } else {
    actual_line = line_offset;
  }

  // Verificar acceso a la memoria
  if (!terminal_verify_memory_access(
          (Terminal *)((char *)cb - offsetof(Terminal, buffer)), actual_line)) {
    return NULL;
  }

  return &cb->data[actual_line * cb->width];
}

uint32_t circular_buffer_get_line_attrs(CircularBuffer *cb,
                                        uint32_t line_offset) {
  if (!cb || !cb->line_attrs)
    return 0;

  if (line_offset >= cb->count)
    return 0;

  uint32_t actual_line;
  if (cb->wrapped) {
    actual_line = (cb->tail + line_offset) % cb->lines;
  } else {
    actual_line = line_offset;
  }

  return cb->line_attrs[actual_line];
}

void circular_buffer_set_line_attrs(CircularBuffer *cb, uint32_t line_offset,
                                    uint32_t attrs) {
  if (!cb || !cb->line_attrs)
    return;

  if (line_offset >= cb->count)
    return;

  uint32_t actual_line;
  if (cb->wrapped) {
    actual_line = (cb->tail + line_offset) % cb->lines;
  } else {
    actual_line = line_offset;
  }

  cb->line_attrs[actual_line] = attrs;
}

int circular_buffer_is_valid_line(CircularBuffer *cb, uint32_t line_offset) {
  if (!cb)
    return 0;
  return line_offset < cb->count;
}

void circular_buffer_clear(CircularBuffer *cb) {
  if (!cb)
    return;

  if (cb->data) {
    terminal_safe_memset(cb->data, ' ', cb->size);
  }

  if (cb->line_attrs) {
    memset(cb->line_attrs, 0, cb->lines * sizeof(uint32_t));
  }

  cb->head = 0;
  cb->tail = 0;
  cb->count = 0;
  cb->wrapped = 0;
}

// =================================================================
// FUNCIONES DEL TERMINAL
// =================================================================

void print_mount_callback(const char *mountpoint, const char *fs_name,
                          void *arg) {
  Terminal *t = (Terminal *)arg;
  terminal_printf(t, "  %s -> %s\r\n", mountpoint, fs_name);
}

// Función callback para buscar filesystems montados en este disco
struct disk_fs_info {
  Terminal *term;
  disk_t *main_disk;
  int fs_found;
} fs_info = {&main_terminal, &main_disk, 0};

void find_fs_callback(const char *mountpoint, const char *fs_name, void *arg) {
  struct disk_fs_info *info = (struct disk_fs_info *)arg;

  // Encontrar el superblock para este mount point
  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(mountpoint, &rel);
  if (!sb)
    return;

  disk_t *mount_disk = (disk_t *)sb->backing_device;
  if (!mount_disk)
    return;

  // Verificar si este disco o partición pertenece al disco principal
  disk_t *target_disk = mount_disk;
  if (mount_disk->is_partition && mount_disk->physical_disk) {
    target_disk = mount_disk->physical_disk;
  }

  if (target_disk != info->main_disk)
    return;

  info->fs_found = 1;

  // Mostrar información básica del mount
  terminal_printf(info->term, "Mount: %s -> %s\r\n", mountpoint, fs_name);
  terminal_printf(info->term, "Filesystem: %s\r\n", sb->fs_name);

  // Si es una partición, mostrar información de offset LBA
  if (mount_disk->is_partition) {
    terminal_printf(info->term, "Mounted on Partition:\r\n");
    terminal_printf(info->term, "  LBA Offset: %llu\r\n",
                    mount_disk->partition_lba_offset);
    terminal_printf(info->term, "  Partition Sector Count: %llu\r\n",
                    mount_disk->sector_count);

    // Nota: La información específica de la partición (type, etc.)
    // debe obtenerse del partition manager o MBR directamente
    terminal_printf(info->term,
                    "  (Para ver detalles de partición use: part info)\r\n");
  }

  // Información específica del sistema de archivos
  if (strcmp(sb->fs_name, "fat32") == 0 && sb->private) {
    fat32_fs_t *fs = (fat32_fs_t *)sb->private;

    terminal_printf(info->term, "FAT32 Details:\r\n");
    terminal_printf(info->term, "  Bytes per Sector: %u\r\n",
                    fs->boot_sector.bytes_per_sector);
    terminal_printf(info->term, "  Sectors per Cluster: %u\r\n",
                    fs->boot_sector.sectors_per_cluster);

    // Calcular cluster size
    uint32_t cluster_size =
        fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    terminal_printf(info->term, "  Cluster Size: %u bytes\r\n", cluster_size);

    // Mostrar información del volumen si está disponible
    if (fs->boot_sector.volume_label[0] != ' ' &&
        fs->boot_sector.volume_label[0] != 0) {
      char label[12] = {0};
      // Copiar label asegurando terminación nula
      for (int i = 0; i < 11; i++) {
        label[i] = fs->boot_sector.volume_label[i];
        if (label[i] == ' ' || label[i] == 0) {
          label[i] = 0;
          break;
        }
      }
      terminal_printf(info->term, "  Volume Label: %s\r\n", label);
    }

    // Mostrar tipo de sistema de archivos
    if (fs->boot_sector.fs_type[0] != 0) {
      char fstype[9] = {0};
      strncpy(fstype, (char *)fs->boot_sector.fs_type, 8);
      terminal_printf(info->term, "  FS Type Field: %s\r\n", fstype);
    }

    // Intentar calcular espacio si tenemos la información necesaria
    if (fs->boot_sector.sectors_per_fat_32 > 0 &&
        fs->boot_sector.total_sectors_32 > 0) {
      // Cálculo aproximado de espacio
      uint32_t data_sectors =
          fs->boot_sector.total_sectors_32 - fs->boot_sector.reserved_sectors -
          (fs->boot_sector.num_fats * fs->boot_sector.sectors_per_fat_32);

      uint32_t clusters = data_sectors / fs->boot_sector.sectors_per_cluster;

      if (clusters > 0) {
        uint64_t total_bytes = (uint64_t)clusters * cluster_size;
        terminal_printf(info->term, "  Approximate Capacity: %llu bytes\r\n",
                        total_bytes);
      }
    }

  } else {
    // Para otros sistemas de archivos, mostrar información básica
    terminal_printf(info->term, "Filesystem Details:\r\n");
    terminal_printf(info->term, "  Type: %s\r\n", sb->fs_name);

    if (sb->flags & VFS_MOUNT_RDONLY) {
      terminal_printf(info->term, "  Access: Read-Only\r\n");
    } else {
      terminal_printf(info->term, "  Access: Read-Write\r\n");
    }
  }

  terminal_printf(info->term, "----------------------------------------\r\n");
}

static void resolve_relative_path(Terminal *term, const char *input,
                                  char *full_path) {
  if (!input || input[0] == '\0') {
    // Sin argumento, usar CWD
    strncpy(full_path, term->cwd, VFS_PATH_MAX);
    full_path[VFS_PATH_MAX - 1] = '\0';
  } else if (input[0] == '/') {
    // Path absoluto
    strncpy(full_path, input, VFS_PATH_MAX);
    full_path[VFS_PATH_MAX - 1] = '\0';
  } else {
    // Path relativo
    snprintf(full_path, VFS_PATH_MAX, "%s/%s", term->cwd, input);
  }

  // Normalizar el path resultante
  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(full_path, normalized, VFS_PATH_MAX) == VFS_OK) {
    strncpy(full_path, normalized, VFS_PATH_MAX);
    full_path[VFS_PATH_MAX - 1] = '\0';
  }
}

static void terminal_update_prompt(Terminal *term) {
  char prompt[64] = {0};
  snprintf(prompt, sizeof(prompt), "%s> ", term->cwd);
  uint8_t old_echo = term->echo;
  term->echo = 1;
  terminal_puts(term, prompt);
  term->echo = old_echo;
}

static void mounts_callback(const char *mountpoint, const char *fs_name,
                            void *arg) {
  Terminal *term = (Terminal *)arg;
  terminal_printf(term, "  %s -> %s\r\n", mountpoint, fs_name);
}

static void terminal_clear_current_line(Terminal *term) {
  if (!term)
    return;

  // Guardar posición original del cursor
  uint32_t original_cursor_x = term->cursor_x;
  uint32_t original_cursor_y = term->cursor_y;

  // Mover cursor al inicio de la línea
  term->cursor_x = 0;

  // Obtener la línea actual del buffer circular
  uint32_t buffer_line = term->view_start_line + term->cursor_y;

  // Limpiar la línea en el buffer circular
  if (buffer_line < term->buffer.count) {
    char *line = circular_buffer_get_line(&term->buffer, buffer_line);
    if (line) {
      terminal_safe_memset(line, ' ', term->width);

      // También limpiar atributos si es necesario
      circular_buffer_set_line_attrs(&term->buffer, buffer_line, 0);
    }
  }

  // Borrar línea completa en pantalla
  fill_rect(0, term->cursor_y * g_current_font.height,
            term->width * (g_current_font.width + g_current_font.spacing),
            g_current_font.height, term->bg_color);

  // Restaurar posición del cursor (pero mantener X en 0 para nueva escritura)
  term->cursor_x = 0; // Siempre empezar desde el inicio
  term->cursor_y = original_cursor_y;

  // Marcar la línea como sucia para forzar redibujado
  if (term->cursor_y < term->height) {
    term->dirty_lines[term->cursor_y] = 1;
  }

  // Forzar actualización del cursor
  term->cursor_state_changed = 1;
}

void terminal_set_cursor(Terminal *term, uint32_t x, uint32_t y) {
  if (!term)
    return;
  if (x >= term->width)
    x = term->width - 1;
  if (y >= term->height)
    y = term->height - 1;

  term->cursor_x = x;
  term->cursor_y = y;
  term->cursor_state_changed = 1;

  if (y < term->height) {
    term->dirty_lines[y] = 1;
  }
}

void terminal_set_color(Terminal *term, uint32_t fg, uint32_t bg) {
  if (!term)
    return;

  term->fg_color = fg;
  term->bg_color = bg;
  term->current_fg_color = fg;
  term->current_bg_color = bg;

  // También actualizar los colores globales de drawing
  set_colors(fg, bg);
}

void terminal_show_cursor(Terminal *term, bool show) {
  if (!term)
    return;

  term->cursor_visible = show ? 1 : 0;
  term->cursor_state_changed = 1;

  if (term->cursor_y < term->height) {
    term->dirty_lines[term->cursor_y] = 1;
  }
}

uint32_t terminal_get_cursor_x(Terminal *term) {
  if (!term)
    return 0;
  return term->cursor_x;
}

uint32_t terminal_get_cursor_y(Terminal *term) {
  if (!term)
    return 0;
  return term->cursor_y;
}

void terminal_init(Terminal *term) {
  if (!term)
    return;

  memset(term, 0, sizeof(Terminal));

  // Calcular dimensiones iniciales
  term->width = terminal_calculate_width();
  term->height = terminal_calculate_height();

  // Calcular líneas del buffer basado en la altura
  uint32_t buffer_lines = term->height * BUFFER_LINE_MULTIPLIER;
  if (buffer_lines < MIN_BUFFER_LINES) {
    buffer_lines = MIN_BUFFER_LINES;
  } else if (buffer_lines > MAX_BUFFER_LINES) {
    buffer_lines = MAX_BUFFER_LINES;
  }

  // Inicializar el buffer circular
  if (!circular_buffer_init(&term->buffer, term->width, buffer_lines)) {
    // Error crítico: no se pudo inicializar el buffer
    while (1)
      __asm__ volatile("hlt");
  }

  // Crear array de dirty_lines
  term->dirty_lines = (uint8_t *)kernel_malloc(term->height * sizeof(uint8_t));
  if (!term->dirty_lines) {
    circular_buffer_destroy(&term->buffer);
    while (1)
      __asm__ volatile("hlt");
  }

  // Configurar parámetros del terminal
  term->fg_color = COLOR_WHITE;
  term->bg_color = COLOR_BLACK;
  term->default_fg = COLOR_WHITE;
  term->default_bg = COLOR_BLACK;
  term->current_fg_color = COLOR_WHITE;
  term->current_bg_color = COLOR_BLACK;
  term->echo = 1;
  term->cursor_visible = 1;
  term->cursor_blink_rate = 500;
  term->last_blink_time = 0;
  term->cursor_state_changed = 1;
  term->view_offset = 0;
  term->view_start_line = 0;
  strcpy(term->cwd, "/home");

  // Estadísticas
  term->total_lines_written = 0;
  term->page_faults_avoided = 0;

  // Agregar la primera línea al buffer
  circular_buffer_add_line(&term->buffer);

  memset(term->dirty_lines, 1, term->height);
  set_font(FONT_8x16_VGA);
  terminal_resize(term);
  set_colors(term->fg_color, term->bg_color);
  terminal_update_prompt(term);
}

void terminal_destroy(Terminal *term) {
  if (!term)
    return;

  circular_buffer_destroy(&term->buffer);

  if (term->dirty_lines) {
    kernel_free(term->dirty_lines);
    term->dirty_lines = NULL;
  }
}

void terminal_update_cursor_blink(Terminal *term, uint32_t current_time_ms) {
  if (!term)
    return;

  if (current_time_ms - term->last_blink_time >= term->cursor_blink_rate) {
    term->cursor_visible = !term->cursor_visible;
    term->last_blink_time = current_time_ms;
    term->cursor_state_changed = 1;
    if (term->cursor_y < term->height) {
      term->dirty_lines[term->cursor_y] = 1;
    }
  }
}

void terminal_clear(Terminal *term) {
  if (!term)
    return;

  // Limpiar el buffer circular
  circular_buffer_clear(&term->buffer);

  // Agregar primera línea vacía
  circular_buffer_add_line(&term->buffer);

  // Resetear estado del terminal
  term->cursor_x = 0;
  term->cursor_y = 0;
  term->view_offset = 0;
  term->view_start_line = 0;

  // Limpiar buffer de input
  term->input_pos = 0;
  memset(term->input_buffer, 0, sizeof(term->input_buffer));
  term->in_history_mode = 0;

  // Forzar redibujado completo
  memset(term->dirty_lines, 1, term->height);
  term->needs_full_redraw = 1;

  // Limpiar pantalla físicamente
  fill_rect(0, 0, term->width * (g_current_font.width + g_current_font.spacing),
            term->height * g_current_font.height, term->bg_color);

  // Redibujar
  terminal_draw(term);

  // Mostrar nuevo prompt
  terminal_update_prompt(term);
}

void terminal_scroll_to_bottom(Terminal *term) {
  if (!term)
    return;

  // Calcular la posición de scroll para mostrar las líneas más recientes
  if (term->buffer.count > term->height) {
    term->view_start_line = term->buffer.count - term->height;
    term->view_offset = 0;

    // Posicionar el cursor en la última línea visible
    term->cursor_y = term->height - 1;
  } else {
    term->view_start_line = 0;
    term->view_offset = 0;
    term->cursor_y = term->buffer.count - 1;
  }

  memset(term->dirty_lines, 1, term->height);
}

void terminal_putchar(Terminal *term, char c) {
  if (!term)
    return;

  if (!term->echo && c != '\n' && c != '\r' && c != '\b') {
    return;
  }

  if (c == '\n') {
    // Agregar nueva línea al buffer circular
    if (!circular_buffer_add_line(&term->buffer)) {
      return; // Error al agregar línea
    }

    term->total_lines_written++;

    // Mover cursor
    term->cursor_x = 0;

    // Si estamos en la parte inferior de la pantalla, hacer scroll
    if (term->cursor_y >= term->height - 1) {
      // Scroll automático si estamos siguiendo el final del buffer
      if (term->view_start_line + term->height >= term->buffer.count - 1) {
        term->view_start_line++;
        memset(term->dirty_lines, 1, term->height);
      }
    } else {
      term->cursor_y++;
    }

    if (term->cursor_y < term->height) {
      term->dirty_lines[term->cursor_y] = 1;
    }
  } else if (c == '\b') {
    return; // El backspace se maneja en terminal_handle_key
  } else if (c == '\r') {
    term->cursor_x = 0;
  } else {
    // Obtener la línea actual del buffer
    uint32_t current_buffer_line = term->view_start_line + term->cursor_y;

    // Asegurar que la línea existe
    while (current_buffer_line >= term->buffer.count) {
      if (!circular_buffer_add_line(&term->buffer)) {
        return;
      }
      term->total_lines_written++;
    }

    char *line = circular_buffer_get_line(&term->buffer, current_buffer_line);
    if (line && term->cursor_x < term->width) {
      line[term->cursor_x] = c;
      if (term->cursor_y < term->height) {
        term->dirty_lines[term->cursor_y] = 1;
      }

      term->cursor_x++;
      if (term->cursor_x >= term->width) {
        terminal_putchar(term, '\n'); // Auto-wrap
      }
    }
  }
}

void terminal_draw_line(Terminal *term, uint32_t screen_y) {
  if (!term || screen_y >= term->height)
    return;

  uint32_t buffer_line = term->view_start_line + screen_y;

  // Borrar línea completa
  fill_rect(0, screen_y * g_current_font.height,
            term->width * (g_current_font.width + g_current_font.spacing),
            g_current_font.height, term->bg_color);

  // Dibujar contenido si la línea existe en el buffer
  if (buffer_line < term->buffer.count) {
    char *line = circular_buffer_get_line(&term->buffer, buffer_line);
    if (line) {
      for (uint32_t x = 0; x < term->width; x++) {
        char c = line[x];
        if (c != ' ' && c != '\0') {
          draw_char_with_shadow(
              x * (g_current_font.width + g_current_font.spacing),
              screen_y * g_current_font.height, c, term->fg_color,
              term->bg_color, COLOR_DARK_GRAY, 0);
        }
      }
    }
  }

  // Dibujar cursor si está en esta línea y es visible
  if (screen_y == term->cursor_y && term->cursor_visible &&
      term->cursor_x < term->width) {
    fill_rect(term->cursor_x * (g_current_font.width + g_current_font.spacing),
              screen_y * g_current_font.height, g_current_font.width,
              g_current_font.height, term->fg_color);
  }

  term->dirty_lines[screen_y] = 0;
}

void terminal_puts(Terminal *term, const char *str) {
  if (!term || !str)
    return;

  while (*str) {
    terminal_putchar(term, *str++);
  }
  terminal_draw(term);
}

void terminal_scroll_up(Terminal *term) {
  if (!term)
    return;

  if (term->view_start_line > 0) {
    term->view_start_line--;
    terminal_safe_memset(term->dirty_lines, 1, term->height);
  }
}

void terminal_scroll_down(Terminal *term) {
  if (!term)
    return;

  if (term->view_start_line + term->height < term->buffer.count) {
    term->view_start_line++;
    terminal_safe_memset(term->dirty_lines, 1, term->height);
  }
}

void terminal_scroll(Terminal *term) {
  if (!term)
    return;
  terminal_scroll_down(term);
}

void terminal_draw(Terminal *term) {
  if (!term)
    return;

  static uint32_t last_cursor_x = 0, last_cursor_y = 0;
  static uint8_t last_cursor_visible = 0;

  // Limpiar pantalla si es necesario (solo para scroll completo)
  if (term->needs_full_redraw) {
    fill_rect(0, 0,
              term->width * (g_current_font.width + g_current_font.spacing),
              term->height * g_current_font.height, term->bg_color);
    term->needs_full_redraw = 0;
    memset(term->dirty_lines, 1, term->height);
  }

  // Redibujar líneas sucias
  for (uint32_t screen_y = 0; screen_y < term->height; screen_y++) {
    if (term->dirty_lines[screen_y]) {
      terminal_draw_line(term, screen_y);
    }
  }

  // MANEJO DEL CURSOR - SIEMPRE VERIFICAR PARPADEO
  int cursor_moved =
      (last_cursor_x != term->cursor_x || last_cursor_y != term->cursor_y);
  int cursor_visibility_changed = (last_cursor_visible != term->cursor_visible);

  if (cursor_moved || cursor_visibility_changed || term->cursor_state_changed ||
      term->cursor_visible != last_cursor_visible) {

    // Borrar cursor anterior solo si estaba visible y en pantalla
    if (last_cursor_visible && last_cursor_y < term->height) {
      uint32_t prev_buffer_line = term->view_start_line + last_cursor_y;

      if (prev_buffer_line < term->buffer.count) {
        char *line = circular_buffer_get_line(&term->buffer, prev_buffer_line);
        if (line && last_cursor_x < term->width) {
          char prev_char = line[last_cursor_x];
          if (prev_char != ' ' && prev_char != '\0') {
            draw_char_with_shadow(
                last_cursor_x * (g_current_font.width + g_current_font.spacing),
                last_cursor_y * g_current_font.height, prev_char,
                term->fg_color, term->bg_color, COLOR_DARK_GRAY, 0);
          } else {
            fill_rect(
                last_cursor_x * (g_current_font.width + g_current_font.spacing),
                last_cursor_y * g_current_font.height, g_current_font.width,
                g_current_font.height, term->bg_color);
          }
        }
      }
    }

    // Dibujar cursor nuevo si está visible y en pantalla
    if (term->cursor_visible && term->cursor_y < term->height &&
        term->cursor_x < term->width) {
      fill_rect(term->cursor_x *
                    (g_current_font.width + g_current_font.spacing),
                term->cursor_y * g_current_font.height, g_current_font.width,
                g_current_font.height, term->fg_color);
    }

    // Actualizar estado anterior
    last_cursor_x = term->cursor_x;
    last_cursor_y = term->cursor_y;
    last_cursor_visible = term->cursor_visible;
    term->cursor_state_changed = 0;
  }
}

void terminal_handle_key(Terminal *term, int key) {
  while (!term) {
    return;
  }

  if (key < 0) { // Special keys
    uint8_t is_scroll_key = 0;
    uint8_t is_edit_key = 0;

    switch (key) {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_HOME:
    case KEY_END:
    case KEY_DELETE:
      is_edit_key = 1;
      break;

    case KEY_PGUP:
    case KEY_PGDOWN:
      is_scroll_key = 1;
      break;

    default:
      break;
    }

    // Manejar teclas de edición (historial, movimiento cursor)
    if (is_edit_key) {
      switch (key) {
      case KEY_UP:
        if (term->history_count > 0) {
          if (!term->in_history_mode) {
            strncpy(term->saved_input, term->input_buffer,
                    sizeof(term->saved_input));
            term->in_history_mode = 1;
            term->current_history = term->history_count;
          }

          if (term->current_history > 0) {
            term->current_history--;
          }

          strncpy(term->input_buffer,
                  term->command_history[term->current_history],
                  sizeof(term->input_buffer));
          term->input_pos = strlen(term->input_buffer);
        }
        break;

      case KEY_DOWN:
        if (term->in_history_mode) {
          if (term->current_history < term->history_count - 1) {
            term->current_history++;
            strncpy(term->input_buffer,
                    term->command_history[term->current_history],
                    sizeof(term->input_buffer));
            term->input_pos = strlen(term->input_buffer);
          } else {
            term->in_history_mode = 0;
            strncpy(term->input_buffer, term->saved_input,
                    sizeof(term->input_buffer));
            term->input_pos = strlen(term->input_buffer);
          }
        }
        break;

      case KEY_LEFT:
        if (term->input_pos > 0) {
          term->input_pos--;
        }
        break;

      case KEY_RIGHT:
        if (term->input_pos < strlen(term->input_buffer)) {
          term->input_pos++;
        }
        break;

      case KEY_HOME:
        term->input_pos = 0;
        break;

      case KEY_END:
        term->input_pos = strlen(term->input_buffer);
        break;

      case KEY_DELETE:
        if (term->input_pos < strlen(term->input_buffer)) {
          memmove(term->input_buffer + term->input_pos,
                  term->input_buffer + term->input_pos + 1,
                  strlen(term->input_buffer) - term->input_pos);
        }
        break;
      }

      // Actualizar solo la línea de input
      terminal_clear_current_line(term);
      terminal_update_prompt(term);
      terminal_puts(term, term->input_buffer);

      // Reposicionar cursor
      uint32_t prompt_len = strlen(term->cwd) + 2; // +2 para "> "
      term->cursor_x = prompt_len + term->input_pos;
      if (term->cursor_y < term->height) {
        term->dirty_lines[term->cursor_y] = 1;
      }

      // Redibujar solo la línea actual
      terminal_draw_line(term, term->cursor_y);
    }

    // Manejar teclas de scroll
    if (is_scroll_key) {
      switch (key) {
      case KEY_PGUP:
        terminal_scroll_up(term);
        break;

      case KEY_PGDOWN:
        terminal_scroll_down(term);
        break;
      }

      // Para scroll, redibujar todo el terminal

      terminal_draw(term);
    }

    return;
  }

  if (key == '\n' || key == '\r') {
    if (term->echo) {
      terminal_putchar(term, '\n');
    }

    // Guardar en historial solo si no está vacío
    if (term->input_pos > 0) {
      uint32_t hist_idx = term->history_count % COMMAND_HISTORY_SIZE;
      strcpy(term->command_history[hist_idx], term->input_buffer);
      if (term->history_count < COMMAND_HISTORY_SIZE) {
        term->history_count++;
      }
      term->current_history = term->history_count;
      terminal_process_command(term);
    }

    // Resetear estado
    term->input_pos = 0;
    memset(term->input_buffer, 0, sizeof(term->input_buffer));
    term->in_history_mode = 0;

    // Limpiar línea antes de mostrar nuevo prompt
    if (term->echo) {
      term->cursor_x = 0;
      fill_rect(0, term->cursor_y * g_current_font.height,
                term->width * (g_current_font.width + g_current_font.spacing),
                g_current_font.height, term->bg_color);
    }

    terminal_update_prompt(term);
  } else if (key == '\b' || key == 127) { // Backspace or DEL
    if (term->input_pos > 0) {
      term->input_pos--;
      term->input_buffer[term->input_pos] = '\0';

      if (term->echo) {
        if (term->cursor_x > 0) {
          term->cursor_x--;
        } else if (term->cursor_y > 0) {
          term->cursor_x = term->width - 1;
          term->cursor_y--;
        }

        // Actualizar el buffer circular
        uint32_t current_buffer_line = term->view_start_line + term->cursor_y;
        if (circular_buffer_is_valid_line(&term->buffer, current_buffer_line)) {
          char *line =
              circular_buffer_get_line(&term->buffer, current_buffer_line);
          if (line && term->cursor_x < term->width) {
            line[term->cursor_x] = ' ';
          }
        }

        // Borrar carácter en pantalla
        fill_rect(term->cursor_x *
                      (g_current_font.width + g_current_font.spacing),
                  term->cursor_y * g_current_font.height, g_current_font.width,
                  g_current_font.height, term->bg_color);

        if (term->cursor_y < term->height) {
          term->dirty_lines[term->cursor_y] = 1;
        }
      }
    }
  } else if (key == 0x1B) { // ESC
    while (term->input_pos > 0) {
      terminal_handle_key(term, '\b');
    }
    memset(term->input_buffer, 0, sizeof(term->input_buffer));
    term->in_history_mode = 0;
  } else if (key >= 32 && key <= 255) { // Printable chars, including Latin-1
    if (term->input_pos < sizeof(term->input_buffer) - 1) {
      if (term->in_history_mode) {
        term->in_history_mode = 0;
      }
      term->input_buffer[term->input_pos++] = (char)key;
      term->input_buffer[term->input_pos] = '\0';

      if (term->echo) {
        terminal_putchar(term, (char)key);
      }
    }
  }
}

void terminal_process_command(Terminal *term) {
  if (!term)
    return;

  if (strlen(term->input_buffer) == 0) {
    return;
  }

  terminal_execute(term, term->input_buffer);
  terminal_update_prompt(term);
}

void terminal_printf(Terminal *term, const char *format, ...) {
  char buffer[1024]; // Buffer en stack
  va_list args;

  va_start(args, format);
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  // Si cabe en el buffer estático
  if (len < (int)sizeof(buffer)) {
    terminal_puts(term, buffer);
    return;
  }

  // Manejo de mensajes largos por partes
  const int chunk_size = sizeof(buffer) - 1;
  char *remaining = (char *)format;

  while (*remaining) {
    va_start(args, format);
    len = vsnprintf(buffer, chunk_size + 1, remaining, args);
    va_end(args);

    if (len > chunk_size) {
      // Copiar solo lo que cabe
      buffer[chunk_size] = '\0';
      terminal_puts(term, buffer);
      remaining += chunk_size;
    } else {
      // Imprimir el resto
      terminal_puts(term, buffer);
      break;
    }
  }
}

void terminal_get_stats(Terminal *term, uint32_t *total_lines,
                        uint32_t *valid_lines, uint32_t *buffer_usage) {
  if (!term)
    return;

  if (total_lines)
    *total_lines = term->total_lines_written;
  if (valid_lines)
    *valid_lines = term->buffer.count;
  if (buffer_usage) {
    *buffer_usage = (term->buffer.count * 100) / term->buffer.lines;
  }
}

// =================================================================
// COMANDO DISK - INFORMACIÓN COMPLETA DEL DISCO Y SISTEMA DE ARCHIVOS
// =================================================================

static void show_disk_health(Terminal *term) {
  if (!disk_is_initialized(&main_disk)) {
    terminal_printf(term,
                    "Disk health: NOT AVAILABLE (disk not initialized)\r\n");
    return;
  }

  // Realizar una operación de prueba de lectura
  uint8_t test_buffer[SECTOR_SIZE];
  uint32_t start_ticks = ticks_since_boot;
  uint64_t start_cycles = rdtsc();

  disk_err_t result = disk_read(&main_disk, 0, 1, test_buffer);

  uint32_t end_ticks = ticks_since_boot;
  uint64_t end_cycles = rdtsc();

  uint32_t tick_delta = end_ticks - start_ticks;
  uint64_t cycle_delta = end_cycles - start_cycles;

  terminal_printf(term, "Disk Health Test:\r\n");
  terminal_printf(term, "  Read Test: %s\r\n",
                  result == DISK_ERR_NONE ? "PASSED" : "FAILED");
  terminal_printf(term, "  Response Time: %u ticks, %llu cycles\r\n",
                  tick_delta, cycle_delta);

  if (result == DISK_ERR_NONE) {
    // Verificar si los datos leídos parecen válidos (MBR signature)
    if (test_buffer[510] == 0x55 && test_buffer[511] == 0xAA) {
      terminal_printf(term, "  MBR Signature: VALID\r\n");
    } else {
      terminal_printf(term, "  MBR Signature: INVALID or missing\r\n");
    }
  }
}

static void cmd_disk_info(Terminal *term, const char *args) {
  (void)args; // No se usan argumentos

  terminal_printf(term, "=== DISK INFORMATION ===\r\n");

  // Información básica del disco
  if (!disk_is_initialized(&main_disk)) {
    terminal_printf(term, "Disk: NOT INITIALIZED\r\n");
    return;
  }

  terminal_printf(term, "Disk Status: INITIALIZED\r\n");
  terminal_printf(term, "Drive Number: 0x%02X\r\n", main_disk.drive_number);
  terminal_printf(term, "Sector Count: %llu\r\n",
                  disk_get_sector_count(&main_disk));
  terminal_printf(term, "Total Size: %llu MB\r\n",
                  (disk_get_sector_count(&main_disk) * SECTOR_SIZE) /
                      (1024 * 1024));
  terminal_printf(term, "LBA48 Support: %s\r\n",
                  main_disk.supports_lba48 ? "YES" : "NO");
  terminal_printf(term, "Present: %s\r\n", main_disk.present ? "YES" : "NO");

  // Estadísticas de I/O
  terminal_printf(term, "I/O Statistics:\r\n");
  terminal_printf(term, "  Total I/O Ticks: %u\r\n", disk_get_io_ticks());
  terminal_printf(term, "  Total I/O Cycles: %llu\r\n", disk_get_io_cycles());

  // Información del sistema de archivos montado
  terminal_printf(term, "\n=== FILESYSTEM INFORMATION ===\r\n");

  vfs_list_mounts(find_fs_callback, &fs_info);

  if (!fs_info.fs_found) {
    terminal_printf(term, "No filesystem mounted on this disk\r\n");

    // Mostrar información de partición si está disponible
    terminal_printf(term, "\n=== PARTITION INFORMATION ===\r\n");
    terminal_printf(term, "Partition Table: MBR (Master Boot Record)\r\n");

    // Intentar leer el MBR para mostrar información de particiones
    uint8_t mbr[SECTOR_SIZE];
    if (disk_read(&main_disk, 0, 1, mbr) == DISK_ERR_NONE) {
      // Verificar firma del MBR
      if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        terminal_printf(term, "MBR Signature: VALID (0x55AA)\r\n");

        // Mostrar información de las 4 particiones primarias
        for (int i = 0; i < 4; i++) {
          int offset = 446 + (i * 16);
          uint8_t boot_flag = mbr[offset];
          uint8_t type = mbr[offset + 4];

          if (type != 0) { // Partición no vacía
            uint32_t lba_start = *(uint32_t *)&mbr[offset + 8];
            uint32_t sector_count = *(uint32_t *)&mbr[offset + 12];

            terminal_printf(term, "Partition %d:\r\n", i + 1);
            terminal_printf(term, "  Bootable: %s\r\n",
                            boot_flag == 0x80 ? "YES" : "NO");
            terminal_printf(term, "  Type: 0x%02X\r\n", type);
            terminal_printf(term, "  LBA Start: %u\r\n", lba_start);
            terminal_printf(term, "  Sector Count: %u\r\n", sector_count);
            terminal_printf(term, "  Size: %u MB\r\n",
                            (sector_count * SECTOR_SIZE) / (1024 * 1024));
          }
        }
      } else {
        terminal_printf(term, "MBR Signature: INVALID\r\n");
      }
    } else {
      terminal_printf(term, "Cannot read MBR from disk\r\n");
    }
  }

  // Información de caché del disco (si está disponible)
  terminal_printf(term, "\n=== CACHE INFORMATION ===\r\n");

  if (total_io_ticks > 0) {
    terminal_printf(term, "FAT Cache: ACTIVE\r\n");
    terminal_printf(term, "I/O Efficiency: %.2f cycles/tick\r\n",
                    (double)total_io_cycles / total_io_ticks);
  } else {
    terminal_printf(term, "FAT Cache: INACTIVE\r\n");
    terminal_printf(term, "I/O Efficiency: 0.0 cycles/tick\r\n");
  }

  terminal_printf(term, "================================\r\n");
}

// Función principal del editor que se puede llamar desde el kernel
void cmd_edit(const char *args) {
  if (!args || !args[0]) {
    terminal_printf(&main_terminal, "Uso: edit <archivo>\r\n");
    return;
  }

  // Crear editor
  text_editor_t *editor = editor_create(&main_terminal);
  if (!editor) {
    terminal_printf(&main_terminal, "Error: No se pudo crear el editor\r\n");
    return;
  }

  // Establecer como editor activo
  editor_set_active(editor);

  // Abrir archivo
  if (editor_open_file(editor, args) != 0) {
    // Si falla, es un archivo nuevo
    terminal_printf(&main_terminal, "Creando nuevo archivo: %s\r\n", args);
    task_sleep(1000); // Pausa de 1 segundo
  }

  // Ejecutar editor
  editor_run(editor);

  // Limpiar
  editor_set_active(NULL);
  editor_destroy(editor);

  // Restaurar terminal
  terminal_clear(&main_terminal);
  terminal_printf(&main_terminal, "Editor cerrado.\r\n");
}

// Tarea dedicada al editor (opcional, para ejecutar en background)
void editor_task(void *arg) {
  const char *filename = (const char *)arg;

  if (!filename || !filename[0]) {
    terminal_printf(&main_terminal, "Editor: No se especificó archivo\r\n");
    task_exit(1);
    return;
  }

  text_editor_t *editor = editor_create(&main_terminal);
  if (!editor) {
    terminal_printf(&main_terminal, "Editor: Error al crear instancia\r\n");
    task_exit(1);
    return;
  }

  editor_set_active(editor);

  if (editor_open_file(editor, filename) != 0) {
    terminal_printf(&main_terminal, "Editor: Creando nuevo archivo\r\n");
    task_sleep(500);
  }

  editor_run(editor);

  editor_set_active(NULL);
  editor_destroy(editor);

  terminal_clear(&main_terminal);
  terminal_printf(&main_terminal, "Editor cerrado\r\n");

  task_exit(0);
}

// Crear tarea del editor
task_t *create_editor_task(const char *filename) {
  if (!filename || !filename[0])
    return NULL;

  // Copiar nombre de archivo a memoria permanente
  char *filename_copy = (char *)kernel_malloc(strlen(filename) + 1);
  if (!filename_copy)
    return NULL;

  strcpy(filename_copy, filename);

  task_t *task =
      task_create("editor", editor_task, filename_copy, TASK_PRIORITY_NORMAL);
  if (!task) {
    kernel_free(filename_copy);
    return NULL;
  }

  return task;
}

void cmd_apic_info(void) {
  if (!apic_is_enabled()) {
    terminal_puts(&main_terminal, "APIC is not enabled\r\n");
    return;
  }

  apic_print_info();
}

void cmd_reboot(void) {
  terminal_puts(&main_terminal, "Rebooting system...\r\n");

  if (acpi_is_supported()) {
    acpi_reboot();
  } else {
    terminal_puts(&main_terminal,
                  "ACPI not available, using keyboard controller reset\r\n");

    //// Método tradicional usando el controlador de teclado
    // uint8_t good = 0x02;
    // while (good & 0x02) {
    //     good = inb(0x64);
    // }
    // outb(0x64, 0xFE);
    //
    //// Triple fault como backup
    //__asm__ volatile("cli");
    // struct {
    //    uint16_t limit;
    //    uint32_t base;
    //} __attribute__((packed)) invalid_idt = {0, 0};
    //__asm__ volatile("lidt %0" : : "m"(invalid_idt));
    //__asm__ volatile("int $0x03");
  }
}

void cmd_suspend(void) {
  if (acpi_is_supported()) {
    acpi_suspend();
  } else {
    terminal_puts(&main_terminal,
                  "ACPI not available. Suspend not supported.\r\n");
  }
}

void cmd_lspci(void) {
  if (pci_device_count > 0) {
    pci_list_devices();
  } else {
    terminal_puts(&main_terminal,
                  "No PCI devices found or PCI not initialized\r\n");
  }
}

void cmd_acpi_info(void) {
  if (acpi_is_supported()) {
    acpi_list_tables();
  } else {
    terminal_puts(&main_terminal, "ACPI not supported or not initialized\r\n");
  }
}

void handle_part_command(Terminal *term, int argc, char *argv[]) {
  if (argc < 2) {
    part_help_command(term, "");
    return;
  }

  const char *subcommand = argv[1];

  if (strcmp(subcommand, "scan") == 0) {
    part_scan_command(term, "");
  } else if (strcmp(subcommand, "list") == 0) {
    const char *list_args = (argc > 2) ? argv[2] : "";
    part_list_command(term, list_args);
  } else if (strcmp(subcommand, "info") == 0) {
    char info_args[64] = "";
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        strncat(info_args, " ", sizeof(info_args) - strlen(info_args) - 1);
      strncat(info_args, argv[i], sizeof(info_args) - strlen(info_args) - 1);
    }
    part_info_command(term, info_args);
  } else if (strcmp(subcommand, "create") == 0) {
    if (argc < 6) {
      terminal_puts(term, "part create: Usage: part create <disk> <partition> "
                          "<type> <size> [bootable]\r\n");
      return;
    }
    char create_args[128] = "";
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        strncat(create_args, " ",
                sizeof(create_args) - strlen(create_args) - 1);
      strncat(create_args, argv[i],
              sizeof(create_args) - strlen(create_args) - 1);
    }
    part_create_command(term, create_args);
  } else if (strcmp(subcommand, "delete") == 0) {
    if (argc < 4) {
      terminal_puts(term,
                    "part delete: Usage: part delete <disk> <partition>\r\n");
      return;
    }
    char delete_args[32] = "";
    snprintf(delete_args, sizeof(delete_args), "%s %s", argv[2], argv[3]);
    part_delete_command(term, delete_args);
  } else if (strcmp(subcommand, "space") == 0) {
    char space_args[32] = "";
    if (argc > 2) {
      strncpy(space_args, argv[2], sizeof(space_args) - 1);
    }
    part_space_command(term, space_args);
  } else if (strcmp(subcommand, "fix-order") == 0) {
    char fix_args[32] = "";
    if (argc > 2) {
      strncpy(fix_args, argv[2], sizeof(fix_args) - 1);
    }
    part_fix_order_command(term, fix_args);
  } else if (strcmp(subcommand, "format") == 0) {
    if (argc < 5) {
      terminal_puts(term, "part format: Usage: part format <disk> <partition> "
                          "<fs_type> [label]\r\n");
      return;
    }
    char format_args[128] = "";
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        strncat(format_args, " ",
                sizeof(format_args) - strlen(format_args) - 1);
      strncat(format_args, argv[i],
              sizeof(format_args) - strlen(format_args) - 1);
    }
    part_format_command(term, format_args);
  } else if (strcmp(subcommand, "format-adv") == 0) {
    if (argc < 7) {
      terminal_puts(term, "part format-adv: Usage: part format-adv <disk> "
                          "<partition> <fs_type> <spc> <fats> [label]\r\n");
      return;
    }
    char format_adv_args[128] = "";
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        strncat(format_adv_args, " ",
                sizeof(format_adv_args) - strlen(format_adv_args) - 1);
      strncat(format_adv_args, argv[i],
              sizeof(format_adv_args) - strlen(format_adv_args) - 1);
    }
    part_format_advanced_command(term, format_adv_args);
  } else if (strcmp(subcommand, "mount") == 0) {
    if (argc < 4) {
      terminal_puts(term, "part mount: Usage: part mount <disk> <partition> "
                          "[mount_point] [fs_type]\r\n");
      return;
    }
    char mount_args[128] = "";
    for (int i = 2; i < argc; i++) {
      if (i > 2)
        strncat(mount_args, " ", sizeof(mount_args) - strlen(mount_args) - 1);
      strncat(mount_args, argv[i], sizeof(mount_args) - strlen(mount_args) - 1);
    }
    part_mount_command(term, mount_args);
  } else if (strcmp(subcommand, "auto-mount") == 0) {
    partition_manager_auto_mount_all();
    terminal_puts(term, "Auto-mounted all partitions\r\n");
  } else if (strcmp(subcommand, "help") == 0) {
    part_help_command(term, "");
  } else {
    terminal_printf(term, "part: Unknown subcommand '%s'\r\n", subcommand);
    terminal_puts(term, "Use 'part help' for usage information\r\n");
  }
}

// Comando para listar módulos
void cmd_list_modules(const char *args) {
  (void)args; // No usado
  module_list_all();
}

void terminal_execute(Terminal *term, const char *cmd) {
  char cmd_line[256];
  char command[256];
  char args[256]; // Reintroducir args para compatibilidad
  char *argv[16]; // Support up to 16 arguments
  int argc = 0;

  // Copiar y limpiar la entrada
  strncpy(cmd_line, cmd, sizeof(cmd_line) - 1);
  cmd_line[sizeof(cmd_line) - 1] = '\0';

  // Eliminar espacios iniciales usando un puntero auxiliar
  char *ptr = cmd_line;
  while (*ptr == ' ')
    ptr++;
  // Copiar la línea sin espacios iniciales de vuelta a cmd_line
  memmove(cmd_line, ptr, strlen(ptr) + 1);

  // Eliminar espacios finales
  int len = strlen(cmd_line);
  while (len > 0 && cmd_line[len - 1] == ' ')
    cmd_line[--len] = '\0';

  if (cmd_line[0] == '\0')
    return; // Línea vacía

  // **CORRECCIÓN: Usar strtok_r para evitar problemas con strtok**
  char *saveptr;
  char *token = strtok_r(cmd_line, " ", &saveptr);
  if (token == NULL)
    return; // No hay comando

  // Copiar el comando a la variable command
  strncpy(command, token, sizeof(command) - 1);
  command[sizeof(command) - 1] = '\0';

  // **CORRECCIÓN: Inicializar argc y argv correctamente**
  argc = 0;
  argv[argc++] = command; // argv[0] es el comando

  // **CORRECCIÓN: Procesar todos los tokens restantes**
  while ((token = strtok_r(NULL, " ", &saveptr)) != NULL && argc < 16) {
    argv[argc++] = token;
  }

  // **CORRECCIÓN: Reconstruir args para compatibilidad con código existente**
  args[0] = '\0';
  if (argc > 1) {
    // Reconstruir la cadena de argumentos completa
    for (int i = 1; i < argc; i++) {
      if (i > 1) {
        strncat(args, " ", sizeof(args) - strlen(args) - 1);
      }
      strncat(args, argv[i], sizeof(args) - strlen(args) - 1);
    }
  }

  // Comandos básicos
  if (strcmp(command, "help") == 0) {
    terminal_puts(term, "Available commands:\r\n");
    terminal_puts(term, "help    - Show this help message\r\n");
    terminal_puts(term, "clear   - Clear the terminal\r\n");
    terminal_puts(term, "echo    - Print arguments\r\n");
    terminal_puts(term, "setfg   - Set foreground color (hex)\r\n");
    terminal_puts(term, "setbg   - Set background color (hex)\r\n");
    terminal_puts(term, "heap    - Show heap memory status\r\n");
    terminal_puts(term, "mounts  - Show current FS mounts\r\n");
  } else if (strcmp(command, "clear") == 0) {
    terminal_clear(term);
  }
  if (strcmp(command, "modules") == 0) {
    cmd_list_modules(args);
  }
  if (strcmp(command, "apic") == 0) {
    apic_print_info();
  }
  if (strcmp(command, "lsdrv") == 0) {
    driver_list_all();
  } else if (strcmp(command, "edit") == 0) {
    if (argc < 2) {
      terminal_printf(term, "Uso: edit <archivo>\r\n");
      return;
    }

    // Opción 2: Ejecutar en tarea separada (si prefieres multitarea)
    task_t *editor_task = create_editor_task(argv[1]);
    if (!editor_task) {
      terminal_printf(term, "Error: No se pudo crear tarea del editor\r\n");
    }

    return;
  } else if (strcmp(command, "echo") == 0) {
    terminal_puts(term, args);
    terminal_puts(term, "\r\n");
  } else if (strcmp(command, "setfg") == 0) {
    uint32_t color = strtoul(args, NULL, 16);
    term->fg_color = color;
    set_colors(term->fg_color, term->bg_color);
    terminal_clear(term);
  }

  else if (strcmp(command, "setbg") == 0) {
    uint32_t color = strtoul(args, NULL, 16);
    term->bg_color = color;
    set_colors(term->fg_color, term->bg_color);
    terminal_clear(term);
  } else if (strcmp(cmd, "lspci") == 0) {
    cmd_lspci();
  } else if (strcmp(cmd, "acpi") == 0) {
    cmd_acpi_info();
  } else if (strcmp(cmd, "reboot") == 0) {
    cmd_reboot();
  } else if (strcmp(cmd, "suspend") == 0) {
    cmd_suspend();
  } else if (strcmp(command, "heap") == 0) {
    size_t libre = heap_available();
    terminal_printf(term, "Memoria libre en el heap: %u bytes\n", libre);
  } else if (strcmp(command, "ticks") == 0) {
    terminal_printf(&main_terminal, "Ticks since boot: %u\r\n",
                    ticks_since_boot);
  } else if (strcmp(command, "heaptest") == 0) {
    heap_test_results_t test_results = heap_run_exhaustive_tests();
    heap_print_test_results(&test_results, &main_terminal);
  } else if (strcmp(command, "async_read") == 0) {
    cmd_async_read_test();
  } else if (strcmp(command, "async_write") == 0) {
    cmd_async_write_test();
  } else if (strcmp(command, "defrag") == 0) {
    cmd_force_defrag();
  } else if (strcmp(command, "defrag_stats") == 0) {
    cmd_defrag_stats();
  } else if (strcmp(command, "disk") == 0) {
    if (strcmp(args, "health") == 0) {
      show_disk_health(term);
    } else {
      cmd_disk_info(term, args);
    }
  } else if (strcmp(cmd, "lsblk") == 0) {
    cmd_lsblk();
  } else if (strcmp(command, "format") == 0) {
    int result = fat32_format(&main_disk, "MYOS_DISK");
    if (result == VFS_OK) {
      terminal_printf(&main_terminal,
                      "Disco formateado como FAT32 exitosamente\n");
    } else {
      terminal_printf(&main_terminal, "Error formateando disco: %d\n", result);
    }
  } else if (strcmp(command, "cpuinfo") == 0) {
    if (argc > 1 && strcmp(argv[1], "detailed") == 0) {
      cmd_cpuinfo_detailed(term, "");
    } else {
      cmd_cpuinfo(term, "");
    }
  } else if (strcmp(command, "cpufreq") == 0) {
    // Asegurarse de que las interrupciones estén habilitadas
    __asm__ __volatile__("sti");

    uint32_t start_ticks = ticks_since_boot;
    uint32_t measure_ticks = 5; // medir sobre 5 ticks (~50ms si PIT=100Hz)
    uint32_t timeout_ticks =
        start_ticks + measure_ticks + 10; // margen de seguridad

    uint64_t start_cycles = rdtsc();

    // Esperar a que pasen 'measure_ticks'
    while ((ticks_since_boot - start_ticks) < measure_ticks) {
      if (ticks_since_boot >= timeout_ticks) {
        terminal_printf(&main_terminal, "Error: timeout esperando ticks\r\n");
        return;
      }
      __asm__ __volatile__("pause");
    }

    uint64_t end_cycles = rdtsc();
    uint64_t cycles_total = end_cycles - start_cycles;

    // Calcular frecuencia en MHz
    // ticks/segundo = 100 si PIT = 100Hz, medimos sobre 'measure_ticks'
    uint64_t cycles_per_second = (cycles_total * 100) / measure_ticks;
    uint32_t freq_mhz = (uint32_t)(cycles_per_second / 1000000ULL);

    terminal_printf(&main_terminal, "Estimated CPU freq: %u MHz\r\n", freq_mhz);
  }
  if (strcmp(command, "tasks") == 0) {
    task_list_all();
  } else if (strcmp(command, "task_state") == 0) {
    show_system_stats();
  } else if (strcmp(command, "tstats") == 0) {
    terminal_printf(term, "Terminal Statistics:\r\n");
    terminal_printf(term, "  Total lines written: %u\r\n",
                    term->total_lines_written);
    terminal_printf(term, "  Valid lines in buffer: %u\r\n",
                    term->buffer.count);
    terminal_printf(term, "  Buffer capacity: %u lines\r\n",
                    term->buffer.lines);
    terminal_printf(term, "  Buffer wrapped: %s\r\n",
                    term->buffer.wrapped ? "Yes" : "No");
    terminal_printf(term, "  Page faults avoided: %u\r\n",
                    term->page_faults_avoided);
    terminal_printf(term, "  Current view line: %u\r\n", term->view_start_line);
    terminal_printf(
        term, "  Memory usage: %u KB\r\n",
        (term->buffer.size + term->buffer.lines * sizeof(uint32_t)) / 1024);
  } else if (strcmp(command, "tbuffer") == 0) {
    terminal_printf(term, "Terminal Buffer Info:\r\n");
    terminal_printf(term, "  Head: %u, Tail: %u\r\n", term->buffer.head,
                    term->buffer.tail);
    terminal_printf(term, "  Count: %u/%u\r\n", term->buffer.count,
                    term->buffer.lines);
    terminal_printf(term, "  Buffer size: %u bytes\r\n", term->buffer.size);
    uint32_t usage = (term->buffer.count * 100) / term->buffer.lines;
    terminal_printf(term, "  Usage: %u%%\r\n", usage);
  } else if (strcmp(command, "kill") == 0) {
    // Verificar que haya un ID
    if (args[0] == '\0') {
      terminal_puts(term, "Error: Usage: kill <task_id>\r\n");
      return;
    }
    uint32_t task_id = atoi(args);
    if (task_id == 0 && args[0] != '0') {
      terminal_puts(term, "Error: Invalid task ID\r\n");
      return;
    }
    task_t *task = task_find_by_id(task_id);
    if (task) {
      if (task == task_current()) {
        terminal_puts(term, "Cannot kill current task\r\n");
      } else if (task == scheduler.idle_task) {
        terminal_puts(term, "Cannot kill idle task\r\n");
      } else {
        terminal_printf(term, "Killing task %s (ID: %u)\r\n", task->name,
                        task->task_id);
        task_destroy(task);
      }
    } else {
      terminal_printf(term, "Task with ID %u not found\r\n", task_id);
    }
  } else if (strcmp(command, "yield") == 0) {
    terminal_puts(term, "Yielding CPU...\r\n");
    task_yield();
  } else if (strncmp(command, "sleep ", 6) == 0) {
    uint32_t ms = atoi(command + 6);
    terminal_printf(term, "Sleeping for %u ms...\r\n", ms);
    task_sleep(ms);
  } else if (strcmp(command, "scheduler") == 0) {
    if (scheduler.scheduler_enabled) {
      terminal_puts(term, "Scheduler is ENABLED\r\n");
    } else {
      terminal_puts(term, "Scheduler is DISABLED\r\n");
    }
    terminal_printf(term, "Total context switches: %u\r\n",
                    scheduler.total_switches);
    terminal_printf(term, "Quantum ticks: %u\r\n", scheduler.quantum_ticks);
  } else if (strcmp(command, "start_scheduler") == 0) {
    scheduler_start();
    terminal_puts(term, "Scheduler started\r\n");
  } else if (strcmp(command, "stop_scheduler") == 0) {
    scheduler_stop();
    terminal_puts(term, "Scheduler stopped\r\n");
  } else if (strcmp(command, "task_health") == 0) {
    task_monitor_health();
  } else if (strcmp(command, "install") == 0) {
    install_err_t result = install_os_complete(&main_disk, &options);
    if (result == INSTALL_OK) {
      terminal_puts(&main_terminal, "¡Instalación completa exitosa!");
      // Opcional: Llama a shutdown() después
    } else {
      terminal_printf(&main_terminal, "Instalación fallida con error: %s\n",
                      installer_error_string(result));
    }
  } else if (strcmp(command, "run") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "run: Usage: run <program_file> [task_name]\r\n");
      return;
    }

    char *saveptr;
    char *filename = strtok_r((char *)args, " ", &saveptr);
    char *taskname = strtok_r(NULL, " ", &saveptr);

    if (!taskname) {
      // Generar nombre por defecto
      static uint32_t task_num = 1;
      char default_name[32];
      snprintf(default_name, sizeof(default_name), "prog%d", task_num++);
      taskname = default_name;
    }

    task_t *task = mini_parser_create_task(filename, taskname);
    if (task) {
      terminal_printf(term, "Started program '%s' as task '%s' (ID: %u)\r\n",
                      filename, taskname, task->task_id);
    } else {
      terminal_printf(term, "Failed to load program: %s\r\n", filename);
    }
  } else if (strcmp(command, "list_programs") == 0) {
    // Listar programas disponibles en /bin
    vfs_node_t *bin_dir = resolve_path_to_vnode(NULL, "/bin");
    if (bin_dir && bin_dir->type == VFS_NODE_DIR) {
      vfs_dirent_t dirents[32];
      uint32_t count = 32;

      if (bin_dir->ops->readdir(bin_dir, dirents, &count, 0) == 0) {
        terminal_printf(term, "Available programs in /bin: (%u)\r\n", count);
        for (uint32_t i = 0; i < count; i++) {
          terminal_printf(term, "  %s\r\n", dirents[i].name);
        }
      }

      bin_dir->refcount--;
      if (bin_dir->refcount == 0 && bin_dir->ops->release) {
        bin_dir->ops->release(bin_dir);
      }
    } else {
      terminal_puts(term, "No /bin directory found\r\n");
    }
  } else if (strcmp(command, "help_tasks") == 0) {
    terminal_puts(term, "\r\nTask Management Commands:\r\n");
    terminal_puts(term, "  tasks              - List all tasks\r\n");
    terminal_puts(term, "  task_state              - List all tasks\r\n");
    terminal_puts(term, "  kill <id>          - Kill task by ID\r\n");
    terminal_puts(term, "  yield              - Yield CPU to other tasks\r\n");
    terminal_puts(
        term,
        "  sleep <ms>         - Sleep current task for N milliseconds\r\n");
    terminal_puts(term, "  scheduler          - Show scheduler status\r\n");
    terminal_puts(term, "  start_scheduler    - Start the scheduler\r\n");
    terminal_puts(term, "  stop_scheduler     - Stop the scheduler\r\n");
    terminal_puts(term, "  create_test_task   - Create a new test task\r\n");
    terminal_puts(term, "  task_health        - Show tasks health\r\n\r\n");
    terminal_puts(term, "  help_tasks         - Show this help\r\n\r\n");
  } else if (strcmp(command, "cat") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "cat: Usage: cat <path>\r\n");
      return;
    }
    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    int fd = vfs_open(full_path, VFS_O_RDONLY);
    if (fd < 0) {
      terminal_printf(term, "cat: Failed to open %s, error: %d\r\n", full_path,
                      fd);
      return;
    }

    char buffer[4096];
    int total_read = 0;
    int read_this_time;
    int has_content = 0;

    while ((read_this_time = vfs_read(fd, buffer, sizeof(buffer) - 1)) > 0) {
      total_read += read_this_time;

      // ✅ FILTRAR CARACTERES NO IMPRIMIBLES
      for (int i = 0; i < read_this_time; i++) {
        if (buffer[i] >= 32 &&
            buffer[i] <= 126) { // Caracteres imprimibles ASCII
          terminal_putchar(term, buffer[i]);
        } else if (buffer[i] == '\n' || buffer[i] == '\t' ||
                   buffer[i] == '\r') {
          terminal_putchar(term, buffer[i]); // Caracteres de control útiles
        } else {
          // Mostrar caracteres no imprimibles como hex
          terminal_printf(term, "[0x%02X]", (unsigned char)buffer[i]);
        }
      }
      has_content = 1;
    }

    if (has_content) {
      terminal_puts(term, "\r\n");
    } else if (read_this_time == 0) {
      terminal_printf(term, "%s: empty file\r\n", full_path);
    } else {
      terminal_printf(term, "cat: Read error: %d\r\n", read_this_time);
    }

    vfs_close(fd);
    terminal_printf(term, "Total bytes read: %d\r\n", total_read);
  } else if (strcmp(command, "ls") == 0) {
    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args[0] ? args : "",
                          full_path); // Usa CWD si no hay args

    const char *relpath;
    vfs_superblock_t *sb = find_mount_for_path(full_path, &relpath);
    if (!sb) {
      terminal_printf(term, "ls: No filesystem mounted at %s\r\n", full_path);
      log_message(LOG_ERROR, "ls failed: no mount for %s\n", full_path);
      return;
    }
    vfs_node_t *dir = resolve_path_to_vnode(sb, relpath);
    if (!dir) {
      terminal_printf(term, "ls: Could not resolve directory %s\r\n",
                      full_path);
      log_message(LOG_ERROR, "ls failed: could not resolve %s\n", full_path);
      return;
    }
    if (dir->type != VFS_NODE_DIR) {
      terminal_printf(term, "ls: %s is not a directory\r\n", full_path);
      log_message(LOG_ERROR, "ls failed: %s not a directory\n", full_path);
      dir->refcount--;
      if (dir->refcount == 0 && dir->ops->release) {
        dir->ops->release(dir);
      }
      return;
    }
    vfs_dirent_t dirents[10];
    uint32_t count = 10;
    if (dir->ops->readdir(dir, dirents, &count, 0) == 0) {
      terminal_printf(term, "Directory listing for %s: %u entries\n", full_path,
                      count);
      for (uint32_t i = 0; i < count; i++) {
        terminal_printf(term, "%s (%s)\n", dirents[i].name,
                        dirents[i].type == VFS_NODE_FILE ? "file" : "dir");
      }
    } else {
      terminal_printf(term, "ls: Failed to list directory %s\n", full_path);
      log_message(LOG_ERROR, "ls failed to list %s\n", full_path);
    }
    dir->refcount--;
    if (dir->refcount == 0 && dir->ops->release) {
      dir->ops->release(dir);
    }
  } else if (strcmp(command, "mounts") == 0) {
    int count = vfs_list_mounts(mounts_callback, term);
    terminal_printf(term, "Current mounts (%d):\r\n", count);
    if (count == 0) {
      terminal_puts(term, "  No mounts found\r\n");
    }
  } else if (strcmp(command, "write_test") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "write_test: Usage: write_test <path>\r\n");
      log_message(LOG_ERROR, "write_test command failed: no path provided\n");
      return;
    }
    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    int fd = vfs_open(full_path, VFS_O_WRONLY | VFS_O_CREAT);
    if (fd < 0) {
      terminal_printf(
          term, "write_test: Failed to open %s for writing, error: %d\r\n",
          full_path, fd);
      log_message(LOG_ERROR, "write_test failed to open %s, error: %d\n",
                  full_path, fd);
      return;
    }
    const char *test_data = "Test data for write_test\n";
    int wrote = vfs_write(fd, test_data, strlen(test_data));
    if (wrote < 0) {
      terminal_printf(term, "write_test: Failed to write to %s, error: %d\r\n",
                      full_path, wrote);
      log_message(LOG_ERROR, "write_test failed to write to %s, error: %d\n",
                  full_path, wrote);
    } else {
      terminal_printf(term, "write_test: Wrote %d bytes to %s\r\n", wrote,
                      full_path);
      log_message(LOG_INFO, "write_test wrote %d bytes to %s\n", wrote,
                  full_path);
    }
    vfs_close(fd);
  } else if (strcmp(command, "read_test") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "read_test: Usage: read_test <path>\r\n");
      log_message(LOG_ERROR, "read_test command failed: no path provided\n");
      return;
    }
    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    int fd = vfs_open(full_path, VFS_O_RDONLY);
    if (fd < 0) {
      terminal_printf(term,
                      "read_test: Failed to open %s for reading, error: %d\r\n",
                      full_path, fd);
      log_message(LOG_ERROR, "read_test failed to open %s, error: %d\n",
                  full_path, fd);
      return;
    }
    char buffer[512];
    int read = vfs_read(fd, buffer, sizeof(buffer) - 1);
    if (read < 0) {
      terminal_printf(term, "read_test: Failed to read from %s, error: %d\r\n",
                      full_path, read);
      log_message(LOG_ERROR, "read_test failed to read %s, error: %d\n",
                  full_path, read);
    } else {
      buffer[read] = '\0';
      terminal_printf(term, "read_test: Read %d bytes from %s: %s\r\n", read,
                      full_path, buffer);
      log_message(LOG_INFO, "read_test read %d bytes from %s\n", read,
                  full_path);
    }
    vfs_close(fd);
  } else if (strcmp(command, "cd") == 0) {
    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args[0] ? args : "/",
                          full_path); // Default a root si no hay args

    const char *rel;
    vfs_superblock_t *sb = find_mount_for_path(full_path, &rel);
    if (!sb) {
      terminal_printf(term, "cd: No mount for %s\r\n", full_path);
      log_message(LOG_ERROR, "cd failed: no mount for %s\n", full_path);
      return;
    }

    vfs_node_t *dir = resolve_path_to_vnode(sb, rel);
    if (!dir || dir->type != VFS_NODE_DIR) {
      if (dir) {
        dir->refcount--;
        if (dir->refcount == 0 && dir->ops->release) {
          dir->ops->release(dir);
        }
      }
      terminal_printf(term, "cd: %s is not a directory\r\n", full_path);
      log_message(LOG_ERROR, "cd failed: %s not a directory\n", full_path);
      return;
    }

    // Actualizar CWD con path normalizado
    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path(full_path, normalized, VFS_PATH_MAX) == VFS_OK) {
      strncpy(term->cwd, normalized, VFS_PATH_MAX);
      term->cwd[VFS_PATH_MAX - 1] = '\0';
    } else {
      terminal_printf(term, "cd: Failed to normalize path %s\r\n", full_path);
      log_message(LOG_ERROR, "cd failed to normalize path %s\n", full_path);
      dir->refcount--;
      if (dir->refcount == 0 && dir->ops->release) {
        dir->ops->release(dir);
      }
      return;
    }

    dir->refcount--;
    if (dir->refcount == 0 && dir->ops->release) {
      dir->ops->release(dir);
    }

    log_message(LOG_INFO, "cd successful to %s\n", term->cwd);
  } else if (strcmp(command, "touch") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "touch: Usage: touch <path>\r\n");
      log_message(LOG_ERROR, "touch failed: no path\n");
      return;
    }

    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    int fd = vfs_open(full_path, VFS_O_CREAT);
    if (fd < 0) {
      terminal_printf(term, "touch: Failed to create %s (error %d)\r\n",
                      full_path, fd);
      log_message(LOG_ERROR, "touch failed for %s (error %d)\n", full_path, fd);
      return;
    }

    vfs_close(fd);
    terminal_printf(term, "touch: Created %s\r\n", full_path);
    log_message(LOG_INFO, "touch created %s\n", full_path);
  } else if (strcmp(command, "rm") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "rm: Usage: rm <path>\r\n");
      log_message(LOG_ERROR, "rm failed: no path\n");
      return;
    }

    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    int ret = vfs_unlink(full_path);
    if (ret != VFS_OK) {
      terminal_printf(term, "rm: Failed to remove %s (error %d)\r\n", full_path,
                      ret);
      log_message(LOG_ERROR, "rm failed for %s (error %d)\n", full_path, ret);
      return;
    }

    terminal_printf(term, "rm: Removed %s\r\n", full_path);
    log_message(LOG_INFO, "rm removed %s\n", full_path);
  } else if (strcmp(command, "mkdir") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "mkdir: Usage: mkdir \r\n");
      log_message(LOG_ERROR, "mkdir failed: no path\n");
      return;
    }

    char full_path[VFS_PATH_MAX];
    resolve_relative_path(term, args, full_path);

    vfs_node_t *new_dir = NULL;
    int ret = vfs_mkdir(full_path, &new_dir);
    if (ret != VFS_OK) {
      terminal_printf(term,
                      "mkdir: Failed to create directory %s (error %d)\r\n",
                      full_path, ret);
      log_message(LOG_ERROR, "mkdir failed for %s (error %d)\n", full_path,
                  ret);
      return;
    }

    // Release the new directory node if not needed
    if (new_dir) {
      new_dir->refcount--;
      if (new_dir->refcount == 0 && new_dir->ops->release) {
        new_dir->ops->release(new_dir);
      }
    }

    terminal_printf(term, "mkdir: Created directory %s\r\n", full_path);
    log_message(LOG_INFO, "mkdir created %s\n", full_path);
  } else if (strcmp(command, "umount") == 0) {
    if (args[0] == '\0') {
      terminal_puts(term, "Error: Usage: umount <mountpoint>\r\n");
      return;
    }

    // Normalizar el mountpoint
    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path(args, normalized, VFS_PATH_MAX) != VFS_OK) {
      terminal_printf(term, "umount: Invalid mountpoint path %s\r\n", args);
      return;
    }

    // Llamar directamente a vfs_unmount (ya maneja la búsqueda)
    int ret = vfs_unmount(normalized);
    if (ret != VFS_OK) {
      terminal_printf(term, "umount: Failed to unmount %s (error %d)\r\n",
                      normalized, ret);
      return;
    }

    terminal_printf(term, "umount: Successfully unmounted %s\r\n", normalized);
  }
  if (strcmp(command, "part") == 0) {
    handle_part_command(term, argc, argv);
  } else if (strcmp(command, "shutdown") == 0) {
    terminal_puts(term, "Initiating system shutdown...\r\n");
    serial_write_string(COM1_BASE, "Initiating system shutdown...\r\n");
    shutdown();
  } else if (strcmp(command, "sata") == 0) {
    if (argc > 1) {
      if (strcmp(argv[1], "list") == 0) {
        sata_disk_list();
      } else if (strcmp(argv[1], "test") == 0) {
        if (argc > 2) {
          uint32_t disk_id = atoi(argv[2]);
          if (sata_disk_test(disk_id)) {
            terminal_puts(term, "SATA disk test passed\r\n");
          } else {
            terminal_puts(term, "SATA disk test failed\r\n");
          }
        } else {
          // test_sata_disks();
        }
      } else if (strcmp(argv[1], "info") == 0) {
        if (argc > 2) {
          uint32_t disk_id = atoi(argv[2]);
          sata_disk_t *disk = sata_disk_get_info(disk_id);
          if (disk) {
            terminal_printf(term, "SATA Disk %u:\r\n", disk_id);
            terminal_printf(term, "  Port: %u\r\n", disk->ahci_port);
            terminal_printf(term, "  Model: %s\r\n", disk->model);
            terminal_printf(term, "  Serial: %s\r\n", disk->serial);
            terminal_printf(term, "  Sectors: %llu\r\n", disk->sector_count);
            terminal_printf(term, "  Size: %llu MB\r\n",
                            (disk->sector_count * 512) / (1024 * 1024));
            terminal_printf(term, "  LBA48: %s\r\n",
                            disk->supports_lba48 ? "Yes" : "No");
            terminal_printf(term, "  DMA: %s\r\n",
                            disk->supports_dma ? "Yes" : "No");
            terminal_printf(term, "  NCQ: %s\r\n",
                            disk->supports_ncq ? "Yes" : "No");
            terminal_printf(term, "  Reads: %llu\r\n", disk->read_count);
            terminal_printf(term, "  Writes: %llu\r\n", disk->write_count);
            terminal_printf(term, "  Errors: %llu\r\n", disk->error_count);
          } else {
            terminal_printf(term, "Invalid SATA disk ID: %u\r\n", disk_id);
          }
        } else {
          terminal_puts(term, "Usage: sata info <disk_id>\r\n");
        }
      } else {
        terminal_puts(
            term, "SATA commands: list, test [disk_id], info <disk_id>\r\n");
      }
    } else {
      terminal_printf(term, "SATA disks: %u available\r\n",
                      sata_disk_get_count());
      terminal_puts(term, "Commands: sata list, sata test, sata info <id>\r\n");
    }

  } else if (strcmp(command, "ahci") == 0) {
    if (argc > 1) {
      if (strcmp(argv[1], "list") == 0) {
        ahci_list_devices();
      } else if (strcmp(argv[1], "port") == 0) {
        if (argc > 2) {
          uint8_t port_num = atoi(argv[2]);
          ahci_print_port_status(port_num);
        } else {
          terminal_puts(term, "Usage: ahci port <port_number>\r\n");
        }
      } else {
        terminal_puts(term, "AHCI commands: list, port <num>\r\n");
      }
    } else {
      if (ahci_controller.initialized) {
        terminal_printf(term, "AHCI Controller: %04x:%04x\r\n",
                        ahci_controller.pci_device->vendor_id,
                        ahci_controller.pci_device->device_id);
        terminal_printf(term, "Ports: %u, Slots: %u, 64bit: %s\r\n",
                        ahci_controller.port_count,
                        ahci_controller.command_slots,
                        ahci_controller.supports_64bit ? "Yes" : "No");
      } else {
        terminal_puts(term, "AHCI controller not initialized\r\n");
      }
      terminal_puts(term, "Commands: ahci list, ahci port <num>\r\n");
    }

  } else if (strcmp(command, "dma") == 0) {
    if (argc > 1) {
      if (strcmp(argv[1], "status") == 0) {
        dma_print_status();
      } else if (strcmp(argv[1], "test") == 0) {
        dma_test_basic_transfer();
      } else {
        terminal_puts(term, "DMA commands: status, test\r\n");
      }
    } else {
      dma_print_status();
    }
  } else if (strcmp(command, "disktest") == 0) {
    if (argc > 1 && strcmp(argv[1], "sata") == 0) {
      if (argc > 2) {
        // Test specific disk
        uint32_t disk_id = atoi(argv[2]);
        if (disk_id >= sata_disk_get_count() ||
            !sata_disk_is_present(disk_id)) {
          terminal_printf(term, "Invalid or unavailable disk ID: %u\n",
                          disk_id);
        } else {
          terminal_printf(term, "Testing SATA disk %u...\n", disk_id);
          if (sata_disk_test(disk_id)) {
            terminal_printf(term, "Test for disk %u passed\n", disk_id);
          } else {
            terminal_printf(term, "Test for disk %u failed\n", disk_id);
          }
        }
      } else {
        // Test all disks
        for (uint32_t i = 0; i < sata_disk_get_count(); i++) {
          if (sata_disk_is_present(i)) {
            terminal_printf(term, "Testing SATA disk %u...\n", i);
            if (sata_disk_test(i)) {
              terminal_printf(term, "Test for disk %u passed\n", i);
            } else {
              terminal_printf(term, "Test for disk %u failed\n", i);
            }
          }
        }
      }
    }
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "Command not found: %s\r\n", command);
    terminal_puts(term, msg);
  }
}
