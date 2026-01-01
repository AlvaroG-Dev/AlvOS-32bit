// vfs.c - Fixed version with proper mount point handling
#include "vfs.h"
#include "boot_log.h"
#include "disk.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"
#include <stdint.h>

/* Usar tus allocators */
extern void *kernel_malloc(size_t size);
extern int kernel_free(void *p);
extern Terminal main_terminal;

/* Tables */
static vfs_fs_type_t fs_table[VFS_MAX_FS_TYPES];
static int fs_count = 0;

/* NUEVO: Tabla de montajes como lista enlazada */
vfs_mount_info_t *mount_list = NULL;
int mount_count = 0;

/* FD table */
vfs_file_t *fd_table[VFS_MAX_FDS];

/* Helpers for allocation */
static void *vfs_alloc(size_t s) { return kernel_malloc(s); }
static void vfs_free(void *p) { kernel_free(p); }

// Función auxiliar para cerrar FDs asociados a un superblock
int close_fds_for_mount(vfs_superblock_t *sb) {
  int closed = 0;
  for (int i = 0; i < VFS_MAX_FDS; i++) {
    if (fd_table[i] && fd_table[i]->node && fd_table[i]->node->sb == sb) {
      if (vfs_close(i) != VFS_OK) {
        terminal_printf(&main_terminal, "shutdown: Failed to close FD %d\r\n",
                        i);
        serial_write_string(COM1_BASE, "shutdown: Failed to close FD\r\n");
        return VFS_ERR;
      }
      closed++;
    }
  }
  if (closed > 0) {
    serial_write_string(COM1_BASE,
                        "shutdown: Closed open file descriptors\r\n");
  }
  return VFS_OK;
}

/* Normalize a path: remove redundant slashes, resolve . and .. */
int vfs_normalize_path(const char *input, char *output, size_t output_size) {
  if (!input || !output || output_size < VFS_PATH_MAX)
    return VFS_ERR;

  char temp[VFS_PATH_MAX];
  size_t temp_idx = 0;

  // Start with root
  temp[0] = '/';
  temp_idx = 1;

  // Process input path
  size_t i = 0;
  while (input[i] == '/')
    i++; // Skip leading slashes
  while (i < strlen(input) && temp_idx < VFS_PATH_MAX - 1) {
    if (input[i] == '/') {
      // Skip multiple slashes
      while (input[i] == '/' && i < strlen(input))
        i++;
      if (i >= strlen(input))
        break;
      if (temp_idx > 1 && temp[temp_idx - 1] != '/')
        temp[temp_idx++] = '/';
    } else if (input[i] == '.' &&
               (input[i + 1] == '/' || input[i + 1] == '\0')) {
      // Handle .
      i++;
      if (input[i] == '/')
        i++;
    } else if (input[i] == '.' && input[i + 1] == '.' &&
               (input[i + 2] == '/' || input[i + 2] == '\0')) {
      // Handle ..
      i += 2;
      if (input[i] == '/')
        i++;
      if (temp_idx > 1) {
        temp_idx--; // Remove trailing /
        while (temp_idx > 1 && temp[temp_idx - 1] != '/')
          temp_idx--; // Backtrack to previous /
      }
    } else {
      temp[temp_idx++] = input[i++];
    }
  }

  // Remove trailing slash unless root
  if (temp_idx > 1 && temp[temp_idx - 1] == '/')
    temp_idx--;

  temp[temp_idx] = '\0';
  if (temp_idx == 1 && temp[0] == '/') {
    // Root case
    strncpy(output, "/", output_size);
  } else {
    strncpy(output, temp, output_size);
  }
  output[output_size - 1] = '\0';

  return VFS_OK;
}

/* Register FS */
int vfs_register_fs(const vfs_fs_type_t *fs) {
  if (!fs)
    return VFS_ERR;
  unsigned int f = vfs_lock_disable_irq();
  if (fs_count >= VFS_MAX_FS_TYPES) {
    vfs_unlock_restore_irq(f);
    return VFS_ERR;
  }
  fs_table[fs_count++] = *fs;
  vfs_unlock_restore_irq(f);
  return VFS_OK;
}

/* Init */
void vfs_init(void) {
  unsigned int f = vfs_lock_disable_irq();
  fs_count = 0;
  mount_count = 0;

  for (int i = 0; i < VFS_MAX_FDS; i++)
    fd_table[i] = NULL;

  // Limpiar lista de montajes
  vfs_mount_info_t *current = mount_list;
  while (current) {
    vfs_mount_info_t *next = current->next;
    vfs_free(current);
    current = next;
  }
  mount_list = NULL;

  vfs_unlock_restore_irq(f);
}

/* find fs by name */
static vfs_fs_type_t *find_fs(const char *name) {
  for (int i = 0; i < fs_count; i++) {
    if (strcmp(fs_table[i].name, name) == 0)
      return &fs_table[i];
  }
  return NULL;
}

/* Split path into parent directory and final component */
int vfs_split_path(const char *path, char *parent_path, char *name) {
  if (!path || !parent_path || !name)
    return VFS_ERR;

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    return VFS_ERR;
  }

  strncpy(parent_path, normalized, VFS_PATH_MAX - 1);
  parent_path[VFS_PATH_MAX - 1] = '\0';

  char *last_slash = strrchr(parent_path, '/');
  if (!last_slash) {
    return VFS_ERR;
  }

  if (last_slash == parent_path) {
    // Mount point is directly under root (e.g., "/ramfs")
    strcpy(name, parent_path + 1);
    strcpy(parent_path, "/");
  } else {
    // Mount point is nested (e.g., "/ramfs/subdir")
    strcpy(name, last_slash + 1);
    *last_slash = '\0';
  }

  return VFS_OK;
}

