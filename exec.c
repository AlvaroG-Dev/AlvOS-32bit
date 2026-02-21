#include "exec.h"
#include "elf.h"
#include "kernel.h"
#include "memory.h"
#include "mmu.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"

// Global verbose flag - when false, suppresses debug output
bool exec_verbose = false;

// Forward declarations
static bool map_user_pages(address_space_t *as, uint32_t virt_start,
                           uint32_t size, const char *region_name);
static bool copy_code_to_user(const void *kernel_buffer, uint32_t size,
                              uint32_t user_dest);

// ============================================================================
// CARGA DE ARCHIVO DESDE DISCO
// ============================================================================

/**
 * Lee un archivo completo desde VFS a un buffer del kernel
 */
static void *load_file_from_disk(const char *path, uint32_t *out_size) {
  if (!path || !out_size) {
    if (exec_verbose)
      serial_printf(COM1_BASE, ANSI_COLOR_RED
                    "[EXEC] ERROR: Invalid parameters" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  if (exec_verbose)
    serial_printf(COM1_BASE,
                  ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                  " Loading file: " ANSI_COLOR_YELLOW
                                  "%s" ANSI_COLOR_RESET "\r\n",
                  path);

  // Abrir archivo
  int fd = vfs_open(path, VFS_O_RDONLY);
  if (fd < 0) {
    if (exec_verbose)
      serial_printf(COM1_BASE,
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
    if (exec_verbose)
      serial_printf(
          COM1_BASE, ANSI_COLOR_RED
          "[EXEC] ERROR: Cannot allocate initial buffer" ANSI_COLOR_RESET
          "\r\n");
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
    if (exec_verbose)
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

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_GREEN
                    "[EXEC]" ANSI_COLOR_RESET " Loaded " ANSI_COLOR_CYAN
                    "%u" ANSI_COLOR_RESET " bytes from disk\r\n",
                    total_read);

  // Debug: mostrar primeros bytes
  if (exec_verbose)
    terminal_printf(&main_terminal, "[EXEC] First 16 bytes: ");
  for (int i = 0; i < 16 && i < total_read; i++) {
    if (exec_verbose)
      terminal_printf(&main_terminal, "%02X ", (uint8_t)buffer[i]);
  }
  if (exec_verbose)
    terminal_printf(&main_terminal, "\r\n");

  return buffer;
}

// ============================================================================
// DETECCIÓN DE DIRECCIÓN DE CARGA
// ============================================================================

/**
 * Verifica si el encabezado ELF es válido para AlvOS (32-bit, Intel 386)
 */
static bool elf_check_header(Elf32_Ehdr *header) {
  if (!header)
    return false;

  // Verificar número mágico
  if (header->e_ident[EI_MAG0] != ELFMAG0 ||
      header->e_ident[EI_MAG1] != ELFMAG1 ||
      header->e_ident[EI_MAG2] != ELFMAG2 ||
      header->e_ident[EI_MAG3] != ELFMAG3) {
    return false;
  }

  // Verificar que sea 32-bit
  if (header->e_ident[EI_CLASS] != ELFCLASS32) {
    if (exec_verbose)
      terminal_printf(
          &main_terminal, ANSI_COLOR_RED
          "[ELF] ERROR: Not a 32-bit executable\r\n" ANSI_COLOR_RESET);
    return false;
  }

  // Verificar endianness (Little Endian para x86)
  if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
    if (exec_verbose)
      terminal_printf(&main_terminal, ANSI_COLOR_RED
                      "[ELF] ERROR: Not little-endian\r\n" ANSI_COLOR_RESET);
    return false;
  }

  // Verificar tipo de archivo (Ejecutable o Dinámico/PIE)
  if (header->e_type != ET_EXEC && header->e_type != ET_DYN) {
    if (exec_verbose)
      terminal_printf(&main_terminal,
                      ANSI_COLOR_RED "[ELF] ERROR: Not a supported executable "
                                     "type (%d)\r\n" ANSI_COLOR_RESET,
                      header->e_type);
    return false;
  }

  // Verificar arquitectura (Intel 386)
  if (header->e_machine != EM_386) {
    if (exec_verbose)
      terminal_printf(
          &main_terminal,
          ANSI_COLOR_RED
          "[ELF] ERROR: Wrong architecture (machine %d)\r\n" ANSI_COLOR_RESET,
          header->e_machine);
    return false;
  }

  return true;
}

/**
 * Aplica relocaciones a un binario ELF cargado en memoria (soporte para PIE)
 */
static bool elf_apply_relocations(const void *file_data, uint32_t delta) {
  if (delta == 0)
    return true; // No hay nada que relocalizar

  Elf32_Ehdr *header = (Elf32_Ehdr *)file_data;
  Elf32_Phdr *ph_table = (Elf32_Phdr *)((uint8_t *)file_data + header->e_phoff);

  Elf32_Dyn *dynamic_table = NULL;

  // 1. Buscar el segmento DYNAMIC
  for (int i = 0; i < header->e_phnum; i++) {
    if (ph_table[i].p_type == PT_DYNAMIC) {
      dynamic_table = (Elf32_Dyn *)(uintptr_t)(ph_table[i].p_vaddr + delta);
      break;
    }
  }

  if (!dynamic_table)
    return true; // No hay tabla dinámica, no hay relocaciones

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN
                    "[ELF]" ANSI_COLOR_RESET
                    " Applying relocations (delta: 0x%08x)...\r\n",
                    delta);

  Elf32_Rel *rel_table = NULL;
  uint32_t rel_size = 0;
  uint32_t rel_ent = 0;

  // 2. Buscar tablas de relocación en la sección dinámica
  for (Elf32_Dyn *dyn = dynamic_table; dyn->d_tag != DT_NULL; dyn++) {
    switch (dyn->d_tag) {
    case DT_REL:
      rel_table = (Elf32_Rel *)(uintptr_t)(dyn->d_un.d_ptr + delta);
      break;
    case DT_RELSZ:
      rel_size = dyn->d_un.d_val;
      break;
    case DT_RELENT:
      rel_ent = dyn->d_un.d_val;
      break;
    }
  }

  // 3. Aplicar relocaciones de tipo RELATIVE (comunes en PIE)
  if (rel_table && rel_ent > 0) {
    uint32_t count = rel_size / rel_ent;
    for (uint32_t i = 0; i < count; i++) {
      Elf32_Rel *rel = (Elf32_Rel *)((uint8_t *)rel_table + (i * rel_ent));
      if (ELF32_R_TYPE(rel->r_info) == R_386_RELATIVE) {
        uint32_t *addr = (uint32_t *)(uintptr_t)(rel->r_offset + delta);
        *addr += delta;
      }
    }
  }

  return true;
}

/**
 * Carga los segmentos de un archivo ELF en memoria
 */
static bool elf_load_segments(const void *data, uint32_t size, uint32_t delta,
                              address_space_t *as) {
  Elf32_Ehdr *header = (Elf32_Ehdr *)data;
  Elf32_Phdr *ph_table = (Elf32_Phdr *)((uint8_t *)data + header->e_phoff);

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN
                    "[ELF]" ANSI_COLOR_RESET
                    " Loading segments (%d total, delta=0x%x)...\r\n",
                    header->e_phnum, delta);

  for (int i = 0; i < header->e_phnum; i++) {
    Elf32_Phdr *phdr = &ph_table[i];

    // Solo nos interesan los segmentos cargables (PT_LOAD)
    if (phdr->p_type != PT_LOAD)
      continue;

    if (phdr->p_memsz == 0)
      continue;

    uint32_t vaddr = phdr->p_vaddr + delta;

    if (exec_verbose)
      terminal_printf(
          &main_terminal,
          "  Segment %d: offset=0x%x, vaddr=0x%x, filesz=0x%x, memsz=0x%x\r\n",
          i, phdr->p_offset, vaddr, phdr->p_filesz, phdr->p_memsz);

    // 1. Mapear la memoria necesaria usando el VMM
    if (!map_user_pages(as, vaddr, phdr->p_memsz, "ELF_SEGMENT")) {
      return false;
    }

    // 2. Copiar datos del archivo
    if (phdr->p_filesz > 0) {
      uint8_t *dest = (uint8_t *)(uintptr_t)vaddr;
      uint8_t *src = (uint8_t *)data + phdr->p_offset;

      // Verificar que no nos salgamos del buffer de datos
      if (phdr->p_offset + phdr->p_filesz > size) {
        terminal_printf(
            &main_terminal, ANSI_COLOR_RED
            "[ELF] ERROR: Segment goes beyond file size\r\n" ANSI_COLOR_RESET);
        return false;
      }

      memcpy(dest, src, phdr->p_filesz);
    }

    // 3. Rellenar con ceros si memsz > filesz (BSS)
    if (phdr->p_memsz > phdr->p_filesz) {
      uint8_t *bss_start = (uint8_t *)(uintptr_t)vaddr + phdr->p_filesz;
      uint32_t bss_size = phdr->p_memsz - phdr->p_filesz;
      memset(bss_start, 0, bss_size);
    }
  }

  return true;
}

