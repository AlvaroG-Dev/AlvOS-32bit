// vfs.h
#ifndef VFS_H
#define VFS_H

#include "memutils.h"
#include <stddef.h>
#include <stdint.h>

/* Ajustes */
#define VFS_MAX_FDS 64
#define VFS_MAX_FS_TYPES 8
#define VFS_MAX_MOUNTS 8
#define VFS_PATH_MAX 256
#define VFS_NAME_MAX 32
#define SECTOR_SIZE 512

/* Errores */
#define VFS_OK 0
#define VFS_ERR -1

/* Vnode types - EXTENDIDOS */
#define VFS_NODE_DIR 1
#define VFS_NODE_FILE 2
#define VFS_NODE_SYMLINK 3 // Nuevo: enlace simbólico
#define VFS_NODE_CHRDEV 4  // Nuevo: dispositivo carácter
#define VFS_NODE_BLKDEV 5  // Nuevo: dispositivo bloque

/* File flags - EXTENDIDOS */
#define VFS_O_RDONLY 0x1
#define VFS_O_WRONLY 0x2
#define VFS_O_RDWR 0x4
#define VFS_O_CREAT 0x8
#define VFS_O_TRUNC 0x10
#define VFS_O_EXCL 0x20     // Nuevo: fallar si existe
#define VFS_O_NOFOLLOW 0x40 // Nuevo: no seguir symlinks

/* Flags de montaje */
#define VFS_MOUNT_RDONLY 0x1
#define VFS_MOUNT_NOEXEC 0x2
#define VFS_MOUNT_NOSUID 0x4
#define VFS_MOUNT_BIND 0x8       // Bind mount
#define VFS_MOUNT_RECURSIVE 0x10 // Bind mount recursivo

/* Path resolution flags */
#define VFS_RESOLVE_NOFOLLOW 0x1 // No seguir último symlink
#define VFS_RESOLVE_BENEATH 0x2  // No salir del root del filesystem

/* Forward decls */
struct vfs_superblock;
struct vfs_file;
struct vfs_node;
struct vfs_mount_info; // NUEVO: forward declaration
extern int mount_count;

/* Definición completa de vfs_node_t ANTES de vnode_ops_t */
typedef struct vfs_node {
  char name[VFS_NAME_MAX];
  uint8_t type;     /* VFS_NODE_DIR / VFS_NODE_FILE */
  void *fs_private; /* FS-specific pointer */
  struct vnode_ops *ops;
  struct vfs_superblock *sb;
  uint32_t refcount;
} vfs_node_t;

typedef enum { VFS_DEV_BLOCK = 1, VFS_DEV_CHAR = 2 } vfs_dev_type_t;

/* Nota: esto protege en kernels uniprocesador con preemptive IRQs. */
static inline unsigned int vfs_lock_disable_irq(void) {
  unsigned int flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
  return flags;
}
static inline void vfs_unlock_restore_irq(unsigned int flags) {
  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
}

/* Dirent structure - EXTENDIDO */
typedef struct vfs_dirent {
  char name[VFS_NAME_MAX];
  uint8_t type;  /* VFS_NODE_DIR, VFS_NODE_FILE, VFS_NODE_SYMLINK, etc. */
  uint32_t size; // Nuevo: tamaño para symlinks
  char link_target[VFS_PATH_MAX]; // Nuevo: destino para symlinks
} vfs_dirent_t;

/* vnode operations - EXTENDIDOS */
typedef struct vnode_ops {
  int (*lookup)(vfs_node_t *parent, const char *name, vfs_node_t **out);
  int (*create)(vfs_node_t *parent, const char *name, vfs_node_t **out);
  int (*mkdir)(vfs_node_t *parent, const char *name, vfs_node_t **out);
  int (*read)(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset);
  int (*write)(vfs_node_t *node, const uint8_t *buf, uint32_t size,
               uint32_t offset);
  int (*readdir)(vfs_node_t *node, vfs_dirent_t *buf, uint32_t *count,
                 uint32_t offset);
  void (*release)(vfs_node_t *node);
  int (*unlink)(vfs_node_t *parent, const char *name);

  // Nuevas operaciones
  int (*symlink)(vfs_node_t *parent, const char *name, const char *target);
  int (*readlink)(vfs_node_t *node, char *buf, uint32_t size);
  int (*truncate)(vfs_node_t *node, uint32_t size);
  int (*getattr)(vfs_node_t *node, vfs_dirent_t *attr);
} vnode_ops_t;

