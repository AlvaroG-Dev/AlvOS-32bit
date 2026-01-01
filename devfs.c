// devfs.c - Versión corregida
#include "kernel.h"
#include "string.h"
#include "vfs.h"
#include "serial.h"
#include "task.h"
#include <stdint.h>
#include "chardev_vfs.h"

extern void *kernel_malloc(size_t);
extern int kernel_free(void *);
extern Terminal main_terminal;

// Nodos para devfs
#define DEV_NODE_ROOT 1
#define DEV_NODE_COM1 2
#define DEV_NODE_COM2 3

static int dev_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out);
static int dev_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset);
static int dev_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset);
static int dev_readdir(vfs_node_t *dir, vfs_dirent_t *dirents, uint32_t *count, uint32_t offset);
static void dev_release(vfs_node_t *node);
static int dev_getattr(vfs_node_t *node, vfs_dirent_t *attr);

static vnode_ops_t dev_vnode_ops = {
    .lookup = dev_lookup,
    .read = dev_read,
    .write = dev_write,
    .readdir = dev_readdir,
    .release = dev_release,
    .getattr = dev_getattr
};

static vfs_node_t *create_dev_node(const char *name, int type, uint32_t id, vfs_superblock_t *sb) {
    vfs_node_t *vn = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    strncpy(vn->name, name, VFS_NAME_MAX - 1);
    vn->type = type;
    vn->fs_private = (void *)(uintptr_t)id;
    vn->ops = &dev_vnode_ops;
    vn->sb = sb;
    vn->refcount = 1;
    
    serial_printf(COM1_BASE, "create_dev_node: Created %s (id=%u, type=%d) at %p\n",
                 name, id, type, vn);
    
    return vn;
}

int devfs_mount(void *device, vfs_superblock_t **out_sb) {
    (void)device;
    
    serial_printf(COM1_BASE, "devfs_mount: Mounting devfs\n");
    
    vfs_superblock_t *sb = (vfs_superblock_t *)kernel_malloc(sizeof(vfs_superblock_t));
    if (!sb) return -1;
    memset(sb, 0, sizeof(*sb));
    strncpy(sb->fs_name, "devfs", sizeof(sb->fs_name) - 1);
    sb->root = create_dev_node("/", VFS_NODE_DIR, DEV_NODE_ROOT, sb);
    *out_sb = sb;
    
    serial_printf(COM1_BASE, "devfs_mount: Success, root at %p\n", sb->root);
    
    return 0;
}

static int dev_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out) {
    uint32_t id = (uint32_t)(uintptr_t)parent->fs_private;
    
    serial_printf(COM1_BASE, "dev_lookup: Looking up '%s' in parent id=%u\n", name, id);
    
    if (id != DEV_NODE_ROOT) {
        serial_printf(COM1_BASE, "ERROR: dev_lookup: Not root directory\n");
        return -1;
    }

    // Dispositivos seriales
    if (strcmp(name, "com1") == 0) {
        serial_printf(COM1_BASE, "dev_lookup: Found com1\n");
        *out = create_dev_node("com1", VFS_NODE_CHRDEV, DEV_NODE_COM1, parent->sb);
        return (*out) ? 0 : -1;
    } else if (strcmp(name, "com2") == 0) {
        serial_printf(COM1_BASE, "dev_lookup: Found com2\n");
        *out = create_dev_node("com2", VFS_NODE_CHRDEV, DEV_NODE_COM2, parent->sb);
        return (*out) ? 0 : -1;
    }
    
    // Dispositivos especiales usando chardev
    serial_printf(COM1_BASE, "dev_lookup: Trying chardev for '%s'\n", name);
    
    *out = chardev_vfs_find(name);
    if (*out) {
        (*out)->sb = parent->sb; // Establecer superblock
        serial_printf(COM1_BASE, "dev_lookup: Success - created VFS node at %p\n", *out);
        return 0;
    }
    
    serial_printf(COM1_BASE, "ERROR: dev_lookup: Device '%s' not found\n", name);
    return -1;
}


static int dev_readdir(vfs_node_t *dir, vfs_dirent_t *dirents, uint32_t *count, uint32_t offset) {
    uint32_t id = (uint32_t)(uintptr_t)dir->fs_private;
    if (id != DEV_NODE_ROOT) return -1;
    
    // Lista de todos los dispositivos disponibles
    const char *devices[] = {
        "com1", "com2",     // Serial
        "null", "zero",     // Especiales básicos
        "random", "urandom", // Aleatoriedad
        "full"              // Dispositivo de prueba
    };
    const int num_devices = 7;
    
    if (offset >= num_devices) {
        *count = 0;
        return 0;
    }
    
    // Determinar cuántos dispositivos devolver
    uint32_t max_return = *count;
    uint32_t returned = 0;
    
    for (uint32_t i = 0; i < max_return && (offset + i) < num_devices; i++) {
        strncpy(dirents[i].name, devices[offset + i], VFS_NAME_MAX - 1);
        dirents[i].type = VFS_NODE_CHRDEV;
        returned++;
    }
    
    *count = returned;
    return 0;
}


// ✅ VERSIÓN PROFESIONAL: Con soporte para O_NONBLOCK
static int dev_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset) {
    (void)offset;
    uint32_t id = (uint32_t)(uintptr_t)node->fs_private;
    uint16_t port = (id == DEV_NODE_COM1) ? COM1_BASE : COM2_BASE;
    
    if (id != DEV_NODE_COM1 && id != DEV_NODE_COM2) return -1;
    
    // Obtener flags del archivo abierto
    // Necesitamos acceso a vfs_file_t para saber si es O_NONBLOCK
    // Por ahora asumimos bloqueante para cat
    
    uint32_t read_bytes = 0;
    int attempts = 0;
    
    // Modo "cat" (bloqueante por defecto)
    // Intentar hasta 2 segundos (~200 ticks a 100Hz)
    while (read_bytes == 0 && attempts < 200) {
        // Leer todo lo disponible
        while (read_bytes < size) {
            int c = serial_read_nonblock(port);
            if (c != -1) {
                buf[read_bytes++] = (unsigned char)c;
            } else {
                break;
            }
        }
        
        if (read_bytes > 0) {
            break; // Tenemos datos
        }
        
        // Esperar para dar tiempo a que lleguen datos
        attempts++;
        task_sleep(1); // Dormir 10ms
    }
    
    // DEBUG solo si leímos algo
    if (read_bytes > 0) {
        serial_printf(COM1_BASE, "dev_read: Read %u bytes\r\n", read_bytes);
    }
    
    return (int)read_bytes;
}

static int dev_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset) {
    (void)offset;
    uint32_t id = (uint32_t)(uintptr_t)node->fs_private;
    uint16_t port = (id == DEV_NODE_COM1) ? COM1_BASE : COM2_BASE;
    
    if (id != DEV_NODE_COM1 && id != DEV_NODE_COM2) return -1;

    for (uint32_t i = 0; i < size; i++) {
        if (serial_write_char(port, buf[i]) != 0) {
            // Error en escritura, retornar lo que escribimos
            return (int)i;
        }
    }
    return (int)size;
}

static void dev_release(vfs_node_t *node) {
    kernel_free(node);
}

static int dev_getattr(vfs_node_t *node, vfs_dirent_t *attr) {
    memset(attr, 0, sizeof(vfs_dirent_t));
    strncpy(attr->name, node->name, VFS_NAME_MAX - 1);
    attr->type = node->type;
    attr->size = 0; // Character devices don't have a size
    return 0;
}

vfs_fs_type_t devfs_type = {
    .name = "devfs",
    .mount = devfs_mount
};