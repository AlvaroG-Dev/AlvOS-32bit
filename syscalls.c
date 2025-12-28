// syscalls.c - VERSIÓN CORREGIDA (COMPATIBLE CON VFS)
#include "syscalls.h"
#include "idt.h"
#include "irq.h"
#include "kernel.h"
#include "mmu.h"
#include "string.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"

extern void syscall_entry(void);

// Variables globales - usar int para FDs (compatible con VFS)
static char cwd_buffer[VFS_PATH_MAX] = "/";
static int open_files[VFS_MAX_FDS] = {0}; // 0 = descriptor libre

// ✅ Función mejorada para verificar puntero de usuario
bool validate_user_pointer(uint32_t ptr, uint32_t size) {
  // Verificar que no sea NULL
  if (ptr == 0) {
    terminal_printf(&main_terminal, "[SYSCALL] ERROR: NULL pointer\r\n");
    return false;
  }

  // ✅ CRÍTICO: Rechazar punteros de tamaño 0 (anteriormente se aceptaban)
  if (size == 0) {
    terminal_printf(&main_terminal, "[SYSCALL] ERROR: Zero size\r\n");
    return false;
  }

  // ✅ Verificar que no esté en espacio del kernel
  if (ptr >= 0xC0000000) {
    terminal_printf(&main_terminal,
                    "[SYSCALL] ERROR: Pointer 0x%08x in kernel space\r\n", ptr);
    return false;
  }

  // ✅ Verificar overflow
  if (ptr + size < ptr) {
    terminal_printf(
        &main_terminal,
        "[SYSCALL] ERROR: Address overflow (ptr=0x%08x, size=%u)\r\n", ptr,
        size);
    return false;
  }

  // ✅ Verificar que TODAS las páginas estén mapeadas con PAGE_USER
  uint32_t start_page = ptr & ~0xFFF;
  uint32_t end_page = (ptr + size - 1) & ~0xFFF;

  for (uint32_t page = start_page; page <= end_page; page += PAGE_SIZE) {
    if (!mmu_is_mapped(page)) {
      terminal_printf(&main_terminal,
                      "[SYSCALL] ERROR: Page 0x%08x not mapped\r\n", page);
      return false;
    }

    // Verificar permisos de usuario
    uint32_t flags = mmu_get_page_flags(page);
    if (!(flags & PAGE_USER)) {
      terminal_printf(
          &main_terminal,
          "[SYSCALL] ERROR: Page 0x%08x missing PAGE_USER (flags=0x%03x)\r\n",
          page, flags);
      return false;
    }
  }

  return true;
}

// ✅ Función mejorada para copiar datos desde usuario
int copy_from_user(void *kernel_dst, uint32_t user_src, size_t size) {
  if (!kernel_dst) {
    terminal_printf(&main_terminal, "[SYSCALL] ERROR: kernel_dst is NULL\r\n");
    return -EFAULT;
  }

  if (!validate_user_pointer(user_src, size)) {
    terminal_printf(
        &main_terminal,
        "[SYSCALL] ERROR: Invalid user pointer 0x%08x (size=%u)\r\n", user_src,
        size);
    return -EFAULT;
  }

  char *dst = (char *)kernel_dst;
  char *src = (char *)user_src;

  // ✅ Copiar con verificación de acceso por página
  for (size_t i = 0; i < size; i++) {
    // Verificar acceso en cada frontera de página
    if ((i & 0xFFF) == 0) {
      uint32_t current_page = (user_src + i) & ~0xFFF;
      if (!mmu_is_mapped(current_page)) {
        terminal_printf(
            &main_terminal,
            "[SYSCALL] Page fault at 0x%08x during copy_from_user\r\n",
            user_src + i);
        return i; // Retorna cuántos bytes se copiaron exitosamente
      }
    }
    dst[i] = src[i];
  }

  return size;
}