/* Find the mount point for a path */
vfs_superblock_t *find_mount_for_path(const char *path,
                                      const char **out_relpath) {
  if (!path || !out_relpath)
    return NULL;

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    terminal_printf(&main_terminal, "VFS: Failed to normalize path %s\r\n",
                    path);
    return NULL;
  }

  unsigned int f = vfs_lock_disable_irq();

  vfs_superblock_t *best_sb = NULL;
  int best_len = -1;
  const char *best_relpath = NULL;
  char best_mountpoint[VFS_PATH_MAX] = "";

  vfs_mount_info_t *current = mount_list;
  while (current) {
    char normalized_mount[VFS_PATH_MAX];
    if (vfs_normalize_path(current->mountpoint, normalized_mount,
                           VFS_PATH_MAX) != VFS_OK) {
      current = current->next;
      continue;
    }

    int mount_len = strlen(normalized_mount);

    if (strcmp(normalized_mount, "/") == 0) {
      if (best_len == -1) {
        best_sb = current->sb;
        best_len = 0;
        best_relpath = normalized + 1;
        if (*best_relpath == '\0')
          best_relpath = "";
        strncpy(best_mountpoint, normalized_mount, VFS_PATH_MAX - 1);
      }
    } else {
      if (strncmp(normalized, normalized_mount, mount_len) == 0 &&
          (normalized[mount_len] == '\0' || normalized[mount_len] == '/')) {
        if (mount_len > best_len) {
          best_sb = current->sb;
          best_len = mount_len;
          best_relpath = normalized + mount_len;
          if (*best_relpath == '/')
            best_relpath++;
          if (*best_relpath == '\0')
            best_relpath = "";
          strncpy(best_mountpoint, normalized_mount, VFS_PATH_MAX - 1);
        }
      }
    }
    current = current->next;
  }

  if (!best_sb) {
    terminal_printf(&main_terminal, "VFS: No mount found for path %s\r\n",
                    normalized);
    vfs_unlock_restore_irq(f);
    return NULL;
  }

  /* serial_printf(COM1_BASE, "VFS: Matched mount %s for path %s (fs: %s)\r\n",
                best_mountpoint, normalized, best_sb->fs_name); */

  *out_relpath = best_relpath;
  vfs_unlock_restore_irq(f);
  return best_sb;
}

/* Mount with proper error handling - FIXED VERSION */
int vfs_mount(const char *mountpoint, const char *fsname, void *device) {
  if (!mountpoint || !fsname)
    return VFS_ERR;

  terminal_printf(&main_terminal, "VFS: Mount attempt %s on %s...\n", fsname, mountpoint);

  unsigned int f = vfs_lock_disable_irq();

  // Check if mount point is already mounted
  vfs_mount_info_t *current = mount_list;
  while (current) {
    if (strcmp(current->mountpoint, mountpoint) == 0) {
      vfs_unlock_restore_irq(f);
      return VFS_ERR; // Already mounted
    }
    current = current->next;
  }

  vfs_unlock_restore_irq(f);

  // **NUEVO: Verificar si ya hay un mount para este dispositivo**
  // **PERO SOLO SI device NO ES NULL (tmpfs tiene device = NULL)**
  vfs_superblock_t *existing_sb = NULL;
  vfs_mount_info_t *existing_mount = NULL;

  if (device != NULL) {
    // Solo buscar superblocks existentes para dispositivos reales
    current = mount_list;
    while (current) {
      // Verificar si es el mismo dispositivo y filesystem
      if (current->sb && current->sb->backing_device == device &&
          strcmp(current->fs_type, fsname) == 0) {
        existing_sb = current->sb;
        existing_mount = current;
        terminal_printf(&main_terminal,
                        "VFS: Found existing %s mount for same device, reusing "
                        "superblock\r\n",
                        fsname);
        break;
      }
      current = current->next;
    }
  } else {
    // Para tmpfs y otros filesystems sin dispositivo, siempre crear nuevo
    // superblock
    terminal_printf(
        &main_terminal,
        "VFS: Creating new superblock for %s (no backing device)\r\n", fsname);
  }

  // Create mount point directory if it's not root and doesn't exist
  if (strcmp(mountpoint, "/") != 0) {
    const char *relpath;
    vfs_superblock_t *parent_sb = find_mount_for_path(mountpoint, &relpath);
    if (!parent_sb) {
      return VFS_ERR;
    }

    terminal_printf(&main_terminal, "VFS: Parent SB found for %s\n", mountpoint);

    vfs_node_t *mount_dir = resolve_path_to_vnode(parent_sb, relpath);
    if (!mount_dir) {
      terminal_printf(&main_terminal, "VFS: Node %s not found, creating...\n", relpath);
      // Mount point doesn't exist, create it
      char parent_path[VFS_PATH_MAX];
      char name[VFS_NAME_MAX];
      if (vfs_split_path(mountpoint, parent_path, name) != VFS_OK) {
        return VFS_ERR;
      }

      const char *parent_relpath;
      vfs_superblock_t *parent_sb_for_parent =
          find_mount_for_path(parent_path, &parent_relpath);
      if (!parent_sb_for_parent) {
        return VFS_ERR;
      }

      vfs_node_t *parent_dir =
          resolve_path_to_vnode(parent_sb_for_parent, parent_relpath);
      if (!parent_dir || parent_dir->type != VFS_NODE_DIR ||
          !parent_dir->ops->mkdir) {
        if (parent_dir) {
          parent_dir->refcount--;
          if (parent_dir->refcount == 0 && parent_dir->ops->release) {
            parent_dir->ops->release(parent_dir);
          }
        }
        return VFS_ERR;
      }

      vfs_node_t *new_dir = NULL;
      if (parent_dir->ops->mkdir(parent_dir, name, &new_dir) != VFS_OK ||
          !new_dir) {
        parent_dir->refcount--;
        if (parent_dir->refcount == 0 && parent_dir->ops->release) {
          parent_dir->ops->release(parent_dir);
        }
        return VFS_ERR;
      }
      terminal_printf(&main_terminal, "VFS: Directory %s created\n", mountpoint);

      // Release the new directory node
      new_dir->refcount--;
      if (new_dir->refcount == 0 && new_dir->ops->release) {
        new_dir->ops->release(new_dir);
      }

      // Release parent directory node
      parent_dir->refcount--;
      if (parent_dir->refcount == 0 && parent_dir->ops->release) {
        parent_dir->ops->release(parent_dir);
      }
    } else {
      // Mount point exists, check if it's a directory
      if (mount_dir->type != VFS_NODE_DIR) {
        mount_dir->refcount--;
        if (mount_dir->refcount == 0 && mount_dir->ops->release) {
          mount_dir->ops->release(mount_dir);
        }
        return VFS_ERR; // Can't mount over a file
      }
      // Release reference
      mount_dir->refcount--;
      if (mount_dir->refcount == 0 && mount_dir->ops->release) {
        mount_dir->ops->release(mount_dir);
      }
    }
  }

  vfs_superblock_t *sb = NULL;

  if (existing_sb) {
    // **REUTILIZAR SUPERBLOCK EXISTENTE**
    sb = existing_sb;
    sb->refcount++; // Incrementar contador de referencias

  } else {
    // **CREAR NUEVO SUPERBLOCK**
    vfs_fs_type_t *fst = find_fs(fsname);
    if (!fst || fst->mount(device, &sb) != VFS_OK || !sb) {
      return VFS_ERR;
    }
    sb->refcount = 1;
    sb->backing_device =
        device; // Guardar referencia al dispositivo (puede ser NULL para tmpfs)
  }

  // Create mount info
  vfs_mount_info_t *mount_info =
      (vfs_mount_info_t *)vfs_alloc(sizeof(vfs_mount_info_t));
  if (!mount_info) {
    // Cleanup sb - solo si nadie más lo está usando
    if (sb->refcount == 1) {
      if (sb->priv)         // Changed from sb->private to sb->priv
        vfs_free(sb->priv); // Changed from sb->private to sb->priv
      vfs_free(sb);
    } else {
      sb->refcount--; // Decrementar ya que no vamos a usarlo
    }
    return VFS_ERR;
  }

  memset(mount_info, 0, sizeof(vfs_mount_info_t));
  mount_info->sb = sb;
  strncpy(mount_info->mountpoint, mountpoint, VFS_PATH_MAX - 1);
  mount_info->mountpoint[VFS_PATH_MAX - 1] = '\0';
  strncpy(mount_info->fs_type, fsname, sizeof(mount_info->fs_type) - 1);

  // **MARCAR COMO BIND MOUNT SI ES REUTILIZACIÓN**
  if (existing_sb) {
    mount_info->flags = VFS_MOUNT_BIND;
    if (existing_mount) {
      strncpy(mount_info->source, existing_mount->mountpoint, VFS_PATH_MAX - 1);
    }
  } else {
    mount_info->flags = 0;
  }

  // Agregar a lista
  f = vfs_lock_disable_irq();
  mount_info->next = mount_list;
  mount_list = mount_info;
  mount_count++;
  vfs_unlock_restore_irq(f);

  terminal_printf(
      &main_terminal, "VFS: Mounted %s at %s (refcount: %u, device: %s)\r\n",
      fsname, mountpoint, sb->refcount, device ? "present" : "none");

  return VFS_OK;
}