/**
 * Intenta detectar la dirección de carga esperada del binario
 */
static uint32_t detect_load_address(const void *data, uint32_t size) {
  if (!data || size < sizeof(Elf32_Ehdr)) {
    return EXEC_CODE_BASE;
  }

  Elf32_Ehdr *header = (Elf32_Ehdr *)data;
  if (elf_check_header(header)) {
    return header->e_entry;
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
static bool map_user_pages(address_space_t *as, uint32_t virt_start,
                           uint32_t size, const char *region_name) {
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN
                    "[EXEC]" ANSI_COLOR_RESET
                    " Mapping region %s at 0x%08x (%u bytes)\r\n",
                    region_name, virt_start, size);

  // Usar el VMM para gestionar el mapeado y la memoria física
  return vmm_map_region(as, virt_start, size,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER);
}

/**
 * Mapea páginas en memoria de usuario con verificación completa (versión
 * legacy)
 */
static bool map_user_pages_legacy(uint32_t virt_start, uint32_t size,
                                  const char *region_name) {
  uint32_t aligned_virt_start = ALIGN_4KB_DOWN(virt_start);
  uint32_t end_addr = virt_start + size;
  uint32_t aligned_end_addr = ALIGN_4KB_UP(end_addr);
  uint32_t aligned_size = aligned_end_addr - aligned_virt_start;
  uint32_t num_pages = aligned_size / PAGE_SIZE;

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN
                    "[EXEC]" ANSI_COLOR_RESET " Mapping %s: " ANSI_COLOR_YELLOW
                    "0x%08x" ANSI_COLOR_RESET " - " ANSI_COLOR_YELLOW
                    "0x%08x" ANSI_COLOR_RESET " (%u pages)\r\n",
                    region_name, aligned_virt_start,
                    aligned_virt_start + aligned_size, num_pages);

  for (uint32_t i = 0; i < num_pages; i++) {
    uint32_t virt_addr = aligned_virt_start + (i * PAGE_SIZE);

    // Si ya está mapeada, verificar permisos
    if (mmu_is_mapped(virt_addr)) {
      uint32_t flags = mmu_get_page_flags(virt_addr);

      // Verificar que tenga PAGE_USER
      if (!(flags & PAGE_USER)) {
        serial_printf(COM1_BASE,
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
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                    " Verifying %s mapping...\r\n",
                    region_name);

  for (uint32_t i = 0; i < num_pages; i++) {
    uint32_t virt_addr = aligned_virt_start + (i * PAGE_SIZE);

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
      serial_printf(COM1_BASE,
                    ANSI_COLOR_RED "[EXEC] ERROR: Page 0x%08x has wrong "
                                   "flags: P=%d W=%d U=%d" ANSI_COLOR_RESET
                                   "\r\n",
                    virt_addr, has_present, has_rw, has_user);
      return false;
    }
  }

  if (exec_verbose)
    terminal_printf(
        &main_terminal,
        ANSI_COLOR_GREEN
        "[EXEC] %s mapped and verified successfully" ANSI_COLOR_RESET "\r\n",
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

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_CYAN "[EXEC]" ANSI_COLOR_RESET
                                    " Copying " ANSI_COLOR_YELLOW
                                    "%u" ANSI_COLOR_RESET
                                    " bytes to user space (" ANSI_COLOR_YELLOW
                                    "0x%08x" ANSI_COLOR_RESET ")\r\n",
                    size, user_dest);

  // Copiar página por página con verificación
  const uint8_t *src = (const uint8_t *)kernel_buffer;
  uint8_t *dst = (uint8_t *)(uintptr_t)user_dest;

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
  if (exec_verbose)
    terminal_printf(&main_terminal, ANSI_COLOR_CYAN
                    "[EXEC]" ANSI_COLOR_RESET " Verifying copied data...\r\n");

  const uint8_t *verify = (const uint8_t *)(uintptr_t)user_dest;
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "[EXEC] First 16 bytes at 0x%08x: ", user_dest);
  for (int i = 0; i < 16 && i < size; i++) {
    if (exec_verbose)
      terminal_printf(&main_terminal, "%02X ", verify[i]);
  }
  if (exec_verbose)
    terminal_printf(&main_terminal, "\r\n");

  // Comparar algunos bytes
  for (int i = 0; i < 16 && i < size; i++) {
    if (verify[i] != src[i]) {
      serial_printf(COM1_BASE,
                    ANSI_COLOR_RED
                    "[EXEC] ERROR: Data mismatch at offset %d: expected "
                    "%02X, got %02X" ANSI_COLOR_RESET "\r\n",
                    i, src[i], verify[i]);
      return false;
    }
  }

  if (exec_verbose)
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
task_t *exec_load_and_run(int argc, char **argv) {
  if (argc < 1 || !argv || !argv[0]) {
    if (exec_verbose)
      terminal_printf(&main_terminal, ANSI_COLOR_RED
                      "[EXEC] ERROR: Invalid arguments" ANSI_COLOR_RESET
                      "\r\n");
    return NULL;
  }

  const char *path = argv[0];

  // ====== ENCABEZADO DEL CARGADOR ======
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n"
                    "================================================\r\n"
                    "         EXECUTABLE LOADER - STARTING\r\n"
                    "================================================\r\n\r\n");

  // ====== PASO 0: Verificar y normalizar path ======
  if (exec_verbose)
    terminal_printf(&main_terminal, ANSI_COLOR_BLUE "[STEP 0]" ANSI_COLOR_RESET
                                                    " Validating path...\r\n");

  char normalized_path[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized_path, VFS_PATH_MAX) != VFS_OK) {
    if (exec_verbose)
      terminal_printf(&main_terminal,
                      ANSI_COLOR_RED
                      "[EXEC] ERROR: Invalid path format: %s" ANSI_COLOR_RESET
                      "\r\n",
                      path);
    return NULL;
  }

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_GREEN "  Path:" ANSI_COLOR_RESET " %s\r\n",
                    normalized_path);

  // Verificar que el path tenga extensión .bin (opcional)
  const char *ext = strrchr(normalized_path, '.');
  if (!ext || strcmp(ext, ".bin") != 0) {
    if (exec_verbose)
      terminal_printf(
          &main_terminal, ANSI_COLOR_YELLOW
          "  WARNING: File doesn't have .bin extension" ANSI_COLOR_RESET
          "\r\n");
  }

  // Verificar que el directorio padre exista
  char parent_dir[VFS_PATH_MAX];
  char filename[VFS_NAME_MAX];

  if (vfs_split_path(normalized_path, parent_dir, filename) != VFS_OK) {
    if (exec_verbose)
      terminal_printf(
          &main_terminal, ANSI_COLOR_RED
          "[EXEC] ERROR: Cannot split path components" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_GREEN "  Directory:" ANSI_COLOR_RESET " %s\r\n",
                    parent_dir);
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_GREEN "  Filename:" ANSI_COLOR_RESET " %s\r\n",
                    filename);

  // ====== PASO 1: Cargar archivo desde disco ======
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_BLUE "[STEP 1]" ANSI_COLOR_RESET
                    " Loading file from disk...\r\n");

  uint32_t file_size;
  void *file_buffer = load_file_from_disk(normalized_path, &file_size);

  if (!file_buffer) {
    // Intentar diagnóstico más detallado
    if (exec_verbose)
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
        serial_printf(COM1_BASE,
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
        serial_printf(COM1_BASE,
                      "  Parent directory %s exists but file not found\r\n",
                      parent_dir);
        vfs_close(dir_fd);
      }
    } else {
      serial_printf(COM1_BASE,
                    "  File opened successfully (fd=%d), but "
                    "load_file_from_disk failed\r\n",
                    test_fd);
      vfs_close(test_fd);
    }

    if (exec_verbose)
      terminal_printf(&main_terminal,
                      "\r\n" ANSI_COLOR_RED
                      "[EXEC] Failed to load file" ANSI_COLOR_RESET "\r\n");
    return NULL;
  }

  // ====== PASO 2: Analizar formato y preparar espacio de direcciones ======
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_BLUE "[STEP 2]" ANSI_COLOR_RESET
                    " Preparing address space...\r\n");

  address_space_t *as = vmm_create_address_space();
  if (!as) {
    terminal_printf(&main_terminal, ANSI_COLOR_RED
                    "[EXEC] Failed to create address space" ANSI_COLOR_RESET
                    "\r\n");
    kernel_free(file_buffer);
    return NULL;
  }

  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  uint32_t entry_point = 0;
  uint32_t code_size = file_size;
  uint32_t base_delta = 0;
  uint32_t load_addr = 0;
  bool is_elf = false;

  Elf32_Ehdr *header = (Elf32_Ehdr *)file_buffer;
  if (file_size >= sizeof(Elf32_Ehdr) && elf_check_header(header)) {
    is_elf = true;
    if (exec_verbose)
      terminal_printf(&main_terminal, ANSI_COLOR_GREEN
                      "  Format: ELF32" ANSI_COLOR_RESET "\r\n");

    if (header->e_type == ET_DYN) {
      static uint32_t next_auto_base = 0x04000000;
      base_delta = next_auto_base;
      next_auto_base += 0x01000000;
    }

    entry_point = header->e_entry + base_delta;

    if (!elf_load_segments(file_buffer, file_size, base_delta, as)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(file_buffer);
      return NULL;
    }

    if (!elf_apply_relocations(file_buffer, base_delta)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(file_buffer);
      return NULL;
    }
  } else {
    load_addr = EXEC_CODE_BASE;
    entry_point = load_addr;

    if (!map_user_pages(as, load_addr, ALIGN_4KB_UP(file_size), "CODE")) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(file_buffer);
      return NULL;
    }

    if (!copy_code_to_user(file_buffer, file_size, load_addr)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(file_buffer);
      return NULL;
    }
  }

  // Restaurar CR3 original
  mmu_load_cr3(old_cr3);
  kernel_free(file_buffer);

  // ====== PASO 5: Crear tarea en modo usuario ======
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  task_t *task = task_create_user(name, (void *)(uintptr_t)entry_point, argc,
                                  argv, code_size, TASK_PRIORITY_NORMAL, as);

  if (!task) {
    if (exec_verbose)
      terminal_printf(&main_terminal, ANSI_COLOR_RED
                      "[EXEC] Failed to create user task" ANSI_COLOR_RESET
                      "\r\n");
    return NULL;
  }

  if (exec_verbose)
    terminal_printf(&main_terminal,
                    ANSI_COLOR_GREEN "  Task created:" ANSI_COLOR_RESET
                                     " PID=%u, name=%s\r\n",
                    task->task_id, task->name);

  // ====== PASO 6: Verificación final ======
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_BLUE "[STEP 6]" ANSI_COLOR_RESET
                    " Final verification...\r\n");

  if (exec_verbose)
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
        task->task_id, task->name, (uint32_t)(uintptr_t)task->user_entry_point,
        (uint32_t)(uintptr_t)task->user_code_base, task->user_code_size,
        (uint32_t)(uintptr_t)task->user_stack_base,
        (uint32_t)(uintptr_t)task->user_stack_top, task->user_stack_size,
        task->flags, (task->flags & TASK_FLAG_USER_MODE) ? "YES" : "NO");

  // ====== ÉXITO ======
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n"
                    "================================================\r\n"
                    "     EXECUTABLE LOADED SUCCESSFULLY\r\n"
                    "================================================\r\n\r\n");

  return task;
}