// ✅ Función mejorada para copiar datos hacia usuario
int copy_to_user(uint32_t user_dst, void *kernel_src, size_t size) {
  if (!kernel_src) {
    terminal_printf(&main_terminal, "[SYSCALL] ERROR: kernel_src is NULL\r\n");
    return -EFAULT;
  }

  if (!validate_user_pointer(user_dst, size)) {
    terminal_printf(
        &main_terminal,
        "[SYSCALL] ERROR: Invalid user pointer 0x%08x (size=%u)\r\n", user_dst,
        size);
    return -EFAULT;
  }

  // ✅ CRÍTICO: Verificar que las páginas tengan permiso de escritura
  uint32_t start_page = user_dst & ~0xFFF;
  uint32_t end_page = (user_dst + size - 1) & ~0xFFF;

  for (uint32_t page = start_page; page <= end_page; page += PAGE_SIZE) {
    if (!mmu_can_user_access(page, true)) { // true = necesita escritura
      terminal_printf(&main_terminal,
                      "[SYSCALL] ERROR: Page 0x%08x not writable for user\r\n",
                      page);
      return -EFAULT;
    }
  }

  char *dst = (char *)user_dst;
  char *src = (char *)kernel_src;

  for (size_t i = 0; i < size; i++) {
    // Verificar acceso en cada frontera de página
    if ((i & 0xFFF) == 0) {
      uint32_t current_page = (user_dst + i) & ~0xFFF;
      if (!mmu_is_mapped(current_page)) {
        terminal_printf(
            &main_terminal,
            "[SYSCALL] Page fault at 0x%08x during copy_to_user\r\n",
            user_dst + i);
        return i;
      }
    }
    dst[i] = src[i];
  }

  return size;
}

// ✅ Función mejorada para copiar string desde usuario
int copy_string_from_user(char *kernel_dst, uint32_t user_src, size_t max_len) {
  if (!kernel_dst) {
    return -EFAULT;
  }

  // Validar al menos 1 byte
  if (!validate_user_pointer(user_src, 1)) {
    return -EFAULT;
  }

  for (size_t i = 0; i < max_len; i++) {
    // Verificar acceso en cada frontera de página
    if ((i & 0xFFF) == 0 && i > 0) {
      uint32_t current_page = (user_src + i) & ~0xFFF;
      if (!mmu_is_mapped(current_page)) {
        terminal_printf(&main_terminal,
                        "[SYSCALL] Page fault at 0x%08x during string copy\r\n",
                        user_src + i);
        return -EFAULT;
      }
    }

    char c;
    if (copy_from_user(&c, user_src + i, 1) != 1) {
      return -EFAULT;
    }

    kernel_dst[i] = c;

    if (c == '\0') {
      return i; // Retorna longitud sin incluir el null terminator
    }
  }

  // Buffer demasiado pequeño o string sin null terminator
  kernel_dst[max_len - 1] = '\0';
  return -ENAMETOOLONG;
}

void syscall_init(void) {
  // Configurar INT 0x80 como puerta de syscall
  idt_set_gate(0x80, (uintptr_t)syscall_entry, 0x08,
               IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_FLAG_INTERRUPT32);

  terminal_puts(&main_terminal, "Syscalls initialized (INT 0x80)\r\n");
}