/* next_component - FIXED VERSION */
static char *next_component(char **p) {
  char *s = *p;
  if (!s || *s == 0)
    return NULL;

  // Skip leading slashes
  while (*s == '/')
    s++;
  if (*s == 0)
    return NULL;

  char *start = s;
  while (*s && *s != '/')
    s++;

  if (*s == '/') {
    *s = 0;
    s++;
    // Skip multiple slashes
    while (*s == '/')
      s++;
  }
  *p = s;
  return start;
}

/* resolve path within superblock - FIXED VERSION */
vfs_node_t *resolve_path_to_vnode(vfs_superblock_t *sb, const char *relpath) {
  if (!sb || !sb->root)
    return NULL;
  if (!relpath || relpath[0] == 0) {
    sb->root->refcount++;
    return sb->root;
  }

  char tmp[VFS_PATH_MAX];
  strncpy(tmp, relpath, VFS_PATH_MAX - 1);
  tmp[VFS_PATH_MAX - 1] = 0;

  char *p = tmp;
  vfs_node_t *cur = sb->root;
  cur->refcount++;

  char *comp;
  while ((comp = next_component(&p)) != NULL) {
    if (cur->type != VFS_NODE_DIR || !cur->ops || !cur->ops->lookup) {
      cur->refcount--;
      if (cur->refcount == 0 && cur->ops && cur->ops->release) {
        cur->ops->release(cur);
      }
      return NULL;
    }
    vfs_node_t *next = NULL;
    int r = cur->ops->lookup(cur, comp, &next);
    cur->refcount--;
    if (cur->refcount == 0 && cur->ops && cur->ops->release) {
      cur->ops->release(cur);
    }
    if (r != VFS_OK || !next)
      return NULL;
    cur = next;
    // next already has refcount of 1 from lookup
  }
  return cur;
}

static int allocate_fd(vfs_file_t *f) {
  for (int i = 0; i < VFS_MAX_FDS; i++) {
    if (!fd_table[i]) {
      fd_table[i] = f;
      return i;
    }
  }
  return -1;
}

/* free fd */
static void free_fd(int fd) {
  if (fd < 0 || fd >= VFS_MAX_FDS)
    return;
  fd_table[fd] = NULL;
}

int vfs_list_mounts(void (*callback)(const char *mountpoint,
                                     const char *fs_name, void *arg),
                    void *arg) {
  if (!callback)
    return VFS_ERR;

  unsigned int f = vfs_lock_disable_irq();
  int count = 0;

  vfs_mount_info_t *current = mount_list;
  while (current) {
    callback(current->mountpoint, current->sb->fs_name, arg);
    count++;
    current = current->next;
  }

  vfs_unlock_restore_irq(f);
  return count;
}

