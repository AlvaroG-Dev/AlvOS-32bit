#include "exec.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "memutils.h"
#include "mmu.h"
#include "string.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"

// ============================================================================
// CARGA DE ARCHIVO DESDE DISCO
// ============================================================================

/**
 * Lee un archivo completo desde VFS a un buffer del kernel
 */
static void *load_file_from_disk(const char *path, uint32_t *out_size) {
  if (!path || !out_size) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] ERROR: Invalid parameters" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                  " Loading file: " ANSI_COLOR_YELLOW
                                  "%s" ANSI_COLOR_RESET "\r\n",
                  path);

  // Abrir archivo
  int fd = vfs_open(path, VFS_O_RDONLY);
  if (fd < 0) {
    terminal_printf(&main_terminal,
                    ANSI_COLOR_RED
                    "[EXEC] ERROR: Cannot open file " ANSI_COLOR_YELLOW
                    "%s" ANSI_COLOR_RED " (error %d)" ANSI_COLOR_RESET "\r\n",
                    path, fd);
    return NULL;
  }

  // Leer en chunks hasta EOF
  const uint32_t CHUNK_SIZE = 4096;
  uint32_t total_allocated = CHUNK_SIZE;
  uint32_t total_read = 0;

  char *buffer = (char *)kernel_malloc(total_allocated);
  if (!buffer) {
    terminal_printf(
        &main_terminal, ANSI_COLOR_RED
        "[EXEC] ERROR: Cannot allocate initial buffer" ANSI_COLOR_RESET "\r\n");
    vfs_close(fd);
    return NULL;
  }

  int bytes_read;
  while ((bytes_read = vfs_read(fd, buffer + total_read, CHUNK_SIZE)) > 0) {
    total_read += bytes_read;

    // Si nos quedamos sin espacio, expandir buffer
    if (total_read + CHUNK_SIZE > total_allocated) {
      uint32_t new_size = total_allocated * 2;

      if (new_size > EXEC_MAX_SIZE) {
        terminal_printf(
            &main_terminal,
            ANSI_COLOR_RED
            "[EXEC] ERROR: File too large (>%u bytes)" ANSI_COLOR_RESET "\r\n",
            EXEC_MAX_SIZE);
        kernel_free(buffer);
        vfs_close(fd);
        return NULL;
      }

      char *new_buffer = (char *)kernel_realloc(buffer, new_size);
      if (!new_buffer) {
        terminal_printf(
            &main_terminal,
            ANSI_COLOR_RED
            "[EXEC] ERROR: Cannot expand buffer to %u bytes" ANSI_COLOR_RESET
            "\r\n",
            new_size);
        kernel_free(buffer);
        vfs_close(fd);
        return NULL;
      }

      buffer = new_buffer;
      total_allocated = new_size;
    }
  }

  vfs_close(fd);

  if (total_read == 0) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] ERROR: Empty file" ANSI_COLOR_RESET "\r\n");
    kernel_free(buffer);
    return NULL;
  }

  // Ajustar tamaño final del buffer
  if (total_read < total_allocated) {
    char *final_buffer = (char *)kernel_realloc(buffer, total_read);
    if (final_buffer) {
      buffer = final_buffer;
    }
  }

  *out_size = total_read;

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "[EXEC]" ANSI_COLOR_RESET
                                   " Loaded " ANSI_COLOR_CYAN
                                   "%u" ANSI_COLOR_RESET " bytes from disk\r\n",
                  total_read);

  // Debug: mostrar primeros bytes
  terminal_printf(&main_terminal, "[EXEC] First 16 bytes: ");
  for (int i = 0; i < 16 && i < total_read; i++) {
    terminal_printf(&main_terminal, "%02X ", (uint8_t)buffer[i]);
  }
  terminal_printf(&main_terminal, "\r\n");

  return buffer;
}

// ============================================================================
// DETECCIÓN DE DIRECCIÓN DE CARGA
// ============================================================================

/**
 * Intenta detectar la dirección de carga esperada del binario
 * Para binarios planos, devuelve EXEC_CODE_BASE por defecto
 */