void syscall_handler(struct regs *r) {
  uint32_t syscall_num = r->eax;
  uint32_t result = 0;

  // Verificar que estamos en una tarea de usuario
  task_t *current = scheduler.current_task;
  if (!current || !(current->flags & TASK_FLAG_USER_MODE)) {
    terminal_printf(&main_terminal, "[SYSCALL] ERROR: Not a user task\r\n");
    r->eax = (uint32_t)-EPERM;
    return;
  }

  // ✅ Debug: Mostrar información de la syscall
  terminal_printf(&main_terminal,
                  "[SYSCALL] Task %u: num=0x%02x (%u), ebx=0x%08x, ecx=0x%08x, "
                  "edx=0x%08x\r\n",
                  current->task_id, syscall_num, syscall_num, r->ebx, r->ecx,
                  r->edx);

  switch (syscall_num) {
  case SYSCALL_EXIT: {
    int exit_code = (int)r->ebx;
    terminal_printf(&main_terminal,
                    "[SYSCALL] Process %u exited with code %d\r\n",
                    current->task_id, exit_code);
    task_exit(exit_code);
    // task_exit nunca retorna
    break;
  }

  case SYSCALL_WRITE: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    size_t count = r->edx;

    terminal_printf(&main_terminal,
                    "[SYSCALL_WRITE] fd=%d, buf=0x%08x, count=%u\r\n", fd,
                    buf_ptr, count);

    // ✅ Validar parámetros básicos
    if (count == 0) {
      result = 0;
      break;
    }

    if (count > 4096) { // Límite razonable
      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] ERROR: count too large (%u)\r\n", count);
      result = (uint32_t)-EINVAL;
      break;
    }

    // ✅ Validar puntero de usuario CON DEBUGGING
    if (!validate_user_pointer(buf_ptr, count)) {
      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] ERROR: Invalid buffer pointer\r\n");
      result = (uint32_t)-EFAULT;
      break;
    }

    // Buffer temporal en kernel
    char *kernel_buffer = (char *)kernel_malloc(count + 1);
    if (!kernel_buffer) {
      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] ERROR: Cannot allocate %u bytes\r\n",
                      count);
      result = (uint32_t)-ENOMEM;
      break;
    }

    // ✅ Copiar datos desde espacio de usuario CON VERIFICACIÓN
    int copied = copy_from_user(kernel_buffer, buf_ptr, count);
    if (copied < 0 || (size_t)copied != count) {
      terminal_printf(
          &main_terminal,
          "[SYSCALL_WRITE] ERROR: copy_from_user failed (copied=%d/%u)\r\n",
          copied, count);
      kernel_free(kernel_buffer);
      result = (uint32_t)-EFAULT;
      break;
    }
    kernel_buffer[count] = '\0';

    terminal_printf(&main_terminal,
                    "[SYSCALL_WRITE] Buffer copied successfully: \"%s\"\r\n",
                    kernel_buffer);

    // Procesar según el file descriptor
    if (fd == 1 || fd == 2) { // stdout/stderr
      terminal_printf(&main_terminal, "[SYSCALL_WRITE] Writing to %s:\r\n",
                      fd == 1 ? "stdout" : "stderr");

      for (size_t i = 0; i < count; i++) {
        terminal_putchar(&main_terminal, kernel_buffer[i]);
      }
      result = count;

      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] Wrote %u bytes successfully\r\n", count);
    } else if (fd == 0) { // stdin - no se puede escribir a stdin
      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] ERROR: Cannot write to stdin\r\n");
      result = (uint32_t)-EBADF;
    } else if (fd >= 3 && fd < VFS_MAX_FDS && open_files[fd] != 0) {
      // Escribir a archivo usando vfs_write
      int32_t vfs_result = vfs_write(open_files[fd], kernel_buffer, count);
      if (vfs_result == VFS_OK) {
        result = count;
        terminal_printf(&main_terminal,
                        "[SYSCALL_WRITE] Wrote %u bytes to file\r\n", count);
      } else {
        terminal_printf(&main_terminal,
                        "[SYSCALL_WRITE] ERROR: vfs_write failed (err=%d)\r\n",
                        vfs_result);
        result = (uint32_t)-EIO;
      }
    } else {
      terminal_printf(&main_terminal,
                      "[SYSCALL_WRITE] ERROR: Invalid fd=%d\r\n", fd);
      result = (uint32_t)-EBADF;
    }

    kernel_free(kernel_buffer);
    break;
  }

  case SYSCALL_READ: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    size_t count = r->edx;

    terminal_printf(&main_terminal,
                    "[SYSCALL_READ] fd=%d, buf=0x%08x, count=%u\r\n", fd,
                    buf_ptr, count);

    // Validar buffer de usuario
    if (!validate_user_pointer(buf_ptr, count)) {
      terminal_printf(&main_terminal,
                      "[SYSCALL_READ] ERROR: Invalid buffer pointer\r\n");
      result = (uint32_t)-EFAULT;
      break;
    }

    // Buffer temporal en kernel
    char *kernel_buffer = (char *)kernel_malloc(count);
    if (!kernel_buffer) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    size_t bytes_read = 0;

    if (fd == 0) { // stdin
      // TODO: Implementación real de stdin
      const char *test_input = "test\n";
      size_t to_copy = (count < 5) ? count : 5;
      memcpy(kernel_buffer, test_input, to_copy);
      bytes_read = to_copy;
    } else if (fd == 1 || fd == 2) { // stdout/stderr - no se puede leer
      terminal_printf(&main_terminal,
                      "[SYSCALL_READ] ERROR: Cannot read from %s\r\n",
                      fd == 1 ? "stdout" : "stderr");
      result = (uint32_t)-EBADF;
      kernel_free(kernel_buffer);
      break;
    } else if (fd >= 3 && fd < VFS_MAX_FDS && open_files[fd] != 0) {
      // Leer de archivo
      int32_t vfs_result = vfs_read(open_files[fd], kernel_buffer, count);
      if (vfs_result == VFS_OK) {
        bytes_read = count;
      } else {
        result = (uint32_t)-EIO;
        kernel_free(kernel_buffer);
        break;
      }
    } else {
      result = (uint32_t)-EBADF;
      kernel_free(kernel_buffer);
      break;
    }

    // Copiar datos al espacio de usuario
    if (bytes_read > 0) {
      int copied = copy_to_user(buf_ptr, kernel_buffer, bytes_read);
      if (copied < 0 || (size_t)copied != bytes_read) {
        terminal_printf(&main_terminal,
                        "[SYSCALL_READ] ERROR: copy_to_user failed\r\n");
        result = (uint32_t)-EFAULT;
      } else {
        result = bytes_read;
      }
    } else {
      result = 0; // EOF o nada leído
    }

    kernel_free(kernel_buffer);
    break;
  }

  case SYSCALL_GETPID:
    result = current->task_id;
    terminal_printf(&main_terminal, "[SYSCALL_GETPID] Returned PID=%u\r\n",
                    result);
    break;

  case SYSCALL_YIELD:
    terminal_printf(&main_terminal, "[SYSCALL_YIELD] Task yielding\r\n");
    task_yield();
    result = 0;
    break;

  case SYSCALL_SLEEP:
    terminal_printf(&main_terminal, "[SYSCALL_SLEEP] Sleep %u ms\r\n", r->ebx);
    task_sleep(r->ebx);
    result = 0;
    break;

  case SYSCALL_GETTIME:
    result = ticks_since_boot;
    break;

  case SYSCALL_STAT: {
    // TODO: Implementar vfs_stat si existe
    result = (uint32_t)-ENOSYS;
    break;
  }

  case SYSCALL_FORK: {
    // TODO: Implementar fork
    result = (uint32_t)-ENOSYS;
    break;
  }

  case SYSCALL_EXECVE: {
    // TODO: Implementar execve
    result = (uint32_t)-ENOSYS;
    break;
  }

  default:
    terminal_printf(&main_terminal,
                    "[SYSCALL] Unknown syscall: 0x%02X (%u)\r\n", syscall_num,
                    syscall_num);
    result = (uint32_t)-ENOSYS;
    break;
  }

  terminal_printf(&main_terminal, "[SYSCALL] Returning result=0x%08x (%d)\r\n",
                  result, (int)result);
  r->eax = result;
}