int vfs_open(const char *path, uint32_t flags) {
  if (!path || strnlen(path, VFS_PATH_MAX + 1) > VFS_PATH_MAX) {
    serial_printf(COM1_BASE, "vfs_open: Invalid path=%p, len=%u\n", path,
                  path ? strnlen(path, VFS_PATH_MAX + 1) : 0);
    return -1;
  }
  for (size_t i = 0; i < strnlen(path, VFS_PATH_MAX + 1); i++) {
    if (path[i] < 0x20 || path[i] > 0x7E) {
      serial_printf(
          COM1_BASE,
          "vfs_open: Invalid character 0x%02X in path at position %u\n",
          (unsigned char)path[i], i);
      return -1;
    }
  }
  serial_printf(COM1_BASE, "vfs_open: Opening path: %s with flags 0x%x\n", path,
                flags);
  debug_hex_dump("Path input", path, strnlen(path, VFS_PATH_MAX + 1));

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    serial_printf(COM1_BASE, "vfs_open: Failed to normalize path %s\n", path);
    return -1;
  }
  debug_hex_dump("Normalized path", normalized,
                 strnlen(normalized, VFS_PATH_MAX));

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(normalized, &rel);
  if (!sb || !rel) {
    serial_printf(COM1_BASE, "vfs_open: No mount found for %s\n", normalized);
    return -1;
  }
  serial_printf(COM1_BASE, "vfs_open: Mountpoint found, relative path: %s\n",
                rel);
  debug_hex_dump("Relative path", rel, strnlen(rel, VFS_PATH_MAX));

  vfs_node_t *node = NULL;
  if (!(flags & VFS_O_CREAT)) {
    node = resolve_path_to_vnode(sb, rel);
    if (!node) {
      serial_printf(COM1_BASE, "vfs_open: File %s not found\n", rel);
      return -1;
    }
  } else {
    char buf[VFS_PATH_MAX];
    size_t rel_len = strnlen(rel, VFS_PATH_MAX);
    if (rel_len == 0 || rel_len >= VFS_PATH_MAX) {
      serial_printf(COM1_BASE, "vfs_open: Invalid relative path length=%u\n",
                    rel_len);
      return -1;
    }
    strncpy(buf, rel, VFS_PATH_MAX - 1);
    buf[VFS_PATH_MAX - 1] = '\0';
    debug_hex_dump("Relative path copy", buf, strnlen(buf, VFS_PATH_MAX));
    char *last_slash = strrchr(buf, '/');
    char parentpath[VFS_PATH_MAX] = {0};
    char *name = buf;
    if (last_slash) {
      *last_slash = '\0';
      name = last_slash + 1;
      strncpy(parentpath, buf, VFS_PATH_MAX - 1);
      parentpath[VFS_PATH_MAX - 1] = '\0';
    } else {
      parentpath[0] = '\0';
    }
    size_t name_len = strnlen(name, VFS_NAME_MAX + 1);
    if (name_len == 0 || name_len > VFS_NAME_MAX) {
      serial_printf(COM1_BASE, "vfs_open: Invalid filename length=%u for %s\n",
                    name_len, name);
      return -1;
    }
    for (size_t i = 0; i < name_len; i++) {
      if (name[i] < 0x20 || name[i] > 0x7E) {
        serial_printf(COM1_BASE,
                      "vfs_open: Invalid character 0x%02X in filename %s at "
                      "position %u\n",
                      (unsigned char)name[i], name, i);
        return -1;
      }
    }
    serial_printf(COM1_BASE, "vfs_open: Parent path: %s, Name: %s\n",
                  parentpath, name);
    debug_hex_dump("Filename", name, name_len);

    // Check if file already exists
    vfs_node_t *existing = resolve_path_to_vnode(sb, rel);
    if (existing) {
      terminal_printf(&main_terminal, "vfs_open: File %s already exists\n",
                      name); // Use 'name' instead of 'rel' for clarity
      existing->refcount--;
      if (existing->refcount == 0 && existing->ops && existing->ops->release) {
        existing->ops->release(existing);
      }
      return -1;
    }

    vfs_node_t *parent = resolve_path_to_vnode(sb, parentpath);
    if (!parent) {
      serial_printf(COM1_BASE, "vfs_open: Parent directory %s not found\n",
                    parentpath);
      return -1;
    }
    if (!parent->ops || !parent->ops->create) {
      serial_printf(COM1_BASE, "vfs_open: Parent %s has no create operation\n",
                    parentpath);
      parent->refcount--;
      if (parent->refcount == 0 && parent->ops && parent->ops->release) {
        parent->ops->release(parent);
      }
      return -1;
    }
    vfs_node_t *created = NULL;
    int r = parent->ops->create(parent, name, &created);
    parent->refcount--;
    if (parent->refcount == 0 && parent->ops && parent->ops->release) {
      parent->ops->release(parent);
    }
    if (r != VFS_OK || !created) {
      serial_printf(COM1_BASE, "vfs_open: Failed to create %s\n", name);
      return -1;
    }
    node = created;
  }

  vfs_file_t *f = (vfs_file_t *)vfs_alloc(sizeof(vfs_file_t));
  if (!f) {
    serial_printf(COM1_BASE, "vfs_open: Failed to allocate vfs_file_t\n");
    node->refcount--;
    if (node->refcount == 0 && node->ops && node->ops->release) {
      node->ops->release(node);
    }
    return -1;
  }
  memset(f, 0, sizeof(vfs_file_t));
  f->node = node;
  f->flags = flags;
  f->offset = 0;
  f->refcount = 1;
  static file_ops_t default_file_ops = {
      .read = NULL, .write = NULL, .close = NULL};
  f->ops = &default_file_ops;

  int fd = allocate_fd(f);
  if (fd < 0) {
    serial_printf(COM1_BASE, "vfs_open: Failed to allocate file descriptor\n");
    node->refcount--;
    if (node->refcount == 0 && node->ops && node->ops->release) {
      node->ops->release(node);
    }
    vfs_free(f);
    return -1;
  }
  serial_printf(COM1_BASE, "vfs_open: Successfully opened %s, fd=%d\n",
                normalized, fd);
  return fd;
}

void debug_hex_dump(const char *label, const char *str, size_t len) {
  serial_printf(COM1_BASE, "DEBUG: %s: ", label);
  for (size_t i = 0; i < len && i < 32; i++) {
    serial_printf(COM1_BASE, "%02X ", (unsigned char)str[i]);
  }
  serial_printf(COM1_BASE, "\n");
}

/* vfs_read / vfs_write dispatch */
int vfs_read(int fd, void *buf, uint32_t size) {
  if (fd < 0 || fd >= VFS_MAX_FDS)
    return -1;
  vfs_file_t *f = fd_table[fd];
  if (!f || !buf)
    return -1;
  if (!f->node || !f->node->ops || !f->node->ops->read)
    return -1;
  
  // ✅ DEBUG
  serial_printf(COM1_BASE, "vfs_read: fd=%d, size=%u, calling node->ops->read\r\n", 
                fd, size);
  
  int got = f->node->ops->read(f->node, (uint8_t *)buf, size, f->offset);
  
  // ✅ DEBUG
  serial_printf(COM1_BASE, "vfs_read: node->ops->read returned %d\r\n", got);
  
  if (got > 0)
    f->offset += got;
  return got;
}