// ============================================================================
// FUNCIÓN SILENCIOSA DE CARGA Y EJECUCIÓN (sin debug)
// ============================================================================

/**
 * Carga un ejecutable desde disco y crea una tarea, sin mensajes de debug.
 * Usada para ejecutar comandos desde la terminal vía PATH/alias.
 */
task_t *exec_run_quiet(int argc, char **argv) {
  exec_verbose = false; // Suppress debug output

  if (argc < 1 || !argv || !argv[0]) {
    exec_verbose = true;
    return NULL;
  }

  const char *path = argv[0];

  // Normalizar path
  char normalized_path[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized_path, VFS_PATH_MAX) != VFS_OK) {
    return NULL;
  }

  // Cargar archivo desde disco (silencioso)
  int fd = vfs_open(normalized_path, VFS_O_RDONLY);
  if (fd < 0) {
    return NULL;
  }

  const uint32_t CHUNK_SIZE = 4096;
  uint32_t total_allocated = CHUNK_SIZE;
  uint32_t total_read = 0;

  char *buffer = (char *)kernel_malloc(total_allocated);
  if (!buffer) {
    vfs_close(fd);
    return NULL;
  }

  int bytes_read;
  while ((bytes_read = vfs_read(fd, buffer + total_read, CHUNK_SIZE)) > 0) {
    total_read += bytes_read;
    if (total_read + CHUNK_SIZE > total_allocated) {
      uint32_t new_size = total_allocated * 2;
      if (new_size > EXEC_MAX_SIZE) {
        kernel_free(buffer);
        vfs_close(fd);
        return NULL;
      }
      char *new_buffer = (char *)kernel_realloc(buffer, new_size);
      if (!new_buffer) {
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
    kernel_free(buffer);
    return NULL;
  }

  // Ajustar tamaño final
  if (total_read < total_allocated) {
    char *final_buffer = (char *)kernel_realloc(buffer, total_read);
    if (final_buffer) {
      buffer = final_buffer;
    }
  }

  // Procesar formato (ELF o Plano)
  address_space_t *as = vmm_create_address_space();
  if (!as) {
    kernel_free(buffer);
    return NULL;
  }

  uint32_t old_cr3 = mmu_get_current_cr3();
  mmu_load_cr3(as->page_directory);

  uint32_t entry_point = 0;
  uint32_t code_size = total_read;
  uint32_t base_delta = 0;
  uint32_t load_addr = 0;

  if (total_read >= sizeof(Elf32_Ehdr) &&
      elf_check_header((Elf32_Ehdr *)buffer)) {
    Elf32_Ehdr *header = (Elf32_Ehdr *)buffer;
    if (header->e_type == ET_DYN) {
      static uint32_t next_auto_base_quiet = 0x04000000;
      base_delta = next_auto_base_quiet;
      next_auto_base_quiet += 0x01000000;
    }

    entry_point = header->e_entry + base_delta;

    if (!elf_load_segments(buffer, total_read, base_delta, as)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(buffer);
      return NULL;
    }

    if (!elf_apply_relocations(buffer, base_delta)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(buffer);
      return NULL;
    }
  } else {
    load_addr = EXEC_CODE_BASE;
    entry_point = load_addr;

    if (!map_user_pages(as, load_addr, ALIGN_4KB_UP(total_read), "CODE")) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(buffer);
      return NULL;
    }

    if (!copy_code_to_user(buffer, total_read, load_addr)) {
      mmu_load_cr3(old_cr3);
      vmm_destroy_address_space(as);
      kernel_free(buffer);
      return NULL;
    }
  }

  mmu_load_cr3(old_cr3);
  kernel_free(buffer);

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  task_t *task = task_create_user(name, (void *)(uintptr_t)entry_point, argc,
                                  argv, code_size, TASK_PRIORITY_NORMAL, as);

  exec_verbose = true; // Restore verbose mode
  return task;
}