static uint32_t detect_load_address(const void *data, uint32_t size) {
  if (!data || size < 4) {
    return EXEC_CODE_BASE;
  }

  const uint8_t *bytes = (const uint8_t *)data;

  // Detectar ELF (futuro)
  if (size >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' &&
      bytes[3] == 'F') {
    terminal_printf(
        &main_terminal, ANSI_COLOR_YELLOW
        "[EXEC] ELF format detected (not fully supported yet)" ANSI_COLOR_RESET
        "\r\n");
    return EXEC_CODE_BASE; // Por ahora
  }

  // Para binarios planos, usar dirección por defecto
  return EXEC_CODE_BASE;
}

// ============================================================================
// MAPEO DE MEMORIA CON VERIFICACIÓN EXHAUSTIVA
// ============================================================================

/**
 * Mapea páginas en memoria de usuario con verificación completa
 */
static bool map_user_pages(uint32_t virt_start, uint32_t size,
                           const char *region_name) {
  uint32_t aligned_size = ALIGN_4KB_UP(size);
  uint32_t num_pages = aligned_size / PAGE_SIZE;

  terminal_printf(&main_terminal,
                  ANSI_COLOR_CYAN
                  "[EXEC]" ANSI_COLOR_RESET " Mapping %s: " ANSI_COLOR_YELLOW
                  "0x%08x" ANSI_COLOR_RESET " - " ANSI_COLOR_YELLOW
                  "0x%08x" ANSI_COLOR_RESET " (%u pages)\r\n",
                  region_name, virt_start, virt_start + aligned_size,
                  num_pages);

  for (uint32_t i = 0; i < num_pages; i++) {
    uint32_t virt_addr = virt_start + (i * PAGE_SIZE);

    // Si ya está mapeada, verificar permisos
    if (mmu_is_mapped(virt_addr)) {
      uint32_t flags = mmu_get_page_flags(virt_addr);

      // Verificar que tenga PAGE_USER
      if (!(flags & PAGE_USER)) {
        terminal_printf(&main_terminal,
                        ANSI_COLOR_YELLOW
                        "[EXEC] WARNING: Page 0x%08x mapped without USER flag, "
                        "fixing..." ANSI_COLOR_RESET "\r\n",
                        virt_addr);

        if (!mmu_set_page_user(virt_addr)) {
          terminal_printf(
              &main_terminal,
              ANSI_COLOR_RED
              "[EXEC] ERROR: Cannot set USER flag on 0x%08x" ANSI_COLOR_RESET
              "\r\n",
              virt_addr);
          return false;
        }
      }

      // Verificar que sea writable
      if (!(flags & PAGE_RW)) {
        if (!mmu_set_flags(virt_addr, flags | PAGE_RW)) {
          terminal_printf(
              &main_terminal,
              ANSI_COLOR_RED
              "[EXEC] ERROR: Cannot set RW flag on 0x%08x" ANSI_COLOR_RESET
              "\r\n",
              virt_addr);
          return false;
        }
      }
    } else {
      // Mapear nueva página (identity mapping para simplificar)
      if (!mmu_map_page(virt_addr, virt_addr,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
        terminal_printf(
            &main_terminal,
            ANSI_COLOR_RED
            "[EXEC] ERROR: Cannot map page at 0x%08x" ANSI_COLOR_RESET "\r\n",
            virt_addr);
        return false;
      }
    }
  }

  // Verificación exhaustiva post-mapeo
  terminal_printf(&main_terminal,
                  ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                  " Verifying %s mapping...\r\n",
                  region_name);

  for (uint32_t i = 0; i < num_pages; i++) {
    uint32_t virt_addr = virt_start + (i * PAGE_SIZE);

    if (!mmu_is_mapped(virt_addr)) {
      terminal_printf(
          &main_terminal,
          ANSI_COLOR_RED
          "[EXEC] ERROR: Page 0x%08x not mapped after mapping!" ANSI_COLOR_RESET
          "\r\n",
          virt_addr);
      return false;
    }

    uint32_t flags = mmu_get_page_flags(virt_addr);
    bool has_user = (flags & PAGE_USER) != 0;
    bool has_rw = (flags & PAGE_RW) != 0;
    bool has_present = (flags & PAGE_PRESENT) != 0;

    if (!has_user || !has_rw || !has_present) {
      terminal_printf(&main_terminal,
                      ANSI_COLOR_RED "[EXEC] ERROR: Page 0x%08x has wrong "
                                     "flags: P=%d W=%d U=%d" ANSI_COLOR_RESET
                                     "\r\n",
                      virt_addr, has_present, has_rw, has_user);
      return false;
    }
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN
                  "[EXEC] %s mapped and verified successfully" ANSI_COLOR_RESET
                  "\r\n",
                  region_name);
  return true;
}

// ============================================================================
// CARGA DE BINARIO EN MEMORIA DE USUARIO
// ============================================================================

/**
 * Copia el código del kernel a memoria de usuario
 */
static bool copy_code_to_user(const void *kernel_buffer, uint32_t size,
                              uint32_t user_dest) {
  if (!kernel_buffer || size == 0) {
    return false;
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                  " Copying " ANSI_COLOR_YELLOW
                                  "%u" ANSI_COLOR_RESET
                                  " bytes to user space (" ANSI_COLOR_YELLOW
                                  "0x%08x" ANSI_COLOR_RESET ")\r\n",
                  size, user_dest);

  // Copiar página por página con verificación
  const uint8_t *src = (const uint8_t *)kernel_buffer;
  uint8_t *dst = (uint8_t *)user_dest;

  for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE) {
    uint32_t page_addr = user_dest + offset;

    // Verificar acceso antes de escribir
    if (!mmu_can_user_access(page_addr, true)) {
      terminal_printf(
          &main_terminal,
          ANSI_COLOR_RED
          "[EXEC] ERROR: Cannot write to user page 0x%08x" ANSI_COLOR_RESET
          "\r\n",
          page_addr);
      return false;
    }

    uint32_t bytes_to_copy = PAGE_SIZE;
    if (offset + bytes_to_copy > size) {
      bytes_to_copy = size - offset;
    }

    memcpy(dst + offset, src + offset, bytes_to_copy);
  }

  // Verificar que se copió correctamente
  terminal_printf(&main_terminal, ANSI_COLOR_CYAN
                  "[EXEC]" ANSI_COLOR_RESET " Verifying copied data...\r\n");

  const uint8_t *verify = (const uint8_t *)user_dest;
  terminal_printf(&main_terminal,
                  "[EXEC] First 16 bytes at 0x%08x: ", user_dest);
  for (int i = 0; i < 16 && i < size; i++) {
    terminal_printf(&main_terminal, "%02X ", verify[i]);
  }
  terminal_printf(&main_terminal, "\r\n");

  // Comparar algunos bytes
  for (int i = 0; i < 16 && i < size; i++) {
    if (verify[i] != src[i]) {
      terminal_printf(&main_terminal,
                      ANSI_COLOR_RED
                      "[EXEC] ERROR: Data mismatch at offset %d: expected "
                      "%02X, got %02X" ANSI_COLOR_RESET "\r\n",
                      i, src[i], verify[i]);
      return false;
    }
  }

  terminal_printf(&main_terminal, ANSI_COLOR_GREEN
                  "[EXEC] Code copied and verified" ANSI_COLOR_RESET "\r\n");
  return true;
}

