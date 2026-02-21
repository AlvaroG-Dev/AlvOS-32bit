#include "syscalls.h"
#include "dns.h"
#include "driver_system.h"
#include "idt.h"
#include "irq.h"
#include "kernel.h"
#include "keyboard.h"
#include "mmu.h"
#include "network.h"
#include "rtc.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "tcp.h"
#include "terminal.h"
#include "vfs.h"

extern void syscall_entry(void);

// Declarar variables externas de vfs.c
extern int mount_count;
// Usamos vfs_cwd declarado en vfs.h

// Función auxiliar para verificar si un FD es válido y está abierto
static bool is_valid_fd(int fd) {
  task_t *curr = scheduler.current_task;
  if (!curr || fd < 0 || fd >= VFS_MAX_FDS)
    return false;
  // 0, 1, 2 son siempre válidos (stdin, stdout, stderr)
  if (fd <= 2)
    return true;
  return (curr->fd_table[fd] != NULL);
}

// Funciones auxiliares mejoradas para seguridad
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

  // Simplificación agresiva: si la dirección es de usuario (bajo 3GB)
  // y está mapeada, la damos por buena.
  for (uint32_t page = start_page; page <= end_page; page += PAGE_SIZE) {
    if (!mmu_is_mapped(page))
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
    return -1;
  volatile char *src = (char *)user_src;
  size_t i = 0;
  while (i < max_len - 1) {
    char c = src[i];
    kernel_dst[i] = c;
    if (c == '\0')
      return (int)i;
    i++;
  }
  kernel_dst[i] = '\0';
  return (int)i;
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

// ============================================================================
// SOCKET ADAPTER (VFS WRAPPER)
// ============================================================================

// Forward declarations
static int socket_read(struct vfs_file *f, uint8_t *buf, uint32_t size);
static int socket_write(struct vfs_file *f, const uint8_t *buf, uint32_t size);
static int socket_close(struct vfs_file *f);
static void socket_release(struct vfs_node *node);

static file_ops_t socket_file_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
};

static vnode_ops_t socket_node_ops = {
    .release = socket_release,
    // Otras operaciones pueden ser nulas o default
};

static int socket_read(struct vfs_file *f, uint8_t *buf, uint32_t size) {
  int socket_id = (int)f->node->fs_private;
  return tcp_receive(socket_id, buf, size);
}

static int socket_write(struct vfs_file *f, const uint8_t *buf, uint32_t size) {
  int socket_id = (int)f->node->fs_private;
  int sent = tcp_send(socket_id, buf, size);
  return (sent >= 0) ? sent : -1;
}

static int socket_close(struct vfs_file *f) {
  int socket_id = (int)f->node->fs_private;
  tcp_close(socket_id);
  return VFS_OK;
}

static void socket_release(struct vfs_node *node) {
  // El socket ya se cerró en file->close, aquí liberamos el nodo dummy
  if (node) {
    kernel_free(node);
  }
}

// Crea un FD que envuelve un socket TCP
static int create_socket_fd(int socket_id) {
  task_t *curr = scheduler.current_task;

  // Buscar FD libre
  int fd = -1;
  for (int i = 3; i < VFS_MAX_FDS; i++) {
    if (curr->fd_table[i] == NULL) {
      fd = i;
      break;
    }
  }

  if (fd == -1)
    return -EMFILE;

  // Crear nodo dummy
  vfs_node_t *node = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
  if (!node)
    return -ENOMEM;

  memset(node, 0, sizeof(vfs_node_t));
  strcpy(node->name, "socket");
  node->type = VFS_NODE_SOCKET;
  node->fs_private = (void *)socket_id;
  node->ops = &socket_node_ops;
  node->refcount = 1;

  // Crear archivo
  vfs_file_t *file = (vfs_file_t *)kernel_malloc(sizeof(vfs_file_t));
  if (!file) {
    kernel_free(node);
    return -ENOMEM;
  }

  memset(file, 0, sizeof(vfs_file_t));
  file->node = node;
  file->flags = VFS_O_RDWR;
  file->ops = &socket_file_ops; // Usar ops de socket
  file->refcount = 1;

  curr->fd_table[fd] = file;
  return fd;
}