int vfs_write(int fd, const void *buf, uint32_t size) {
  if (fd < 0 || fd >= VFS_MAX_FDS)
    return -1;
  vfs_file_t *f = fd_table[fd];
  if (!f || !buf)
    return -1;
  if (!f->node || !f->node->ops || !f->node->ops->write)
    return -1;
  int wrote =
      f->node->ops->write(f->node, (const uint8_t *)buf, size, f->offset);
  if (wrote > 0)
    f->offset += wrote;
  return wrote;
}

/* close */
int vfs_close(int fd) {
  if (fd < 0 || fd >= VFS_MAX_FDS)
    return VFS_ERR;
  vfs_file_t *f = fd_table[fd];
  if (!f)
    return VFS_ERR;
  /* decrease vnode refcount and call release if zero */
  if (f->node) {
    if (f->node->refcount > 0)
      f->node->refcount--;
    if (f->node->refcount == 0 && f->node->ops && f->node->ops->release) {
      f->node->ops->release(f->node);
    }
  }
  free_fd(fd);
  vfs_free(f);
  return VFS_OK;
}

/* unlink */
int vfs_unlink(const char *path) {
  if (!path)
    return VFS_ERR;

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    terminal_printf(&main_terminal, "VFS: Failed to normalize path %s\r\n",
                    path);
    return VFS_ERR;
  }

  char parent_path[VFS_PATH_MAX];
  char name[VFS_NAME_MAX];
  if (vfs_split_path(normalized, parent_path, name) != VFS_OK) {
    return VFS_ERR;
  }

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(parent_path, &rel);
  if (!sb)
    return VFS_ERR;

  vfs_node_t *parent = resolve_path_to_vnode(sb, rel);
  if (!parent)
    return VFS_ERR;

  if (!parent->ops || !parent->ops->unlink) {
    parent->refcount--;
    if (parent->refcount == 0 && parent->ops->release) {
      parent->ops->release(parent);
    }
    return VFS_ERR;
  }

  int ret = parent->ops->unlink(parent, name);

  parent->refcount--;
  if (parent->refcount == 0 && parent->ops->release) {
    parent->ops->release(parent);
  }

  return ret;
}

/* vfs_mkdir implementation */
int vfs_mkdir(const char *path, vfs_node_t **out) {
  if (!path || !out)
    return VFS_ERR;

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    return VFS_ERR;
  }

  // Si el directorio ya existe, retornar éxito
  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(normalized, &rel);
  if (!sb) {
    return VFS_ERR;
  }

  vfs_node_t *existing = resolve_path_to_vnode(sb, rel);
  if (existing) {
    *out = existing;
    return VFS_OK; // Ya existe, éxito
  }

  // Crear directorios padres si no existen
  char parent_path[VFS_PATH_MAX];
  char name[VFS_NAME_MAX];
  if (vfs_split_path(normalized, parent_path, name) != VFS_OK) {
    return VFS_ERR;
  }

  // Asegurar que el directorio padre existe
  if (strcmp(parent_path, "/") != 0) {
    const char *parent_rel;
    vfs_superblock_t *parent_sb = find_mount_for_path(parent_path, &parent_rel);
    if (parent_sb) {
      vfs_node_t *parent = resolve_path_to_vnode(parent_sb, parent_rel);
      if (!parent) {
        // Crear directorio padre recursivamente
        vfs_node_t *parent_dir = NULL;
        if (vfs_mkdir(parent_path, &parent_dir) != VFS_OK) {
          return VFS_ERR;
        }
        if (parent_dir) {
          parent_dir->refcount--;
          if (parent_dir->refcount == 0 && parent_dir->ops->release) {
            parent_dir->ops->release(parent_dir);
          }
        }
      } else {
        parent->refcount--;
        if (parent->refcount == 0 && parent->ops->release) {
          parent->ops->release(parent);
        }
      }
    }
  }

  // Ahora crear el directorio final
  const char *final_rel;
  vfs_superblock_t *final_sb = find_mount_for_path(parent_path, &final_rel);
  if (!final_sb) {
    return VFS_ERR;
  }

  vfs_node_t *parent = resolve_path_to_vnode(final_sb, final_rel);
  if (!parent) {
    return VFS_ERR;
  }

  if (!parent->ops || !parent->ops->mkdir) {
    parent->refcount--;
    if (parent->refcount == 0 && parent->ops->release) {
      parent->ops->release(parent);
    }
    return VFS_ERR;
  }

  int ret = parent->ops->mkdir(parent, name, out);

  parent->refcount--;
  if (parent->refcount == 0 && parent->ops->release) {
    parent->ops->release(parent);
  }

  return ret;
}