// ============================================================================
// FUNCIÓN PRINCIPAL DE CARGA Y EJECUCIÓN
// ============================================================================

/**
 * Carga un ejecutable desde disco y crea una tarea en modo usuario
 */
task_t *exec_load_and_run(const char *path) {
  if (!path) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] ERROR: NULL path" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  // ====== ENCABEZADO DEL CARGADOR ======
  terminal_printf(&main_terminal,
                  "\r\n"
                  "================================================\r\n"
                  "         EXECUTABLE LOADER - STARTING\r\n"
                  "================================================\r\n\r\n");

  // ====== PASO 0: Verificar y normalizar path ======
  terminal_printf(&main_terminal, ANSI_COLOR_BLUE "[STEP 0]" ANSI_COLOR_RESET
                                                  " Validating path...\r\n");

  char normalized_path[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized_path, VFS_PATH_MAX) != VFS_OK) {
    terminal_printf(&main_terminal,
                    ANSI_COLOR_RED
                    "[EXEC] ERROR: Invalid path format: %s" ANSI_COLOR_RESET
                    "\r\n",
                    path);
    return NULL;
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Path:" ANSI_COLOR_RESET " %s\r\n",
                  normalized_path);

  // Verificar que el path tenga extensión .bin (opcional)
  const char *ext = strrchr(normalized_path, '.');
  if (!ext || strcmp(ext, ".bin") != 0) {
    terminal_printf(
        &main_terminal, ANSI_COLOR_YELLOW
        "  WARNING: File doesn't have .bin extension" ANSI_COLOR_RESET "\r\n");
  }

  // Verificar que el directorio padre exista
  char parent_dir[VFS_PATH_MAX];
  char filename[VFS_NAME_MAX];

  if (vfs_split_path(normalized_path, parent_dir, filename) != VFS_OK) {
    terminal_printf(
        &main_terminal, ANSI_COLOR_RED
        "[EXEC] ERROR: Cannot split path components" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Directory:" ANSI_COLOR_RESET " %s\r\n",
                  parent_dir);
  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Filename:" ANSI_COLOR_RESET " %s\r\n",
                  filename);

  // ====== PASO 1: Cargar archivo desde disco ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 1]" ANSI_COLOR_RESET
                  " Loading file from disk...\r\n");

  uint32_t file_size;
  void *file_buffer = load_file_from_disk(normalized_path, &file_size);

  if (!file_buffer) {
    // Intentar diagnóstico más detallado
    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_MAGENTA "[DEBUG]" ANSI_COLOR_RESET
                    " Debugging mount points...\r\n");

    // Intentar abrir el archivo directamente para debugging
    int test_fd = vfs_open(normalized_path, VFS_O_RDONLY);
    if (test_fd < 0) {
      terminal_printf(&main_terminal, "  vfs_open failed with fd=%d\r\n",
                      test_fd);

      // Intentar abrir el directorio padre
      int dir_fd = vfs_open(parent_dir, VFS_O_RDONLY);
      if (dir_fd < 0) {
        terminal_printf(&main_terminal,
                        "  Also cannot open parent directory %s (fd=%d)\r\n",
                        parent_dir, dir_fd);

        // Sugerir montar un filesystem en /home
        terminal_printf(
            &main_terminal,
            "\r\n" ANSI_COLOR_CYAN "[SOLUTION]" ANSI_COLOR_RESET
            " You may need to mount a filesystem:\r\n"
            "  1. Create /home directory: vfs_mkdir(\"/home\", NULL);\r\n"
            "  2. Mount filesystem: vfs_mount(\"/home\", \"ramfs\", NULL);\r\n"
            "  3. Copy hello.bin to /home/\r\n");
      } else {
        terminal_printf(&main_terminal,
                        "  Parent directory %s exists but file not found\r\n",
                        parent_dir);
        vfs_close(dir_fd);
      }
    } else {
      terminal_printf(&main_terminal,
                      "  File opened successfully (fd=%d), but "
                      "load_file_from_disk failed\r\n",
                      test_fd);
      vfs_close(test_fd);
    }

    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_RED
                    "[EXEC] Failed to load file" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  // ====== PASO 2: Detectar dirección de carga ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 2]" ANSI_COLOR_RESET
                  " Detecting load address...\r\n");

  uint32_t load_addr = detect_load_address(file_buffer, file_size);
  uint32_t aligned_size = ALIGN_4KB_UP(file_size);

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN
                  "  Load address:" ANSI_COLOR_RESET
                  " 0x%08x\r\n" ANSI_COLOR_GREEN "  File size:" ANSI_COLOR_RESET
                  " %u bytes (aligned: %u bytes, %u pages)\r\n",
                  load_addr, file_size, aligned_size, aligned_size / PAGE_SIZE);

  // ====== PASO 3: Mapear memoria de código ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 3]" ANSI_COLOR_RESET
                  " Mapping code memory...\r\n");

  if (!map_user_pages(load_addr, aligned_size, "CODE")) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] Failed to map code pages" ANSI_COLOR_RESET "\r\n");
    kernel_free(file_buffer);
    return NULL;
  }

  // ====== PASO 4: Copiar código a memoria de usuario ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 4]" ANSI_COLOR_RESET
                  " Copying code to user space...\r\n");

  if (!copy_code_to_user(file_buffer, file_size, load_addr)) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] Failed to copy code" ANSI_COLOR_RESET "\r\n");
    kernel_free(file_buffer);
    return NULL;
  }

  // Ya no necesitamos el buffer del kernel
  kernel_free(file_buffer);

  // ====== PASO 5: Crear tarea en modo usuario ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 5]" ANSI_COLOR_RESET
                  " Creating user mode task...\r\n");

  // Extraer nombre del programa
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Program name:" ANSI_COLOR_RESET " %s\r\n",
                  name);
  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Entry point:" ANSI_COLOR_RESET
                                   " 0x%08x\r\n",
                  load_addr);

  // Crear tarea usando task_create_user
  task_t *task = task_create_user(name, (void *)load_addr, NULL, file_size,
                                  TASK_PRIORITY_NORMAL);

  if (!task) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] Failed to create user task" ANSI_COLOR_RESET
                    "\r\n");
    return NULL;
  }

  terminal_printf(&main_terminal,
                  ANSI_COLOR_GREEN "  Task created:" ANSI_COLOR_RESET
                                   " PID=%u, name=%s\r\n",
                  task->task_id, task->name);

  // ====== PASO 6: Verificación final ======
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_BLUE "[STEP 6]" ANSI_COLOR_RESET
                  " Final verification...\r\n");

  terminal_printf(
      &main_terminal,
      ANSI_COLOR_GREEN "  Task info:" ANSI_COLOR_RESET "\r\n"
                       "    - PID: %u\r\n"
                       "    - Name: %s\r\n"
                       "    - Entry: 0x%08x\r\n"
                       "    - Code base: 0x%08x\r\n"
                       "    - Code size: %u bytes\r\n"
                       "    - User stack: 0x%08x - 0x%08x (%u bytes)\r\n"
                       "    - Flags: 0x%08x (USER_MODE=%s)\r\n",
      task->task_id, task->name, (uint32_t)task->user_entry_point,
      (uint32_t)task->user_code_base, task->user_code_size,
      (uint32_t)task->user_stack_base, (uint32_t)task->user_stack_top,
      task->user_stack_size, task->flags,
      (task->flags & TASK_FLAG_USER_MODE) ? "YES" : "NO");

  // ====== ÉXITO ======
  terminal_printf(&main_terminal,
                  "\r\n"
                  "================================================\r\n"
                  "     EXECUTABLE LOADED SUCCESSFULLY\r\n"
                  "================================================\r\n\r\n");

  return task;
}

// ============================================================================
// FUNCIÓN DE PRUEBA CON COLORES
// ============================================================================

/**
 * Carga y prueba un ejecutable con colores ANSI
 */
void exec_test_program(const char *path) {
  terminal_printf(&main_terminal,
                  "\r\n" ANSI_COLOR_CYAN
                  "=== TESTING EXECUTABLE LOADER ===" ANSI_COLOR_RESET
                  "\r\n" ANSI_COLOR_GREEN "Program:" ANSI_COLOR_RESET
                  " %s\r\n\r\n",
                  path);

  task_t *task = exec_load_and_run(path);

  if (task) {
    terminal_printf(
        &main_terminal,
        ANSI_COLOR_GREEN
        "[SUCCESS]" ANSI_COLOR_RESET " Program loaded successfully!\r\n"
        "  " ANSI_COLOR_CYAN "PID:" ANSI_COLOR_RESET " %u\r\n"
        "  The program will start executing when scheduled.\r\n\r\n",
        task->task_id);
  } else {
    terminal_printf(&main_terminal,
                    ANSI_COLOR_RED "[FAILED]" ANSI_COLOR_RESET
                                   " Failed to load program\r\n\r\n");
  }
}