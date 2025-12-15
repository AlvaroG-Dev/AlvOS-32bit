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

vfs_superblock_t *mount_table[VFS_MAX_MOUNTS];
char mount_points[VFS_MAX_MOUNTS][VFS_PATH_MAX];
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
  for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    mount_table[i] = NULL;
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

  for (int i = 0; i < mount_count; i++) {
    if (!mount_table[i] || !mount_points[i][0])
      continue;

    char normalized_mount[VFS_PATH_MAX];
    if (vfs_normalize_path(mount_points[i], normalized_mount, VFS_PATH_MAX) !=
        VFS_OK)
      continue;

    int mount_len = strlen(normalized_mount);

    if (strcmp(normalized_mount, "/") == 0) {
      if (best_len == -1) {
        best_sb = mount_table[i];
        best_len = 0;
        best_relpath = normalized + 1;
        if (*best_relpath == '\0')
          best_relpath = "";
        serial_printf(COM1_BASE,
                      "VFS: Matched root mount for path %s (fs: %s)\r\n",
                      normalized, best_sb->fs_name);
      }
    } else {
      if (strncmp(normalized, normalized_mount, mount_len) == 0 &&
          (normalized[mount_len] == '\0' || normalized[mount_len] == '/')) {
        if (mount_len > best_len) {
          best_sb = mount_table[i];
          best_len = mount_len;
          best_relpath = normalized + mount_len;
          if (*best_relpath == '/')
            best_relpath++;
          if (*best_relpath == '\0')
            best_relpath = "";
          serial_printf(COM1_BASE,
                        "VFS: Matched mount %s for path %s (fs: %s)\r\n",
                        normalized_mount, normalized, best_sb->fs_name);
        }
      }
    }
  }

  if (!best_sb) {
    terminal_printf(&main_terminal, "VFS: No mount found for path %s\r\n",
                    normalized);
  }

  *out_relpath = best_relpath;
  vfs_unlock_restore_irq(f);
  return best_sb;
}

/* Mount with proper error handling - FIXED VERSION */
int vfs_mount(const char *mountpoint, const char *fsname, void *device) {
  if (!mountpoint || !fsname)
    return VFS_ERR;

  unsigned int f = vfs_lock_disable_irq();
  if (mount_count >= VFS_MAX_MOUNTS) {
    vfs_unlock_restore_irq(f);
    return VFS_ERR;
  }

  vfs_fs_type_t *fst = find_fs(fsname);
  if (!fst) {
    vfs_unlock_restore_irq(f);
    return VFS_ERR;
  }

  // Check if mount point is already mounted
  for (int i = 0; i < mount_count; i++) {
    if (strcmp(mount_points[i], mountpoint) == 0) {
      vfs_unlock_restore_irq(f);
      return VFS_ERR; // Already mounted
    }
  }

  // Release IRQ lock before doing filesystem operations
  vfs_unlock_restore_irq(f);

  // Create mount point directory if it's not root and doesn't exist
  if (strcmp(mountpoint, "/") != 0) {
    const char *relpath;
    vfs_superblock_t *parent_sb = find_mount_for_path(mountpoint, &relpath);
    if (!parent_sb) {
      return VFS_ERR;
    }

    vfs_node_t *mount_dir = resolve_path_to_vnode(parent_sb, relpath);
    if (!mount_dir) {
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

  // Now proceed with mounting
  vfs_superblock_t *sb = NULL;
  if (fst->mount(device, &sb) != VFS_OK || !sb) {
    return VFS_ERR;
  }

  // Re-acquire lock for mount table modification
  f = vfs_lock_disable_irq();
  if (mount_count >= VFS_MAX_MOUNTS) {
    vfs_unlock_restore_irq(f);
    // TODO: Should cleanup sb here
    return VFS_ERR;
  }

  // Add to mount table
  mount_table[mount_count] = sb;
  strncpy(mount_points[mount_count], mountpoint, VFS_PATH_MAX - 1);
  mount_points[mount_count][VFS_PATH_MAX - 1] = '\0';
  mount_count++;

  vfs_unlock_restore_irq(f);
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
  for (int i = 0; i < mount_count; i++) {
    if (mount_table[i] && mount_points[i][0]) {
      callback(mount_points[i], mount_table[i]->fs_name, arg);
      count++;
    }
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
  int got = f->node->ops->read(f->node, (uint8_t *)buf, size, f->offset);
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

  // Buscar el mountpoint
  unsigned int f = vfs_lock_disable_irq();
  int found = -1;
  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (mount_table[i] && strcmp(mount_points[i], normalized) == 0) {
      found = i;
      break;
    }
  }
  vfs_unlock_restore_irq(f);

  if (found == -1) {
    terminal_printf(&main_terminal,
                    "VFS: unmount failed: mountpoint %s not found\r\n",
                    normalized);
    return VFS_ERR;
  }

  vfs_superblock_t *sb = mount_table[found];
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

  // ✅ Llamar a FS-specific unmount - EL FS DEBE ENCARGARSE DE LIBERAR TODO
  vfs_fs_type_t *fst = find_fs(sb->fs_name);
  if (fst && fst->unmount) {
    int ret = fst->unmount(sb);
    if (ret != VFS_OK) {
      boot_log_info("VFS: FS-specific unmount failed for %s (error %d)\r\n",
                    normalized, ret);
      boot_log_error();
      return ret;
    }
  } else {
    boot_log_warn("VFS: Warning: No FS-specific unmount for %s (fs: %s)\r\n",
                  normalized, sb->fs_name);

    // ✅ Liberación mínima si no hay unmount específico
    if (sb->root && sb->root->ops && sb->root->ops->release) {
      sb->root->ops->release(sb->root);
    }
    if (sb->private) {
      kernel_free(sb->private);
    }
    kernel_free(sb);
  }

  // ✅ Limpiar tabla de mounts (esto debe hacerse SIEMPRE)
  f = vfs_lock_disable_irq();
  mount_table[found] = NULL;
  mount_points[found][0] = '\0';
  mount_count--;
  vfs_unlock_restore_irq(f);

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