/* vfs_unmount implementation */
int vfs_unmount(const char *mountpoint) {
  if (!mountpoint) {
    terminal_printf(&main_terminal,
                    "VFS: unmount failed: invalid mountpoint\r\n");
    return VFS_ERR;
  }

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(mountpoint, normalized, VFS_PATH_MAX) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "VFS: unmount failed: cannot normalize mountpoint %s\r\n",
                    mountpoint);
    return VFS_ERR;
  }

  // Buscar el mountpoint en la lista
  unsigned int f = vfs_lock_disable_irq();
  vfs_mount_info_t *prev = NULL;
  vfs_mount_info_t *current = mount_list;
  vfs_mount_info_t *found_info = NULL;

  while (current) {
    if (strcmp(current->mountpoint, normalized) == 0) {
      found_info = current;
      break;
    }
    prev = current;
    current = current->next;
  }
  vfs_unlock_restore_irq(f);

  if (!found_info) {
    terminal_printf(&main_terminal,
                    "VFS: unmount failed: mountpoint %s not found\r\n",
                    normalized);
    return VFS_ERR;
  }

  vfs_superblock_t *sb = found_info->sb;
  if (!sb) {
    terminal_printf(&main_terminal,
                    "VFS: unmount failed: invalid superblock for %s\r\n",
                    normalized);
    return VFS_ERR;
  }

  // ✅ Verificar FDs abiertos PARA ESTE SUPERBLOCK
  int open_fds = 0;
  for (int i = 0; i < VFS_MAX_FDS; i++) {
    if (fd_table[i] && fd_table[i]->node && fd_table[i]->node->sb == sb) {
      open_fds++;
      terminal_printf(&main_terminal, "VFS: FD %d still open for %s\r\n", i,
                      normalized);
    }
  }

  if (open_fds > 0) {
    terminal_printf(
        &main_terminal,
        "VFS: unmount failed: %d open file descriptors exist for %s\r\n",
        open_fds, normalized);
    return VFS_ERR;
  }

  // **Decrementar refcount**
  terminal_printf(&main_terminal,
                  "VFS: Unmounting %s (fs: %s, refcount: %u -> %u)\r\n",
                  normalized, sb->fs_name, sb->refcount, sb->refcount - 1);

  sb->refcount--;

  // **SOLO LIBERAR SI ES LA ÚLTIMA REFERENCIA**
  if (sb->refcount == 0) {
    terminal_printf(&main_terminal,
                    "VFS: Last reference to superblock, freeing...\r\n");

    // ✅ Llamar a FS-specific unmount
    vfs_fs_type_t *fst = find_fs(sb->fs_name);
    if (fst && fst->unmount) {
      int ret = fst->unmount(sb);
      if (ret != VFS_OK) {
        boot_log_info("VFS: FS-specific unmount failed for %s (error %d)\r\n",
                      normalized, ret);
        boot_log_error();
        // Restaurar refcount ya que falló
        sb->refcount++;
        return ret;
      }
    } else {
      // **MANEJO ESPECIAL PARA TMPFS Y OTROS SIN UNMOUNT**
      terminal_printf(
          &main_terminal,
          "VFS: No FS-specific unmount for %s, using generic cleanup\r\n",
          sb->fs_name);

      // ✅ Liberación genérica
      if (sb->root && sb->root->ops && sb->root->ops->release) {
        sb->root->ops->release(sb->root);
      }
      if (sb->priv) {
        kernel_free(sb->priv);
      }
      kernel_free(sb);
    }
  } else {
    terminal_printf(
        &main_terminal,
        "VFS: Superblock still has %u references, keeping alive\r\n",
        sb->refcount);
  }

  // ✅ Limpiar tabla de mounts
  f = vfs_lock_disable_irq();
  if (prev) {
    prev->next = found_info->next;
  } else {
    mount_list = found_info->next;
  }
  mount_count--;
  vfs_unlock_restore_irq(f);

  // Liberar mount_info
  vfs_free(found_info);

  terminal_printf(&main_terminal, "VFS: Successfully unmounted %s\r\n",
                  normalized);
  return VFS_OK;
}

int vfs_mknod(const char *path, vfs_dev_type_t dev_type, uint32_t major,
              uint32_t minor) {
  if (!path)
    return VFS_ERR;

  char parent_path[VFS_PATH_MAX];
  char name[VFS_NAME_MAX];
  if (vfs_split_path(path, parent_path, name) != VFS_OK) {
    return VFS_ERR;
  }

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(parent_path, &rel);
  if (!sb)
    return VFS_ERR;

  vfs_node_t *parent = resolve_path_to_vnode(sb, rel);
  if (!parent)
    return VFS_ERR;

  if (!parent->ops || !parent->ops->create) {
    parent->refcount--;
    if (parent->refcount == 0 && parent->ops->release) {
      parent->ops->release(parent);
    }
    return VFS_ERR;
  }

  vfs_node_t *node = NULL;
  int ret = parent->ops->create(parent, name, &node);

  parent->refcount--;
  if (parent->refcount == 0 && parent->ops->release) {
    parent->ops->release(parent);
  }

  if (ret != VFS_OK || !node) {
    return VFS_ERR;
  }

  // Marcar como dispositivo
  node->type = VFS_NODE_FILE; // Pero con información de dispositivo
  // Guardar info de dispositivo en fs_private
  uint32_t *dev_info = (uint32_t *)kernel_malloc(3 * sizeof(uint32_t));
  if (dev_info) {
    dev_info[0] = (uint32_t)dev_type;
    dev_info[1] = major;
    dev_info[2] = minor;
    node->fs_private = dev_info;
  }

  node->refcount--;
  if (node->refcount == 0 && node->ops->release) {
    node->ops->release(node);
  }

  return VFS_OK;
}

/* ================ NUEVAS FUNCIONES PARA BIND MOUNTS ================ */

/* Buscar mount info por punto de montaje (helper) */
static vfs_mount_info_t *find_mount_info(const char *mountpoint) {
  vfs_mount_info_t *current = mount_list;
  while (current) {
    if (strcmp(current->mountpoint, mountpoint) == 0) {
      return current;
    }
    current = current->next;
  }
  return NULL;
}
/* Funciones wrapper estáticas para bind mounts */
static int bind_readdir_wrapper(vfs_node_t *node, vfs_dirent_t *buf,
                                uint32_t *count, uint32_t offset) {
  vfs_node_t *original = (vfs_node_t *)node->fs_private;
  if (!original || !original->ops || !original->ops->readdir) {
    return VFS_ERR;
  }
  return original->ops->readdir(original, buf, count, offset);
}

static int bind_lookup_wrapper(vfs_node_t *parent, const char *name,
                               vfs_node_t **out) {
  vfs_node_t *original = (vfs_node_t *)parent->fs_private;
  if (!original || !original->ops || !original->ops->lookup) {
    return VFS_ERR;
  }
  return original->ops->lookup(original, name, out);
}

static void bind_release_wrapper(vfs_node_t *node) {
  vfs_node_t *original = (vfs_node_t *)node->fs_private;
  if (original) {
    original->refcount--;
    if (original->refcount == 0 && original->ops && original->ops->release) {
      original->ops->release(original);
    }
  }
  // Liberar el nodo bind
  kernel_free(node);
}

static int bind_create_wrapper(vfs_node_t *parent, const char *name,
                               vfs_node_t **out) {
  vfs_node_t *original = (vfs_node_t *)parent->fs_private;
  if (!original || !original->ops || !original->ops->create) {
    return VFS_ERR;
  }
  return original->ops->create(original, name, out);
}

static int bind_mkdir_wrapper(vfs_node_t *parent, const char *name,
                              vfs_node_t **out) {
  vfs_node_t *original = (vfs_node_t *)parent->fs_private;
  if (!original || !original->ops || !original->ops->mkdir) {
    return VFS_ERR;
  }
  return original->ops->mkdir(original, name, out);
}