/* Resto del archivo sin cambios */
typedef struct file_ops {
  int (*read)(struct vfs_file *f, uint8_t *buf, uint32_t size);
  int (*write)(struct vfs_file *f, const uint8_t *buf, uint32_t size);
  int (*close)(struct vfs_file *f);
} file_ops_t;

/* Superblock - EXTENDIDO con flags de montaje */
typedef struct vfs_superblock {
  char fs_name[16];
  void *priv; /* FS private data */
  vfs_node_t *root;
  void *backing_device;               /* e.g. disk_t* or NULL */
  uint32_t flags;                     /* Flags de montaje */
  struct vfs_superblock *bind_source; /* CORRECCIÓN: usa struct aquí */
  char bind_path[VFS_PATH_MAX];       /* Path dentro del source */
  uint32_t refcount;                  /* Contador de referencias */
} vfs_superblock_t;

/* open file descriptor (internal) */
typedef struct vfs_file {
  vfs_node_t *node;
  uint32_t flags;
  uint32_t offset;
  file_ops_t *ops;
  uint32_t refcount;
} vfs_file_t;

/* FS type registration */
typedef int (*fs_mount_fn)(void *device, vfs_superblock_t **out_sb);
typedef int (*fs_unmount_fn)(
    vfs_superblock_t *sb); // Nueva: callback para unmount

typedef struct vfs_fs_type {
  char name[16];
  fs_mount_fn mount;
  fs_unmount_fn unmount; // Nueva
} vfs_fs_type_t;

/* Mount info para tabla de montajes */
typedef struct vfs_mount_info {
  vfs_superblock_t *sb;
  char mountpoint[VFS_PATH_MAX];
  char source[VFS_PATH_MAX]; // Dispositivo fuente o "none"
  char fs_type[16];
  uint32_t flags;
  struct vfs_mount_info *next; // Para lista enlazada
} vfs_mount_info_t;

extern vfs_mount_info_t *mount_list;

/* Public API (POSIX-like fd interface) */
void vfs_init(void);
int vfs_register_fs(const vfs_fs_type_t *fs);
int vfs_mount(const char *mountpoint, const char *fsname, void *device);
int vfs_open(const char *path, uint32_t flags); /* returns fd or -1 */
int vfs_read(int fd, void *buf, uint32_t size);
int vfs_write(int fd, const void *buf, uint32_t size);
int vfs_close(int fd);
int vfs_unlink(const char *path); // Nueva función pública
int vfs_unlink_recursive(const char *path, int recursive);
int vfs_mknod(const char *path, vfs_dev_type_t dev_type, uint32_t major,
              uint32_t minor);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, uint32_t size);
int vfs_bind_mount(const char *source, const char *target, int recursive);
int vfs_umount(const char *mountpoint, int flags);
int vfs_stat(const char *path, vfs_dirent_t *statbuf);
int vfs_lstat(const char *path, vfs_dirent_t *statbuf); // No sigue symlinks
vfs_node_t *vfs_resolve_path(const char *path, uint32_t flags,
                             vfs_superblock_t **out_sb,
                             const char **out_relpath);

/* Path resolution functions */
vfs_node_t *resolve_path_to_vnode(vfs_superblock_t *sb, const char *relpath);
vfs_superblock_t *find_mount_for_path(const char *path,
                                      const char **out_relpath);

/* Path utility function */
int vfs_split_path(const char *path, char *parent_path, char *name);
int vfs_normalize_path(const char *input, char *output, size_t output_size);

/* List all active mounts */
int vfs_list_mounts(void (*callback)(const char *mountpoint,
                                     const char *fs_name, void *arg),
                    void *arg);
int close_fds_for_mount(vfs_superblock_t *sb);

// Fix: Agregar declaraciones públicas para mkdir y unmount
int vfs_mkdir(const char *path, vfs_node_t **out);
int vfs_unmount(const char *mountpoint);
void debug_hex_dump(const char *label, const char *str, size_t len);

#endif /* VFS_H */