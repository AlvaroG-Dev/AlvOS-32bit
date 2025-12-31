#include "kernel.h"
#include "string.h"
#include "vfs.h"
#include <stdint.h>

extern void *kernel_malloc(size_t);
extern int kernel_free(void *);
extern uint32_t ticks_since_boot;

// Nodos virtuales para sysfs
#define SYS_NODE_ROOT 1
#define SYS_NODE_INFO 2
#define SYS_NODE_MEM 3

static int sys_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out);
static int sys_read(vfs_node_t *node, uint8_t *buf, uint32_t size,
                    uint32_t offset);
static int sys_readdir(vfs_node_t *dir, vfs_dirent_t *dirents, uint32_t *count,
                       uint32_t offset);
static void sys_release(vfs_node_t *node);

static vnode_ops_t sys_vnode_ops = {.lookup = sys_lookup,
                                    .read = sys_read,
                                    .readdir = sys_readdir,
                                    .release = sys_release};

static vfs_node_t *create_sys_node(const char *name, int type, uint32_t id,
                                   vfs_superblock_t *sb) {
  vfs_node_t *vn = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
  if (!vn)
    return NULL;
  memset(vn, 0, sizeof(*vn));
  strncpy(vn->name, name, VFS_NAME_MAX - 1);
  vn->type = type;
  vn->fs_private = (void *)(uintptr_t)id; // Usamos el ID como private data
  vn->ops = &sys_vnode_ops;
  vn->sb = sb;
  vn->refcount = 1;
  return vn;
}

int sysfs_mount(void *device, vfs_superblock_t **out_sb) {
  (void)device;
  vfs_superblock_t *sb =
      (vfs_superblock_t *)kernel_malloc(sizeof(vfs_superblock_t));
  if (!sb)
    return -1;
  memset(sb, 0, sizeof(*sb));
  strncpy(sb->fs_name, "sysfs", sizeof(sb->fs_name) - 1);

  sb->root = create_sys_node("/", VFS_NODE_DIR, SYS_NODE_ROOT, sb);
  *out_sb = sb;
  return 0;
}

static int sys_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out) {
  uint32_t id = (uint32_t)(uintptr_t)parent->fs_private;
  if (id != SYS_NODE_ROOT)
    return -1;

  if (strcmp(name, "info") == 0) {
    *out = create_sys_node("info", VFS_NODE_FILE, SYS_NODE_INFO, parent->sb);
    return 0;
  } else if (strcmp(name, "uptime") == 0) {
    *out = create_sys_node("uptime", VFS_NODE_FILE, SYS_NODE_MEM, parent->sb);
    return 0;
  }
  return -1;
}

static int sys_readdir(vfs_node_t *dir, vfs_dirent_t *dirents, uint32_t *count,
                       uint32_t offset) {
  uint32_t id = (uint32_t)(uintptr_t)dir->fs_private;
  if (id != SYS_NODE_ROOT)
    return -1;
  if (offset != 0) {
    *count = 0;
    return 0;
  }

  int n = 0;
  strncpy(dirents[n].name, "info", VFS_NAME_MAX - 1);
  dirents[n].type = VFS_NODE_FILE;
  n++;

  strncpy(dirents[n].name, "uptime", VFS_NAME_MAX - 1);
  dirents[n].type = VFS_NODE_FILE;
  n++;

  *count = n;
  return 0;
}

static int sys_read(vfs_node_t *node, uint8_t *buf, uint32_t size,
                    uint32_t offset) {
  uint32_t id = (uint32_t)(uintptr_t)node->fs_private;
  char data[256];
  memset(data, 0, sizeof(data));

  if (id == SYS_NODE_INFO) {
    snprintf(data, sizeof(data),
             "OS: MicroKernelOS\nVersion: 0.1.0\nAuthor: Alvaro\n");
  } else if (id == SYS_NODE_MEM) {
    snprintf(data, sizeof(data), "%u\n", ticks_since_boot);
  } else {
    return -1;
  }

  size_t len = strlen(data);
  if (offset >= len)
    return 0;

  size_t to_copy = len - offset;
  if (to_copy > size)
    to_copy = size;

  memcpy(buf, data + offset, to_copy);
  return (int)to_copy;
}

static void sys_release(vfs_node_t *node) { kernel_free(node); }

vfs_fs_type_t sysfs_type = {.name = "sysfs", .mount = sysfs_mount};