static int bind_unlink_wrapper(vfs_node_t *parent, const char *name) {
  vfs_node_t *original = (vfs_node_t *)parent->fs_private;
  if (!original || !original->ops || !original->ops->unlink) {
    return VFS_ERR;
  }
  return original->ops->unlink(original, name);
}

/* Crear bind mount - VERSIÓN CORREGIDA PARA C */
int vfs_bind_mount(const char *source, const char *target, int recursive) {
  if (!source || !target) {
    terminal_printf(&main_terminal, "VFS_BIND_MOUNT: Invalid parameters\r\n");
    return VFS_ERR;
  }

  char norm_source[VFS_PATH_MAX];
  char norm_target[VFS_PATH_MAX];

  if (vfs_normalize_path(source, norm_source, VFS_PATH_MAX) != VFS_OK ||
      vfs_normalize_path(target, norm_target, VFS_PATH_MAX) != VFS_OK) {
    terminal_printf(&main_terminal,
                    "VFS_BIND_MOUNT: Failed to normalize paths\r\n");
    return VFS_ERR;
  }

  terminal_printf(&main_terminal, "VFS_BIND_MOUNT: Attempting %s -> %s\r\n",
                  norm_source, norm_target);

  // Buscar el filesystem source
  const char *source_rel;
  vfs_superblock_t *source_sb = find_mount_for_path(norm_source, &source_rel);
  if (!source_sb) {
    terminal_printf(&main_terminal, "VFS_BIND_MOUNT: Source not found: %s\r\n",
                    norm_source);
    return VFS_ERR;
  }

  // Resolver el nodo source - DEBE SER UN DIRECTORIO
  vfs_node_t *source_node = resolve_path_to_vnode(source_sb, source_rel);
  if (!source_node) {
    terminal_printf(&main_terminal,
                    "VFS_BIND_MOUNT: Source node not found: %s\r\n",
                    source_rel);
    return VFS_ERR;
  }

  // **VERIFICACIÓN CRÍTICA: Debe ser un directorio**
  if (source_node->type != VFS_NODE_DIR) {
    terminal_printf(&main_terminal,
                    "VFS_BIND_MOUNT: Source is not a directory (type: %d)\r\n",
                    source_node->type);
    source_node->refcount--;
    if (source_node->refcount == 0 && source_node->ops &&
        source_node->ops->release) {
      source_node->ops->release(source_node);
    }
    return VFS_ERR;
  }

  // **CREAR NODO PARA BIND MOUNT**
  vfs_node_t *bind_node = (vfs_node_t *)vfs_alloc(sizeof(vfs_node_t));
  if (!bind_node) {
    source_node->refcount--;
    if (source_node->refcount == 0 && source_node->ops &&
        source_node->ops->release) {
      source_node->ops->release(source_node);
    }
    return VFS_ERR;
  }

  memset(bind_node, 0, sizeof(vfs_node_t));

  // **CONFIGURAR COMO DIRECTORIO**
  strncpy(bind_node->name, source_node->name, VFS_NAME_MAX - 1);
  bind_node->type = VFS_NODE_DIR;
  bind_node->refcount = 1;

  // **GUARDAR EL NODO ORIGINAL**
  bind_node->fs_private = source_node;

  // **CREAR OPERACIONES WRAPPER ESTÁTICAS**
  static vnode_ops_t bind_ops = {
      .lookup = bind_lookup_wrapper,
      .create = bind_create_wrapper,
      .mkdir = bind_mkdir_wrapper,
      .read = NULL,  // No soportado para directorios en bind mount
      .write = NULL, // No soportado para directorios en bind mount
      .readdir = bind_readdir_wrapper,
      .release = bind_release_wrapper,
      .unlink = bind_unlink_wrapper,
      .symlink = NULL, // Puedes agregar wrapper si es necesario
      .readlink = NULL,
      .truncate = NULL,
      .getattr = NULL};

  // **COPIAR OTRAS OPERACIONES DEL SOURCE SI EXISTEN**
  if (source_node->ops) {
    // Ya configuramos las principales, copiamos las opcionales
    if (source_node->ops->symlink) {
      // Necesitarías crear un wrapper para symlink también
    }
    if (source_node->ops->readlink) {
      // Necesitarías crear un wrapper para readlink también
    }
    if (source_node->ops->truncate) {
      bind_ops.truncate = source_node->ops->truncate;
    }
    if (source_node->ops->getattr) {
      bind_ops.getattr = source_node->ops->getattr;
    }
  }

  bind_node->ops = &bind_ops;

  // Crear superblock para bind mount
  vfs_superblock_t *bind_sb =
      (vfs_superblock_t *)vfs_alloc(sizeof(vfs_superblock_t));
  if (!bind_sb) {
    kernel_free(bind_node);
    source_node->refcount--;
    if (source_node->refcount == 0 && source_node->ops &&
        source_node->ops->release) {
      source_node->ops->release(source_node);
    }
    return VFS_ERR;
  }

  memset(bind_sb, 0, sizeof(vfs_superblock_t));
  strncpy(bind_sb->fs_name, "bind", sizeof(bind_sb->fs_name) - 1);
  bind_sb->root = bind_node;
  bind_sb->flags = VFS_MOUNT_BIND | (recursive ? VFS_MOUNT_RECURSIVE : 0);
  bind_sb->bind_source = source_sb;
  strncpy(bind_sb->bind_path, source_rel, VFS_PATH_MAX - 1);
  bind_sb->backing_device = source_sb->backing_device; // Mismo dispositivo

  // **ESTABLECER SUPERBLOCK**
  bind_node->sb = bind_sb;

  // Crear mount info
  vfs_mount_info_t *mount_info =
      (vfs_mount_info_t *)vfs_alloc(sizeof(vfs_mount_info_t));
  if (!mount_info) {
    kernel_free(bind_sb);
    kernel_free(bind_node);
    source_node->refcount--;
    if (source_node->refcount == 0 && source_node->ops &&
        source_node->ops->release) {
      source_node->ops->release(source_node);
    }
    return VFS_ERR;
  }

  memset(mount_info, 0, sizeof(vfs_mount_info_t));
  mount_info->sb = bind_sb;
  strncpy(mount_info->mountpoint, norm_target, VFS_PATH_MAX - 1);
  strncpy(mount_info->source, norm_source, VFS_PATH_MAX - 1);
  strncpy(mount_info->fs_type, "bind", sizeof(mount_info->fs_type) - 1);
  mount_info->flags = bind_sb->flags;

  // Agregar a lista de montajes
  unsigned int f = vfs_lock_disable_irq();
  mount_info->next = mount_list;
  mount_list = mount_info;
  mount_count++;
  vfs_unlock_restore_irq(f);

  terminal_printf(&main_terminal, "✓ Bind mount created: %s -> %s\r\n",
                  norm_source, norm_target);

  // **INCREMENTAR REFCOUNT DEL SOURCE NODE**
  source_node->refcount++;

  return VFS_OK;
}

