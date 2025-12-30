// syscalls.c - VERSIÓN EXTENDIDA CON MÁS SYSCALLS (CORREGIDA)
#include "syscalls.h"
#include "idt.h"
#include "irq.h"
#include "kernel.h"
#include "keyboard.h" // Para funciones de teclado
#include "mmu.h"
#include "string.h"
#include "task.h"
#include "terminal.h"
#include "vfs.h"

extern void syscall_entry(void);

// Declarar variables externas de vfs.c
extern vfs_file_t
    *fd_table[VFS_MAX_FDS]; // ¡AQUÍ ESTÁ LA DECLARACIÓN QUE FALTA!
extern int mount_count;
static char cwd_buffer[VFS_PATH_MAX] = "/";

// Variables globales locales
static int open_files[VFS_MAX_FDS] = {0};

// Funciones auxiliares mejoradas
bool validate_user_pointer(uint32_t ptr, uint32_t size) {
  if (ptr == 0)
    return false;
  if (size == 0)
    return true; // Permite punteros con tamaño 0

  // Verificar que no esté en espacio del kernel
  if (ptr >= 0xC0000000)
    return false;

  // Verificar overflow
  if (ptr + size < ptr)
    return false;

  // Verificar que todas las páginas estén mapeadas
  uint32_t start_page = ptr & ~0xFFF;
  uint32_t end_page = (ptr + size - 1) & ~0xFFF;

  for (uint32_t page = start_page; page <= end_page; page += PAGE_SIZE) {
    if (!mmu_is_mapped(page))
      return false;
    uint32_t flags = mmu_get_page_flags(page);
    if (!(flags & PAGE_USER))
      return false;
    if ((flags & PAGE_RW) == 0 && page == start_page)
      return false;
  }

  return true;
}

int copy_from_user(void *kernel_dst, uint32_t user_src, size_t size) {
  if (!kernel_dst || !validate_user_pointer(user_src, size)) {
    return -EFAULT;
  }

  char *dst = (char *)kernel_dst;
  char *src = (char *)user_src;

  // Copiar byte por byte con verificación
  for (size_t i = 0; i < size; i++) {
    dst[i] = src[i];
  }

  return size;
}

int copy_to_user(uint32_t user_dst, void *kernel_src, size_t size) {
  if (!kernel_src || !validate_user_pointer(user_dst, size)) {
    return -EFAULT;
  }

  char *dst = (char *)user_dst;
  char *src = (char *)kernel_src;

  for (size_t i = 0; i < size; i++) {
    dst[i] = src[i];
  }

  return size;
}

int copy_string_from_user(char *kernel_dst, uint32_t user_src, size_t max_len) {
  if (!kernel_dst || max_len == 0)
    return -EFAULT;

  for (size_t i = 0; i < max_len; i++) {
    // Verificar cada byte individualmente
    if (!validate_user_pointer(user_src + i, 1)) {
      kernel_dst[i] = '\0';
      return -EFAULT;
    }

    char c = *(char *)(user_src + i);
    kernel_dst[i] = c;

    if (c == '\0') {
      return i;
    }
  }

  // Buffer lleno, asegurar terminación
  kernel_dst[max_len - 1] = '\0';
  return max_len - 1;
}

int copy_string_to_user(uint32_t user_dst, const char *kernel_src,
                        size_t max_len) {
  if (!kernel_src)
    return -EFAULT;

  size_t len = strlen(kernel_src);
  if (len >= max_len)
    len = max_len - 1;

  if (!validate_user_pointer(user_dst, len + 1)) {
    return -EFAULT;
  }

  return copy_to_user(user_dst, (void *)kernel_src, len + 1);
}

// Función auxiliar para verificar si un FD es válido y está abierto
static bool is_valid_fd(int fd) {
  return (fd >= 0 && fd < VFS_MAX_FDS && fd_table[fd] != NULL);
}

