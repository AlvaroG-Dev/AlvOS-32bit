// chardev_vfs.c - Versión mejorada
#include "chardev_vfs.h"
#include "kernel.h"
#include "memutils.h"
#include "string.h"
#include "serial.h"
#include "terminal.h"

extern Terminal main_terminal;

// ==================== ADAPTADOR VFS ====================
static vnode_ops_t chardev_vnode_ops = {
    .lookup = NULL,
    .create = NULL,
    .mkdir = NULL,
    .read = chardev_vfs_read,
    .write = chardev_vfs_write,
    .readdir = NULL,
    .release = chardev_vfs_release,
    .unlink = NULL,
    .symlink = NULL,
    .readlink = NULL,
    .truncate = NULL,
    .getattr = NULL
};

int chardev_vfs_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset) {
    chardev_t *cdev = (chardev_t *)node->fs_private;
    if (!cdev || !cdev->ops || !cdev->ops->read) {
        serial_printf(COM1_BASE, "ERROR: chardev_vfs_read: Invalid device\n");
        return -1;
    }
    
    // DEBUG
    serial_printf(COM1_BASE, "chardev_vfs_read: Reading from %s, size=%u, offset=%u\n", 
                  cdev->name, size, offset);
    
    int result = cdev->ops->read(buf, size, offset, cdev->priv);
    
    serial_printf(COM1_BASE, "chardev_vfs_read: Result = %d\n", result);
    
    return result;
}

int chardev_vfs_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset) {
    chardev_t *cdev = (chardev_t *)node->fs_private;
    if (!cdev || !cdev->ops || !cdev->ops->write) {
        serial_printf(COM1_BASE, "ERROR: chardev_vfs_write: Invalid device\n");
        return -1;
    }
    
    serial_printf(COM1_BASE, "chardev_vfs_write: Writing to %s, size=%u\n", cdev->name, size);
    
    return cdev->ops->write(buf, size, offset, cdev->priv);
}

void chardev_vfs_release(vfs_node_t *node) {
    chardev_t *cdev = (chardev_t *)node->fs_private;
    if (cdev) {
        // Decrementar refcount del chardev
        if (cdev->refcount > 0) {
            cdev->refcount--;
        }
        
        // Si nadie más usa este chardev, no lo destruimos porque está en la tabla global
        // Solo lo destruiremos si es necesario (depende de tu diseño)
    }
    kernel_free(node);
}

vfs_node_t *chardev_to_vfs_node(chardev_t *cdev) {
    if (!cdev) {
        serial_printf(COM1_BASE, "ERROR: chardev_to_vfs_node: NULL device\n");
        return NULL;
    }
    
    vfs_node_t *vn = (vfs_node_t *)kernel_malloc(sizeof(vfs_node_t));
    if (!vn) {
        serial_printf(COM1_BASE, "ERROR: chardev_to_vfs_node: Out of memory\n");
        return NULL;
    }
    
    memset(vn, 0, sizeof(vfs_node_t));
    strncpy(vn->name, cdev->name, VFS_NAME_MAX - 1);
    vn->name[VFS_NAME_MAX - 1] = '\0';
    vn->type = VFS_NODE_CHRDEV;
    vn->fs_private = cdev;
    vn->ops = &chardev_vnode_ops;
    vn->refcount = 1;
    
    // DEBUG
    serial_printf(COM1_BASE, "chardev_to_vfs_node: Created VFS node for %s at %p\n",
                 cdev->name, vn);
    
    return vn;
}

// ==================== FUNCIÓN DE BÚSQUEDA ====================
vfs_node_t *chardev_vfs_find(const char *name) {
    serial_printf(COM1_BASE, "DEBUG: chardev_vfs_find: Looking for '%s'\n", name);
    
    // Buscar en la tabla global de dispositivos especiales
    for (int i = 0; i < special_devices_count; i++) {
        if (special_devices[i] && strcmp(special_devices[i]->name, name) == 0) {
            serial_printf(COM1_BASE, "DEBUG: Found device %s at index %d\n", name, i);
            
            // Crear nodo VFS para este dispositivo
            vfs_node_t *node = chardev_to_vfs_node(special_devices[i]);
            if (node) {
                // Incrementar refcount del dispositivo
                special_devices[i]->refcount++;
                serial_printf(COM1_BASE, "DEBUG: Created VFS node %p, device refcount=%u\n",
                             node, special_devices[i]->refcount);
                return node;
            }
        }
    }
    
    serial_printf(COM1_BASE, "ERROR: chardev_vfs_find: Device '%s' not found\n", name);
    serial_printf(COM1_BASE, "DEBUG: Total devices registered: %d\n", special_devices_count);
    
    // DEBUG: Listar dispositivos disponibles
    for (int i = 0; i < special_devices_count; i++) {
        if (special_devices[i]) {
            serial_printf(COM1_BASE, "  [%d] %s (type: %d, refcount: %u)\n",
                         i, special_devices[i]->name, 
                         special_devices[i]->type, 
                         special_devices[i]->refcount);
        }
    }
    
    return NULL;
}