// ============================================================================
// FUNCIÓN DE PRUEBA CON COLORES
// ============================================================================

/**
 * Carga y prueba un ejecutable con colores ANSI
 */
void exec_test_program(const char *path) {
  if (exec_verbose)
    terminal_printf(&main_terminal,
                    "\r\n" ANSI_COLOR_CYAN
                    "=== TESTING EXECUTABLE LOADER ===" ANSI_COLOR_RESET
                    "\r\n" ANSI_COLOR_GREEN "Program:" ANSI_COLOR_RESET
                    " %s\r\n\r\n",
                    path);

  char *tmp_argv[] = {(char *)path};
  task_t *task = exec_load_and_run(1, tmp_argv);

  if (task) {
    if (exec_verbose)
      terminal_printf(
          &main_terminal,
          ANSI_COLOR_GREEN
          "[SUCCESS]" ANSI_COLOR_RESET " Program loaded successfully!\r\n"
          "  " ANSI_COLOR_CYAN "PID:" ANSI_COLOR_RESET " %u\r\n"
          "  The program will start executing when scheduled.\r\n\r\n",
          task->task_id);
  } else {
    if (exec_verbose)
      terminal_printf(&main_terminal,
                      ANSI_COLOR_RED "[FAILED]" ANSI_COLOR_RESET
                                     " Failed to load program\r\n\r\n");
  }
}