/* Crear symlink */
int vfs_symlink(const char *target, const char *linkpath) {
  if (!target || !linkpath) {
    return VFS_ERR;
  }

  char parent_path[VFS_PATH_MAX];
  char name[VFS_NAME_MAX];

  if (vfs_split_path(linkpath, parent_path, name) != VFS_OK) {
    return VFS_ERR;
  }

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(parent_path, &rel);
  if (!sb) {
    return VFS_ERR;
  }

  vfs_node_t *parent = resolve_path_to_vnode(sb, rel);
  if (!parent) {
    return VFS_ERR;
  }

  if (!parent->ops || !parent->ops->symlink) {
    parent->refcount--;
    if (parent->refcount == 0 && parent->ops->release) {
      parent->ops->release(parent);
    }
    return VFS_ERR;
  }

  int ret = parent->ops->symlink(parent, name, target);

  parent->refcount--;
  if (parent->refcount == 0 && parent->ops->release) {
    parent->ops->release(parent);
  }

  return ret;
}

/* Leer symlink */
int vfs_readlink(const char *path, char *buf, uint32_t size) {
  if (!path || !buf || size == 0) {
    return VFS_ERR;
  }

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(path, &rel);
  if (!sb) {
    return VFS_ERR;
  }

  vfs_node_t *node = resolve_path_to_vnode(sb, rel);
  if (!node) {
    return VFS_ERR;
  }

  if (node->type != VFS_NODE_SYMLINK) {
    node->refcount--;
    if (node->refcount == 0 && node->ops->release) {
      node->ops->release(node);
    }
    return VFS_ERR;
  }

  if (!node->ops || !node->ops->readlink) {
    node->refcount--;
    if (node->refcount == 0 && node->ops->release) {
      node->ops->release(node);
    }
    return VFS_ERR;
  }

  int ret = node->ops->readlink(node, buf, size);

  node->refcount--;
  if (node->refcount == 0 && node->ops->release) {
    node->ops->release(node);
  }

  return ret;
}

/* Resolver path mejorado (para bind mounts) */
vfs_node_t *vfs_resolve_path(const char *path, uint32_t flags,
                             vfs_superblock_t **out_sb,
                             const char **out_relpath) {
  if (!path || !out_sb || !out_relpath) {
    return NULL;
  }

  char normalized[VFS_PATH_MAX];
  if (vfs_normalize_path(path, normalized, VFS_PATH_MAX) != VFS_OK) {
    return NULL;
  }

  // Buscar el mejor mount point
  vfs_mount_info_t *best_mount = NULL;
  int best_len = -1;
  const char *best_relpath = NULL;

  vfs_mount_info_t *current = mount_list;
  while (current) {
    char normalized_mount[VFS_PATH_MAX];
    if (vfs_normalize_path(current->mountpoint, normalized_mount,
                           VFS_PATH_MAX) != VFS_OK) {
      current = current->next;
      continue;
    }

    int mount_len = strlen(normalized_mount);

    // Comparación exacta o prefijo
    if (strcmp(normalized_mount, "/") == 0) {
      // Root mount siempre coincide
      if (best_len == -1) {
        best_mount = current;
        best_len = 0;
        best_relpath = normalized + 1;
        if (*best_relpath == '\0')
          best_relpath = "";
      }
    } else if (strncmp(normalized, normalized_mount, mount_len) == 0 &&
               (normalized[mount_len] == '\0' ||
                normalized[mount_len] == '/')) {
      if (mount_len > best_len) {
        best_mount = current;
        best_len = mount_len;
        best_relpath = normalized + mount_len;
        if (*best_relpath == '/')
          best_relpath++;
        if (*best_relpath == '\0')
          best_relpath = "";
      }
    }

    current = current->next;
  }

  if (!best_mount) {
    return NULL;
  }

  *out_sb = best_mount->sb;
  *out_relpath = best_relpath;

  // Si es un bind mount, resolver dentro del source
  if (best_mount->sb->flags & VFS_MOUNT_BIND) {
    // Construir path completo en el source
    if (best_relpath[0] == '\0') {
      // Apuntar directamente al directorio bindeado
      best_mount->sb->root->refcount++;
      return best_mount->sb->root;
    } else {
      // Resolver path relativo dentro del source
      char full_source_path[VFS_PATH_MAX];
      snprintf(full_source_path, VFS_PATH_MAX, "%s/%s",
               best_mount->sb->bind_path, best_relpath);
      return resolve_path_to_vnode(best_mount->sb->bind_source,
                                   full_source_path);
    }
  }

  // Montaje normal
  return resolve_path_to_vnode(best_mount->sb, best_relpath);
}

/* Funciones stub para compatibilidad */
int vfs_umount(const char *mountpoint, int flags) {
  (void)flags; // No usado por ahora
  return vfs_unmount(mountpoint);
}

int vfs_stat(const char *path, vfs_dirent_t *statbuf) {
  // Implementación simple - solo para compatibilidad
  if (!path || !statbuf)
    return VFS_ERR;

  const char *rel;
  vfs_superblock_t *sb = find_mount_for_path(path, &rel);
  if (!sb)
    return VFS_ERR;

  vfs_node_t *node = resolve_path_to_vnode(sb, rel);
  if (!node)
    return VFS_ERR;

  memset(statbuf, 0, sizeof(vfs_dirent_t));
  statbuf->type = node->type;
  // Nota: El tamaño no se implementa aquí

  node->refcount--;
  if (node->refcount == 0 && node->ops->release) {
    node->ops->release(node);
  }

  return VFS_OK;
}

int vfs_lstat(const char *path, vfs_dirent_t *statbuf) {
  // Por ahora, igual que vfs_stat (no seguimos symlinks en stat)
  return vfs_stat(path, statbuf);
}