// Handler de syscall principal
void syscall_handler(struct regs *r) {
  uint32_t syscall_num = r->eax;

  // LOG UNIVERSAL: Comentado para evitar ruido, ya sabemos que el kernel
  // funciona. serial_printf(COM1_BASE, "[SYSCALL] PID %d calling %d
  // (EBX=%d)\r\n",
  //               (scheduler.current_task ? scheduler.current_task->task_id :
  //               0), syscall_num, r->ebx);

  // Habilitar interrupciones para permitir que el timer y el teclado

  // funcionen durante syscalls bloqueantes (como READ)
  __asm__ volatile("sti");

  uint32_t result = 0;

  task_t *current = scheduler.current_task;
  if (!current || !(current->flags & TASK_FLAG_USER_MODE)) {
    r->eax = (uint32_t)-EPERM;
    return;
  }

  switch (syscall_num) {
  // ============================================
  // SYSCALLS ESENCIALES DEL SISTEMA
  // ============================================
  case SYSCALL_EXIT: {
    int exit_code = (int)r->ebx;
    serial_printf(COM1_BASE, "[SYSCALL] Process %u exited with code %d\r\n",
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

    if (fd == 1 || fd == 2) { // stdout/stderr
      // Optimización para salidas grandes (como 'cat')
      for (size_t i = 0; i < count; i++) {
        terminal_putchar(&main_terminal, kernel_buffer[i]);
      }
      terminal_draw(&main_terminal); // Refresh so user sees output streaming!
      task_yield();
      result = count;
    } else if (fd == 0) { // stdin
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

    // --- PRIORIDAD ABSOLUTA: STDIN (TECLADO) ---
    if (fd == 0) {
      serial_printf(COM1_BASE,
                    ">>> SYSCALL_READ ENTERED (KERNEL V1172) <<<\r\n");

      if (current->task_id > 2 &&
          (current->flags & TASK_FLAG_KBD_CLEAN_REQUIRED)) {
        keyboard_clear_buffer();
        current->flags &= ~TASK_FLAG_KBD_CLEAN_REQUIRED;
        serial_printf(COM1_BASE, "[KBD] Cleaned for PID %d\r\n",
                      current->task_id);
      }

      int key = -1;
      while (key <= 0) {
        key = keyboard_getkey_nonblock();
        if (key <= 0) {
          __asm__ volatile("sti");
          task_sleep(5);
        }
      }

      char c = (char)key;
      serial_printf(COM1_BASE, "[KBD] PID %d GOT: %d\r\n", current->task_id,
                    (int)c);

      if (c == '\n' || (c >= 32 && c < 127) || c == '\b') {
        terminal_putchar(&main_terminal, c);
      }

      *((char *)buf_ptr) = c;
      result = 1;
      break;
    }

    // --- ARCHIVOS NORMALES ---
    int32_t bytes_read = 0;
    if (is_valid_fd(fd)) {
      char *kernel_buffer = (char *)kernel_malloc(count);
      if (!kernel_buffer) {
        result = (uint32_t)-ENOMEM;
        break;
      }
      bytes_read = vfs_read(fd, kernel_buffer, (uint32_t)count);

      if (bytes_read > 0) {
        if (validate_user_pointer(buf_ptr, (uint32_t)bytes_read)) {
          memcpy((void *)buf_ptr, kernel_buffer, (size_t)bytes_read);
          result = (uint32_t)bytes_read;
        } else {
          result = (uint32_t)-EFAULT;
        }
      } else {
        result = (uint32_t)bytes_read;
      }
      kernel_free(kernel_buffer);
      task_yield(); // Yielding per read ensures system doesn't hang!
    } else {
      result = (uint32_t)-EBADF;
    }
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
  // SYSCALLS DE TECLADO
  // ============================================
  case SYSCALL_READKEY: {
    // Limpieza agresiva al nacer
    if (current->task_id > 2 &&
        (current->flags & TASK_FLAG_KBD_CLEAN_REQUIRED)) {
      keyboard_clear_buffer();
      task_sleep(40);
      keyboard_clear_buffer();
      current->flags &= ~TASK_FLAG_KBD_CLEAN_REQUIRED;
      serial_printf(COM1_BASE, "[READKEY] KBD Cleaned for PID %d\r\n",
                    current->task_id);
    }

    int key = -1;
    while (key == -1) {
      key = keyboard_getkey_nonblock();
      if (key == -1)
        task_sleep(10);
    }
    serial_printf(COM1_BASE, "[READKEY] Result: %d\r\n", key);
    result = (uint32_t)key;
    break;
  }

  case SYSCALL_KEY_AVAILABLE:
    result = (uint32_t)keyboard_available();
    break;

  case SYSCALL_GETC: {
    if (current->task_id > 2 &&
        (current->flags & TASK_FLAG_KBD_CLEAN_REQUIRED)) {
      keyboard_clear_buffer();
      task_sleep(40);
      keyboard_clear_buffer();
      current->flags &= ~TASK_FLAG_KBD_CLEAN_REQUIRED;
      serial_printf(COM1_BASE, "[GETC] KBD Cleaned for PID %d\r\n",
                    current->task_id);
    }

    int key = -1;
    while (key == -1) {
      key = keyboard_getkey_nonblock();
      if (key == -1)
        task_sleep(10);
    }
    serial_printf(COM1_BASE, "[GETC] Result: %d\r\n", key);
    result = (uint32_t)key;
    break;
  }

  case SYSCALL_GETS: {
    uint32_t buf_ptr = r->ebx;
    size_t max_len = r->ecx;

    if (current->task_id > 2 &&
        (current->flags & TASK_FLAG_KBD_CLEAN_REQUIRED)) {
      keyboard_clear_buffer();
      task_sleep(40);
      keyboard_clear_buffer();
      current->flags &= ~TASK_FLAG_KBD_CLEAN_REQUIRED;
      serial_printf(COM1_BASE, "[GETS] KBD Cleaned for PID %d\r\n",
                    current->task_id);
    }

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
        if (key == -1)
          task_sleep(10);
      }

      if (key == '\n') {
        kernel_buffer[pos] = '\0';
        done = true;
        terminal_putchar(&main_terminal, '\n'); // Echo extra para GETS
        break;
      } else if (key == '\b') {
        if (pos > 0) {
          pos--;
          terminal_putchar(&main_terminal, '\b');
        }
      } else if (key >= 32 && key < 127) {
        kernel_buffer[pos++] = (char)key;
        terminal_putchar(&main_terminal, (char)key);
      }
    }

    kernel_buffer[max_len - 1] = '\0';
    serial_printf(COM1_BASE, "[GETS] Result: %s\r\n", kernel_buffer);
    memcpy((void *)buf_ptr, kernel_buffer, max_len);
    kernel_free(kernel_buffer);
    result = 0;
    break;
  }

  case SYSCALL_KBHIT:
    result = (uint32_t)keyboard_available();
    break;

  case SYSCALL_KBFLUSH:
    keyboard_clear_buffer();
    result = 0;
    break;

  // ============================================
  // SYSCALLS DE ARCHIVOS Y DIRECTORIOS
  // ============================================
  case SYSCALL_OPEN: {
    uint32_t path_ptr = r->ebx;
    uint32_t flags = r->ecx;

    // Usar el heap en lugar del stack para evitar desbordamientos (Stack
    // Overflow)
    char *kernel_path = (char *)kernel_malloc(VFS_PATH_MAX);
    if (!kernel_path) {
      result = (uint32_t)-ENOMEM;
      break;
    }
    memset(kernel_path, 0, VFS_PATH_MAX);

    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      kernel_free(kernel_path);
      result = (uint32_t)-EFAULT;
      break;
    }

    int fd = vfs_open(kernel_path, flags);
    result = (uint32_t)fd;

    kernel_free(kernel_path);
    break;
  }

  case SYSCALL_CLOSE: {
    int fd = (int)r->ebx;
    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    if (fd < 3) { // stdin/stdout/stderr
      result = (uint32_t)-EBADF;
    } else {
      int ret = vfs_close(fd);
      result = (ret == VFS_OK) ? 0 : (uint32_t)-EBADF;
    }
    break;
  }

  case SYSCALL_GETCWD: {
    uint32_t buf_ptr = r->ebx;
    uint32_t size = r->ecx;

    if (!validate_user_pointer(buf_ptr, size)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    if (strlen(current->cwd) >= size) {
      result = (uint32_t)-ERANGE;
      break;
    }

    strcpy((char *)buf_ptr, current->cwd);
    result = 0;
    break;
  }

  case SYSCALL_CHDIR: {
    uint32_t path_ptr = r->ebx;
    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Resolver el path
    vfs_superblock_t *sb;
    const char *rel;
    vfs_node_t *node = vfs_resolve_path(kernel_path, 0, &sb, &rel);

    if (!node) {
      result = (uint32_t)-ENOENT;
      break;
    }

    if (node->type != VFS_NODE_DIR) {
      node->refcount--;
      result = (uint32_t)-ENOTDIR;
      break;
    }

    node->refcount--;
    if (node->refcount == 0 && node->ops->release) {
      node->ops->release(node);
    }

    // Normalizar antes de guardar en la tarea
    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path(kernel_path, normalized, VFS_PATH_MAX) == VFS_OK) {
      strncpy(current->cwd, normalized, VFS_PATH_MAX - 1);
      current->cwd[VFS_PATH_MAX - 1] = '\0';
      result = 0;
    } else {
      result = (uint32_t)-ENOENT;
    }
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

  case SYSCALL_EXECVE: {
    uint32_t path_ptr = r->ebx;
    uint32_t argv_ptr = r->ecx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Parse argv from user space
    int argc = 0;
    char *kernel_argv[32]; // Max 32 arguments
    memset(kernel_argv, 0, sizeof(kernel_argv));

    if (argv_ptr != 0) {
      // argv is an array of char* pointers
      uint32_t *user_argv = (uint32_t *)argv_ptr;

      while (argc < 32) {
        // Validate pointer to argv[argc]
        if (!validate_user_pointer((uint32_t)&user_argv[argc],
                                   sizeof(uint32_t))) {
          break;
        }

        uint32_t str_ptr = user_argv[argc];
        if (str_ptr == 0) { // NULL terminator
          break;
        }

        // Allocate space for this argument
        kernel_argv[argc] = (char *)kernel_malloc(256);
        if (!kernel_argv[argc]) {
          // Free previously allocated args
          for (int i = 0; i < argc; i++) {
            kernel_free(kernel_argv[i]);
          }
          result = (uint32_t)-ENOMEM;
          goto execve_cleanup;
        }

        // Copy the string from user space
        if (copy_string_from_user(kernel_argv[argc], str_ptr, 256) < 0) {
          kernel_free(kernel_argv[argc]);
          for (int i = 0; i < argc; i++) {
            kernel_free(kernel_argv[i]);
          }
          result = (uint32_t)-EFAULT;
          goto execve_cleanup;
        }

        argc++;
      }
    }

    // If no arguments were provided, use just the path
    if (argc == 0) {
      kernel_argv[0] = kernel_path;
      argc = 1;
    }

    extern task_t *exec_load_and_run(int argc, char **argv);
    task_t *new_task = exec_load_and_run(argc, kernel_argv);

    // Free allocated argv strings (but not kernel_path if it was used)
    for (int i = 0; i < argc; i++) {
      if (kernel_argv[i] != kernel_path) {
        kernel_free(kernel_argv[i]);
      }
    }

    if (new_task) {
      new_task->parent = current;
      result = new_task->task_id;
    } else {
      result = (uint32_t)-ENOENT;
    }

  execve_cleanup:
    break;
  }

  case SYSCALL_WAITPID: {
    uint32_t pid = r->ebx;
    uint32_t status_ptr = r->ecx;

    task_t *target = task_find_by_id(pid);
    if (!target) {
      result = (uint32_t)-ECHILD;
      break;
    }

    if (target->state == TASK_FINISHED || target->state == TASK_ZOMBIE) {
      if (status_ptr && validate_user_pointer(status_ptr, 4)) {
        *(int *)status_ptr = target->exit_code;
      }
      result = pid;
    } else {
      current->state = TASK_WAITING;
      current->wait_for_pid = pid;
      task_yield();

      if (status_ptr && validate_user_pointer(status_ptr, 4)) {
        *(int *)status_ptr = target->exit_code;
      }
      result = pid;
    }
    break;
  }

  case SYSCALL_SEEK: {
    int fd = (int)r->ebx;
    int offset = (int)r->ecx;
    int whence = (int)r->edx;

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_file_t *f = current->fd_table[fd];

    switch (whence) {
    case 0: // SEEK_SET
      f->offset = offset;
      result = f->offset;
      break;
    case 1: // SEEK_CUR
      f->offset += offset;
      result = f->offset;
      break;
    case 2: { // SEEK_END - Asumir tamaño del archivo si es posible
      vfs_dirent_t stat;
      if (f->node->ops->getattr) {
        f->node->ops->getattr(f->node, &stat);
        f->offset = stat.size + offset;
        result = f->offset;
      } else {
        result = (uint32_t)-ENOSYS;
      }
      break;
    }
    default:
      result = (uint32_t)-EINVAL;
      break;
    }
    break;
  }

  case SYSCALL_TELL: {
    int fd = (int)r->ebx;

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_file_t *f = current->fd_table[fd];
    result = f->offset;
    break;
  }

  // ============================================
  // SYSCALLS DE DISPOSITIVOS (IOCTL)
  // ============================================
  case SYSCALL_IOCTL: {
    uint32_t ioctl_info_ptr = r->ebx;

    // Estructura simple para ioctl
    struct {
      char name[DRIVER_NAME_MAX];
      uint32_t cmd;
      uint32_t arg_size;
      uint8_t arg[256];
    } ioctl_info;

    // Copiar desde usuario
    if (copy_from_user(&ioctl_info, ioctl_info_ptr, sizeof(ioctl_info)) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Buscar driver
    driver_instance_t *drv = driver_find_by_name(ioctl_info.name);
    if (!drv) {
      result = (uint32_t)-ENODEV;
      break;
    }

    // Verificar que esté activo
    if (drv->state != DRIVER_STATE_ACTIVE) {
      result = (uint32_t)-EBUSY;
      break;
    }

    // Verificar que tenga ioctl
    if (!drv->ops || !drv->ops->ioctl) {
      result = (uint32_t)-ENOTTY;
      break;
    }

    // Ejecutar ioctl
    void *arg = (ioctl_info.arg_size > 0) ? ioctl_info.arg : NULL;
    result = drv->ops->ioctl(drv, ioctl_info.cmd, arg);
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
    strcpy(info.release, "0.2.0");
    strcpy(info.version, "Built " __DATE__ " " __TIME__);
    strcpy(info.machine, "i386");
    strcpy(info.domainname, "local");

    int copied = copy_to_user(buf_ptr, &info, sizeof(info));
    result = (copied < 0) ? (uint32_t)-EFAULT : 0;
    break;
  }

  // ============================================
  // SYSCALLS DE RED 🌐
  // ============================================
  case SYSCALL_DNS_RESOLVE: {
    uint32_t host_ptr = r->ebx;
    uint32_t ip_ptr = r->ecx;
    char kernel_host[256];
    if (copy_string_from_user(kernel_host, host_ptr, 256) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }
    ip_addr_t server_ip;
    if (dns_resolve(kernel_host, server_ip)) {
      if (copy_to_user(ip_ptr, server_ip, 4) < 0)
        result = (uint32_t)-EFAULT;
      else
        result = 0;
    } else
      result = (uint32_t)-ENOENT;
    break;
  }

  case SYSCALL_CONNECT: {
    uint32_t ip_ptr = r->ebx;
    uint16_t port = (uint16_t)r->ecx;
    ip_addr_t server_ip;
    if (copy_from_user(server_ip, ip_ptr, 4) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }
    int socket_id = tcp_connect(server_ip, port);

    if (socket_id >= 0) {
      // Envolver en FD
      int fd = create_socket_fd(socket_id);
      if (fd >= 0) {
        result = (uint32_t)fd;
      } else {
        tcp_close(socket_id); // Falló crear FD
        result = (uint32_t)-ENOMEM;
      }
    } else {
      result = (uint32_t)-ECONNREFUSED;
    }
    break;
  }

  case SYSCALL_SEND: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    uint32_t len = r->edx;

    if (!validate_user_pointer(buf_ptr, len)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Verificar que sea un socket válido
    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }
    vfs_file_t *f = scheduler.current_task->fd_table[fd];
    if (f->node->type != VFS_NODE_SOCKET) {
      result = (uint32_t)-ENOTSOCK;
      break;
    }

    // Usar la lógica de socket_write o llamar directo
    // Podemos usar f->ops->write directamente!
    uint8_t *kernel_buf = kernel_malloc(len);
    if (!kernel_buf) {
      result = (uint32_t)-ENOMEM;
      break;
    }
    if (copy_from_user(kernel_buf, buf_ptr, len) < 0) {
      kernel_free(kernel_buf);
      result = (uint32_t)-EFAULT;
      break;
    }

    // Llamada directa o via ops
    int sent = f->ops->write(f, kernel_buf, len);
    kernel_free(kernel_buf);
    result = (sent >= 0) ? (uint32_t)sent : (uint32_t)-EIO;
    break;
  }

  case SYSCALL_RECV: {
    int fd = (int)r->ebx;
    uint32_t buf_ptr = r->ecx;
    uint32_t len = r->edx;
    if (!validate_user_pointer(buf_ptr, len)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }
    vfs_file_t *f = scheduler.current_task->fd_table[fd];
    if (f->node->type != VFS_NODE_SOCKET) {
      result = (uint32_t)-ENOTSOCK;
      break;
    }

    uint8_t *kernel_buf = kernel_malloc(len);
    if (!kernel_buf) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    int received = f->ops->read(f, kernel_buf, len);

    if (received > 0) {
      if (copy_to_user(buf_ptr, kernel_buf, received) < 0)
        result = (uint32_t)-EFAULT;
      else
        result = (uint32_t)received;
    } else if (received == -2)
      result = 0; // EOF
    else
      result = (uint32_t)-EAGAIN;
    kernel_free(kernel_buf);
    break;
  }

  case SYSCALL_RTC_GET_DATETIME: {
    uint32_t buf_ptr = r->ebx;
    if (!validate_user_pointer(buf_ptr, sizeof(rtc_time_t))) {
      result = (uint32_t)-EFAULT;
      break;
    }

    rtc_time_t time;
    rtc_get_time(&time);
    result = (uint32_t)copy_to_user(buf_ptr, &time, sizeof(rtc_time_t));
    if ((int32_t)result >= 0)
      result = 0;
    break;
  }

  case SYSCALL_RMDIR: {
    uint32_t path_ptr = r->ebx;
    char path[VFS_PATH_MAX];
    if (copy_string_from_user(path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }
    int ret = vfs_rmdir(path);
    result = (ret == VFS_OK) ? 0 : (uint32_t)-EACCES;
    break;
  }

  case SYSCALL_RENAME: {
    uint32_t old_ptr = r->ebx;
    uint32_t new_ptr = r->ecx;
    char old_path[VFS_PATH_MAX], new_path[VFS_PATH_MAX];
    if (copy_string_from_user(old_path, old_ptr, VFS_PATH_MAX) < 0 ||
        copy_string_from_user(new_path, new_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }
    int ret = vfs_rename(old_path, new_path);
    result = (ret == VFS_OK) ? 0 : (uint32_t)-EACCES;
    break;
  }

  case SYSCALL_FORK:
    // Stub: No soportamos fork real aún (requiere clonar memoria)
    result = (uint32_t)-ENOSYS;
    break;

  case SYSCALL_GETPPID:
    result = current->parent ? current->parent->task_id : 0;
    break;

  case SYSCALL_GETUID:
  case SYSCALL_GETGID:
    result = 0; // Por ahora root
    break;

  case SYSCALL_DUP: {
    // Stub: No soportamos dup aún
    result = (uint32_t)-ENOSYS;
    break;
  }

  case SYSCALL_SBRK: {
    int32_t incr = (int32_t)r->ebx;
    if (!current->address_space) {
      result = (uint32_t)-ENOMEM;
      break;
    }

    uint32_t old_brk = current->address_space->heap_current;
    if (incr == 0) {
      result = old_brk;
    } else {
      void *new_brk_ptr =
          vmm_brk(current->address_space, (void *)(old_brk + incr));
      if (new_brk_ptr == (void *)-1) {
        result = (uint32_t)-ENOMEM;
      } else {
        result = old_brk; // Newlib espera el break ANTERIOR
      }
    }
    break;
  }

  case SYSCALL_FSTAT: {
    int fd = (int)r->ebx;
    uint32_t stat_ptr = r->ecx;

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_stat_t st;
    memset(&st, 0, sizeof(st));
    vfs_file_t *f = current->fd_table[fd];

    st.st_mode = (f->node->type == VFS_NODE_DIR) ? 0040000 : 0100000;
    if (f->node->type == VFS_NODE_CHRDEV)
      st.st_mode = 0020000;

    vfs_dirent_t vstat;
    if (f->node->ops->getattr) {
      f->node->ops->getattr(f->node, &vstat);
      st.st_size = vstat.size;
    }

    if (copy_to_user(stat_ptr, &st, sizeof(st)) < 0)
      result = (uint32_t)-EFAULT;
    else
      result = 0;
    break;
  }

  case SYSCALL_STAT: {
    uint32_t path_ptr = r->ebx;
    uint32_t stat_ptr = r->ecx;
    char path[VFS_PATH_MAX];
    if (copy_string_from_user(path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    vfs_dirent_t vstat;
    if (vfs_stat(path, &vstat) == VFS_OK) {
      vfs_stat_t st;
      memset(&st, 0, sizeof(st));
      st.st_mode = (vstat.type == VFS_NODE_DIR) ? 0040000 : 0100000;
      st.st_size = vstat.size;
      if (copy_to_user(stat_ptr, &st, sizeof(st)) < 0)
        result = (uint32_t)-EFAULT;
      else
        result = 0;
    } else {
      result = (uint32_t)-ENOENT;
    }
    break;
  }

  case SYSCALL_TIMES: {
    uint32_t buf_ptr = r->ebx;
    struct {
      uint32_t tms_utime;
      uint32_t tms_stime;
      uint32_t tms_cutime;
      uint32_t tms_cstime;
    } tms;
    tms.tms_utime = current->total_runtime;
    tms.tms_stime = 0;
    tms.tms_cutime = 0;
    tms.tms_cstime = 0;
    if (validate_user_pointer(buf_ptr, sizeof(tms))) {
      copy_to_user(buf_ptr, &tms, sizeof(tms));
      result = ticks_since_boot;
    } else {
      result = (uint32_t)-EFAULT;
    }
    break;
  }

  case SYSCALL_LSEEK: {
    int fd = (int)r->ebx;
    int offset = (int)r->ecx;
    int whence = (int)r->edx;

    if (!is_valid_fd(fd)) {
      result = (uint32_t)-EBADF;
      break;
    }

    vfs_file_t *f = current->fd_table[fd];
    switch (whence) {
    case 0:
      f->offset = offset;
      break;
    case 1:
      f->offset += offset;
      break;
    case 2: {
      vfs_dirent_t vstat;
      if (f->node->ops->getattr) {
        f->node->ops->getattr(f->node, &vstat);
        f->offset = vstat.size + offset;
      }
      break;
    }
    }
    result = f->offset;
    break;
  }
  case SYSCALL_GETDENTS: {
    // ebx = path pointer, ecx = user buffer pointer, edx = buffer size
    uint32_t path_ptr = r->ebx;
    uint32_t buf_ptr = r->ecx;
    uint32_t buf_size = r->edx;

    char kernel_path[VFS_PATH_MAX];
    if (copy_string_from_user(kernel_path, path_ptr, VFS_PATH_MAX) < 0) {
      result = (uint32_t)-EFAULT;
      break;
    }

    if (!validate_user_pointer(buf_ptr, buf_size)) {
      result = (uint32_t)-EFAULT;
      break;
    }

    // Resolve path to node
    const char *relpath;
    vfs_superblock_t *sb = find_mount_for_path(kernel_path, &relpath);
    if (!sb) {
      result = (uint32_t)-ENOENT;
      break;
    }

    vfs_node_t *dir = resolve_path_to_vnode(sb, relpath);
    if (!dir) {
      result = (uint32_t)-ENOENT;
      break;
    }

    if (dir->type != VFS_NODE_DIR) {
      dir->refcount--;
      if (dir->refcount == 0 && dir->ops && dir->ops->release)
        dir->ops->release(dir);
      result = (uint32_t)-ENOTDIR;
      break;
    }

    if (!dir->ops || !dir->ops->readdir) {
      dir->refcount--;
      if (dir->refcount == 0 && dir->ops && dir->ops->release)
        dir->ops->release(dir);
      result = (uint32_t)-ENOSYS;
      break;
    }

    // Read directory entries (max 64 at a time)
    uint32_t max_entries = 64;
    vfs_dirent_t *dirents =
        (vfs_dirent_t *)kernel_malloc(sizeof(vfs_dirent_t) * max_entries);
    if (!dirents) {
      dir->refcount--;
      if (dir->refcount == 0 && dir->ops && dir->ops->release)
        dir->ops->release(dir);
      result = (uint32_t)-ENOMEM;
      break;
    }

    uint32_t count = max_entries;
    int ret = dir->ops->readdir(dir, dirents, &count, 0);

    dir->refcount--;
    if (dir->refcount == 0 && dir->ops && dir->ops->release)
      dir->ops->release(dir);

    if (ret != 0) {
      kernel_free(dirents);
      result = (uint32_t)-EIO;
      break;
    }

    // Pack entries into user buffer
    // Format per entry: [1 byte type][null-terminated name string]
    uint32_t offset = 0;
    uint8_t *kernel_buf = (uint8_t *)kernel_malloc(buf_size);
    if (!kernel_buf) {
      kernel_free(dirents);
      result = (uint32_t)-ENOMEM;
      break;
    }
    memset(kernel_buf, 0, buf_size);

    // First 4 bytes: entry count
    if (buf_size < 4) {
      kernel_free(kernel_buf);
      kernel_free(dirents);
      result = (uint32_t)-EINVAL;
      break;
    }

    uint32_t *count_ptr = (uint32_t *)kernel_buf;
    *count_ptr = count;
    offset = 4;

    for (uint32_t i = 0; i < count && offset < buf_size - 2; i++) {
      uint32_t name_len = strlen(dirents[i].name);
      // Need: 1 byte type + name_len + 1 null terminator + 4 bytes size
      if (offset + 1 + name_len + 1 + 4 > buf_size)
        break;
      kernel_buf[offset++] = dirents[i].type;
      memcpy(kernel_buf + offset, dirents[i].name, name_len + 1);
      offset += name_len + 1;
      // Add file size (4 bytes, little endian)
      uint32_t fsize = dirents[i].size;
      memcpy(kernel_buf + offset, &fsize, 4);
      offset += 4;
    }

    if (copy_to_user(buf_ptr, kernel_buf, offset) < 0) {
      result = (uint32_t)-EFAULT;
    } else {
      result = offset; // Return bytes written
    }

    kernel_free(kernel_buf);
    kernel_free(dirents);
    break;
  }

  case SYSCALL_LINK:
  case SYSCALL_SYMLINK:
  case SYSCALL_READLINK:
  case SYSCALL_FCHDIR:
  case SYSCALL_FCHMOD:
  case SYSCALL_FCHOWN:
  case SYSCALL_UTIME:
  case SYSCALL_SYNC:
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