// Handler de syscall principal
void syscall_handler(struct regs *r) {
  uint32_t syscall_num = r->eax;
  uint32_t result = 0;

  task_t *current = scheduler.current_task;
  if (!current || !(current->flags & TASK_FLAG_USER_MODE)) {
    r->eax = (uint32_t)-EPERM;
    return;
  }

  switch (syscall_num) {
  // ============================================
  // SYSCALLS EXISTENTES (NO MODIFICADAS)
  // ============================================
  case SYSCALL_EXIT: {
    int exit_code = (int)r->ebx;
    terminal_printf(&main_terminal,
                    "[SYSCALL] Process %u exited with code %d\r\n",
                    current->task_id, exit_code);
    task_exit(exit_code);
    break;
  }

  case SYSCALL_WRITE: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    size_t count = r->edx;

    if (count > 4096) {
      result = (uint32_t)-EINVAL;
      break;
    }

    char *kernel_buffer = (char *)kernel_malloc(count + 1);
    if (!kernel_buffer) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    memset(kernel_buffer, 0, count + 1);

    int copied = copy_from_user(kernel_buffer, buf_ptr, count);
    if (copied < 0 || (size_t)copied != count) {
      kernel_free(kernel_buffer);
      result = (uint32_t)-EFAULT;
      break;
    }

    if (fd == 1 || fd == 2) {
      for (size_t i = 0; i < count; i++) {
        terminal_putchar(&main_terminal, kernel_buffer[i]);
      }
      result = count;
    } else if (fd == 0) {
      result = (uint32_t)-EBADF;
    } else if (is_valid_fd(fd)) {
      int32_t vfs_result = vfs_write(fd, kernel_buffer, count);
      result = (vfs_result >= 0) ? vfs_result : (uint32_t)-EIO;
    } else {
      result = (uint32_t)-EBADF;
    }

    kernel_free(kernel_buffer);
    break;
  }

  case SYSCALL_READ: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    size_t count = r->edx;

    if (!validate_user_pointer(buf_ptr, count)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    char *kernel_buffer = (char *)kernel_malloc(count);
    if (!kernel_buffer) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    size_t bytes_read = 0;

    if (fd == 0) {
      // stdin - lee del teclado
      for (bytes_read = 0; bytes_read < count; bytes_read++) {
        int key = keyboard_getkey_nonblock();
        if (key == -1) {
          // No hay más teclas, esperar un poco
          task_sleep(1);
          if (keyboard_available()) {
            key = keyboard_getkey_nonblock();
          } else {
            break;
          }
        }

        if (key == '\n') {
          kernel_buffer[bytes_read] = '\n';
          bytes_read++;
          break;
        } else if (key == '\b' && bytes_read > 0) {
          bytes_read--;
        } else if (key > 0 && key < 128) {
          kernel_buffer[bytes_read++] = (char)key;
        }
      }
    } else if (fd == 1 || fd == 2) {
      result = (uint32_t)-EBADF;
      kernel_free(kernel_buffer);
      break;
    } else if (is_valid_fd(fd)) {
      int32_t vfs_result = vfs_read(fd, kernel_buffer, count);
      if (vfs_result >= 0) {
        bytes_read = vfs_result;
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

    if (bytes_read > 0) {
      int copied = copy_to_user(buf_ptr, kernel_buffer, bytes_read);
      if (copied < 0) {
        result = (uint32_t)-EFAULT;
      } else {
        result = bytes_read;
      }
    }

    kernel_free(kernel_buffer);
    break;
  }

  case SYSCALL_GETPID:
    result = current->task_id;
    break;

  case SYSCALL_YIELD:
    task_yield();
    result = 0;
    break;

  case SYSCALL_SLEEP:
    task_sleep(r->ebx);
    result = 0;
    break;

  case SYSCALL_GETTIME:
    result = ticks_since_boot;
    break;

  // ============================================
  // NUEVAS SYSCALLS DE TECLADO
  // ============================================
  case SYSCALL_READKEY: {
    // Leer una tecla (bloqueante)
    int key = -1;
    while (key == -1) {
      key = keyboard_getkey_nonblock();
      if (key == -1) {
        task_sleep(10); // Esperar 10ms
      }
    }
    result = (uint32_t)key;
    break;
  }

  case SYSCALL_KEY_AVAILABLE: {
    // Verificar si hay teclas disponibles
    result = (uint32_t)keyboard_available();
    break;
  }

  case SYSCALL_GETC: {
    // Similar a getchar() - lee un carácter
    int key = -1;
    while (key == -1) {
      key = keyboard_getkey_nonblock();
      if (key == -1) {
        task_sleep(10);
      }
    }
    result = (uint32_t)key;
    break;
  }

  case SYSCALL_GETS: {
    // Lee una línea completa
    uint32_t buf_ptr = r->ebx;
    size_t max_len = r->ecx;

    if (!validate_user_pointer(buf_ptr, max_len)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    char *kernel_buffer = (char *)kernel_malloc(max_len);
    if (!kernel_buffer) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    memset(kernel_buffer, 0, max_len);

    size_t pos = 0;
    bool done = false;

    while (!done && pos < max_len - 1) {
      int key = -1;
      while (key == -1) {
        key = keyboard_getkey_nonblock();
        if (key == -1) {
          task_sleep(10);
        }
      }

      if (key == '\n') {
        kernel_buffer[pos] = '\0';
        done = true;
        break;
      } else if (key == '\b') {
        if (pos > 0) {
          pos--;
          // Enviar backspace a la terminal también
          terminal_putchar(&main_terminal, '\b');
        }
      } else if (key >= 32 && key < 127) {
        kernel_buffer[pos++] = (char)key;
        terminal_putchar(&main_terminal, (char)key);
      }
    }

    // Asegurar terminación
    kernel_buffer[max_len - 1] = '\0';

    // Copiar a usuario
    int copied = copy_to_user(buf_ptr, kernel_buffer, max_len);
    if (copied < 0) {
      result = (uint32_t)-EFAULT;
    } else {
      result = (uint32_t)pos;
    }

    kernel_free(kernel_buffer);
    break;
  }

  case SYSCALL_KBHIT: {
    // Verificar si hay tecla sin leer
    result = (uint32_t)keyboard_available();
    break;
  }

  case SYSCALL_KBFLUSH: {
    // Limpiar buffer del teclado
    keyboard_clear_buffer();
    result = 0;
    break;
  }

  // ============================================
  // NUEVAS SYSCALLS DE ARCHIVOS Y DIRECTORIOS
  // ============================================
  case SYSCALL_OPEN: {
    uint32_t path_ptr = r->ebx;
    uint32_t flags = r->ecx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    int fd = vfs_open(kernel_path, flags);
    result = (uint32_t)fd;
    break;
  }

  case SYSCALL_CLOSE: {
    int fd = (int)r->ebx;
    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    if (fd < 3) {
      // No se pueden cerrar stdin/stdout/stderr
      result = (uint32_t)-EBADF;
    } else {
      int ret = vfs_close(fd);
      result = (ret == VFS_OK) ? 0 : (uint32_t)-EBADF;
    }
    break;
  }

  case SYSCALL_GETCWD: {
    uint32_t buf_ptr = r->ebx;
    size_t size = r->ecx;

    if (!validate_user_pointer(buf_ptr, size)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    int copied = copy_string_to_user(buf_ptr, cwd_buffer, size);
    if (copied < 0) {
      result = (uint32_t)-EFAULT;
    } else {
      result = 0;
    }
    break;
  }

  case SYSCALL_CHDIR: {
    uint32_t path_ptr = r->ebx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Verificar que el directorio existe
    const char *rel;
    vfs_superblock_t *sb = find_mount_for_path(kernel_path, &rel);
    if (!sb) {
      result = (uint32_t)-ENOENT;
      break;
    }

    vfs_node_t *node = resolve_path_to_vnode(sb, rel);
    if (!node || node->type != VFS_NODE_DIR) {
      if (node) {
        node->refcount--;
        if (node->refcount == 0 && node->ops->release) {
          node->ops->release(node);
        }
      }
      result = (uint32_t)-ENOTDIR;
      break;
    }

    node->refcount--;
    if (node->refcount == 0 && node->ops->release) {
      node->ops->release(node);
    }

    // Actualizar CWD
    strncpy(cwd_buffer, kernel_path, VFS_PATH_MAX - 1);
    cwd_buffer[VFS_PATH_MAX - 1] = '\0';
    result = 0;
    break;
  }

  case SYSCALL_MKDIR: {
    uint32_t path_ptr = r->ebx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    vfs_node_t *new_dir = NULL;
    int ret = vfs_mkdir(kernel_path, &new_dir);

    if (ret == VFS_OK && new_dir) {
      new_dir->refcount--;
      if (new_dir->refcount == 0 && new_dir->ops->release) {
        new_dir->ops->release(new_dir);
      }
      result = 0;
    } else {
      result = (uint32_t)-EACCES;
    }
    break;
  }

  case SYSCALL_UNLINK: {
    uint32_t path_ptr = r->ebx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    int ret = vfs_unlink(kernel_path);
    result = (ret == VFS_OK) ? 0 : (uint32_t)-EACCES;
    break;
  }

  case SYSCALL_SEEK: {
    int fd = (int)r->ebx;
    int offset = (int)r->ecx;
    int whence = (int)r->edx; // 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_file_t *f = fd_table[fd];

    switch (whence) {
    case 0: // SEEK_SET
      f->offset = offset;
      break;
    case 1: // SEEK_CUR
      f->offset += offset;
      break;
    case 2: // SEEK_END
      // Necesitaríamos el tamaño del archivo
      result = (uint32_t)-ENOSYS;
      break;
    default:
      result = (uint32_t)-EINVAL;
      break;
    }

    if (result != (uint32_t)-ENOSYS && result != (uint32_t)-EINVAL) {
      result = f->offset;
    }
    break;
  }

  case SYSCALL_TELL: {
    int fd = (int)r->ebx;

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_file_t *f = fd_table[fd];
    result = f->offset;
    break;
  }

  // ============================================
  // SYSCALLS DE INFORMACIÓN DEL SISTEMA
  // ============================================
  case SYSCALL_UNAME: {
    uint32_t buf_ptr = r->ebx;

    if (!validate_user_pointer(buf_ptr, sizeof(uname_t))) {
      result = (uint32_t)-EFAULT;
      break;
    }

    uname_t info;
    memset(&info, 0, sizeof(info));

    strcpy(info.sysname, "MicroKernelOS");
    strcpy(info.nodename, "localhost");
    strcpy(info.release, "0.1.0");
    strcpy(info.version, "Built " __DATE__ " " __TIME__);
    strcpy(info.machine, "i386");
    strcpy(info.domainname, "local");

    int copied = copy_to_user(buf_ptr, &info, sizeof(info));
    result = (copied < 0) ? (uint32_t)-EFAULT : 0;
    break;
  }

  // ============================================
  // SYSCALLS STUB - PARA IMPLEMENTACIÓN FUTURA
  // ============================================
  case SYSCALL_STAT:
  case SYSCALL_FORK:
  case SYSCALL_EXECVE:
  case SYSCALL_RMDIR:
  case SYSCALL_IOCTL:
  case SYSCALL_GETPPID:
  case SYSCALL_GETUID:
  case SYSCALL_GETGID:
  case SYSCALL_DUP:
  case SYSCALL_DUP2:
  case SYSCALL_PIPE:
  case SYSCALL_WAITPID:
  case SYSCALL_BRK:
  case SYSCALL_SBRK:
  case SYSCALL_MMAP:
  case SYSCALL_MUNMAP:
  case SYSCALL_GETDENTS:
  case SYSCALL_FSTAT:
  case SYSCALL_FSYNC:
  case SYSCALL_TRUNCATE:
  case SYSCALL_ACCESS:
  case SYSCALL_CHMOD:
  case SYSCALL_CHOWN:
  case SYSCALL_UMASK:
  case SYSCALL_GETRUSAGE:
  case SYSCALL_TIMES:
  case SYSCALL_SYSCONF:
  case SYSCALL_GETPGRP:
  case SYSCALL_SETPGID:
  case SYSCALL_SETSID:
  case SYSCALL_GETSID:
  case SYSCALL_MOUNT:
  case SYSCALL_UMOUNT:
  case SYSCALL_LSEEK:
  case SYSCALL_LINK:
  case SYSCALL_SYMLINK:
  case SYSCALL_READLINK:
  case SYSCALL_RENAME:
  case SYSCALL_FCHDIR:
  case SYSCALL_FCHMOD:
  case SYSCALL_FCHOWN:
  case SYSCALL_UTIME:
  case SYSCALL_SYNC:
    // TODO: Implementar en el futuro
    result = (uint32_t)-ENOSYS;
    break;

  default:
    terminal_printf(&main_terminal,
                    "[SYSCALL] Unknown syscall: 0x%02X (%u)\r\n", syscall_num,
                    syscall_num);
    result = (uint32_t)-ENOSYS;
    break;
  }

  r->eax = result;
}

void syscall_init(void) {
  // Configurar INT 0x80 como puerta de syscall
  idt_set_gate(0x80, (uintptr_t)syscall_entry, 0x08,
               IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_FLAG_INTERRUPT32);

  terminal_puts(&main_terminal, "Syscalls initialized (INT 0x80)\r\n");
}