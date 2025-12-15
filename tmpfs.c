// tmpfs.c - Fixed version with mkdir support
#include "tmpfs.h"
#include "vfs.h"
#include "string.h"
#include "terminal.h"
#include "kernel.h"
#include "log.h"

extern void *kernel_malloc(size_t);
extern int kernel_free(void*);

typedef struct tmp_node {
    char name[VFS_NAME_MAX];
    uint8_t type;
    uint8_t *data;
    uint32_t size;
    struct tmp_node *parent;
    struct tmp_node **children;
    int child_count;
} tmp_node_t;

typedef struct tmp_sb {
    tmp_node_t *root;
} tmp_sb_t;

// Forward declarations
static int tmp_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out);
static int tmp_create(vfs_node_t *parent, const char *name, vfs_node_t **out);
static int tmp_mkdir(vfs_node_t *parent, const char *name, vfs_node_t **out);
static int tmp_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset);
static int tmp_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset);
static int tmp_readdir(vfs_node_t *node, vfs_dirent_t *buf, uint32_t *count, uint32_t offset);
static void tmp_release(vfs_node_t *node);
static int tmp_unlink(vfs_node_t *parent, const char *name);

static vnode_ops_t tmp_vnode_ops = {
    .lookup = tmp_lookup,
    .create = tmp_create,
    .mkdir = tmp_mkdir,  // Add mkdir support
    .read = tmp_read,
    .write = tmp_write,
    .readdir = tmp_readdir,
    .release = tmp_release,
    .unlink = tmp_unlink
};

static vfs_node_t *tmpnode_to_vnode(tmp_node_t *tn, vfs_superblock_t *sb) {
    vfs_node_t *vn = (vfs_node_t*)kernel_malloc(sizeof(vfs_node_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    strncpy(vn->name, tn->name, VFS_NAME_MAX-1);
    vn->type = tn->type;
    vn->fs_private = tn;
    vn->ops = &tmp_vnode_ops;
    vn->sb = sb;
    vn->refcount = 1;
    return vn;
}

int tmpfs_mount(void *device, vfs_superblock_t **out_sb) {
    (void)device;
    vfs_superblock_t *sb = (vfs_superblock_t*)kernel_malloc(sizeof(vfs_superblock_t));
    if (!sb) return -1;
    memset(sb, 0, sizeof(*sb));
    strncpy(sb->fs_name, "tmpfs", sizeof(sb->fs_name)-1);
    
    tmp_sb_t *t = (tmp_sb_t*)kernel_malloc(sizeof(tmp_sb_t));
    if (!t) { 
        kernel_free(sb); 
        return -1; 
    }
    memset(t, 0, sizeof(*t));
    
    tmp_node_t *root = (tmp_node_t*)kernel_malloc(sizeof(tmp_node_t));
    if (!root) {
        kernel_free(t);
        kernel_free(sb);
        return -1;
    }
    memset(root, 0, sizeof(*root));
    strncpy(root->name, "/", VFS_NAME_MAX-1);
    root->type = VFS_NODE_DIR;
    root->parent = NULL;
    
    t->root = root;
    sb->private = t;
    sb->root = tmpnode_to_vnode(root, sb);
    sb->backing_device = NULL;
    *out_sb = sb;
    return 0;
}

static int tmp_add_child(tmp_node_t *parent, tmp_node_t *child) {
    tmp_node_t **newarr = (tmp_node_t**)kernel_malloc(sizeof(tmp_node_t*) * (parent->child_count + 1));
    if (!newarr) return -1;

    // Copy existing children
    for (int i = 0; i < parent->child_count; i++)
        newarr[i] = parent->children[i];

    // Add new child
    newarr[parent->child_count] = child;

    // Free old array if it existed
    if (parent->children) 
        kernel_free(parent->children);

    parent->children = newarr;
    parent->child_count++;
    child->parent = parent;
    return 0;
}

static int tmp_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out) {
    tmp_node_t *tn = (tmp_node_t*)parent->fs_private;
    if (tn->type != VFS_NODE_DIR) return -1;
    
    for (int i = 0; i < tn->child_count; i++) {
        if (strcmp(tn->children[i]->name, name) == 0) {
            *out = tmpnode_to_vnode(tn->children[i], parent->sb);
            return 0;
        }
    }
    return -1;
}

static int tmp_create(vfs_node_t *parent, const char *name, vfs_node_t **out) {
    tmp_node_t *pt = (tmp_node_t*)parent->fs_private;
    if (pt->type != VFS_NODE_DIR) return -1;
    
    // Check if already exists
    for (int i = 0; i < pt->child_count; i++) {
        if (strcmp(pt->children[i]->name, name) == 0) {
            return -1; // Already exists
        }
    }
    
    tmp_node_t *n = (tmp_node_t*)kernel_malloc(sizeof(tmp_node_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME_MAX-1);
    n->type = VFS_NODE_FILE;

    if (tmp_add_child(pt, n) != 0) {
        kernel_free(n);
        return -1;
    }

    *out = tmpnode_to_vnode(n, parent->sb);
    return 0;
}

static int tmp_mkdir(vfs_node_t *parent, const char *name, vfs_node_t **out) {
    tmp_node_t *pt = (tmp_node_t*)parent->fs_private;
    if (pt->type != VFS_NODE_DIR) return -1;
    
    // Check if already exists
    for (int i = 0; i < pt->child_count; i++) {
        if (strcmp(pt->children[i]->name, name) == 0) {
            return -1; // Already exists
        }
    }
    
    tmp_node_t *n = (tmp_node_t*)kernel_malloc(sizeof(tmp_node_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME_MAX-1);
    n->type = VFS_NODE_DIR;

    if (tmp_add_child(pt, n) != 0) {
        kernel_free(n);
        return -1;
    }

    *out = tmpnode_to_vnode(n, parent->sb);
    return 0;
}

static int tmp_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset) {
    tmp_node_t *tn = (tmp_node_t*)node->fs_private;
    if (tn->type != VFS_NODE_FILE) return -1;
    
    
    if (offset >= tn->size) {
        //log_message(LOG_INFO, "[TMPFS_READ] Offset beyond file size, returning 0\n");
        return 0;  // EOF
    }
    
    uint32_t tocopy = size;
    if (offset + tocopy > tn->size) {
        tocopy = tn->size - offset;
    }
    
    if (tn->data && tocopy > 0) {
        memcpy(buf, tn->data + offset, tocopy);
    } else {

    }
    
    return (int)tocopy;
}

static int tmp_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset) {
    tmp_node_t *tn = (tmp_node_t*)node->fs_private;
    if (tn->type != VFS_NODE_FILE) {
        terminal_puts(&main_terminal, "[TMPFS_WRITE] Not a file!\n");
        return -1;
    }
    
    uint32_t need = offset + size;
    
    // ✅ Si necesitamos más espacio, redimensionar
    if (need > tn->size) {
        uint8_t *newdata = (uint8_t*)kernel_malloc(need);
        if (!newdata) {
            return -1;
        }
        
        // Inicializar nuevo buffer a 0
        memset(newdata, 0, need);
        
        // Copiar datos existentes
        if (tn->data) {
            memcpy(newdata, tn->data, tn->size);
            kernel_free(tn->data);
        }
        
        tn->data = newdata;
        tn->size = need;
    }
    
    // ✅ Copiar datos
    if (tn->data) {
        memcpy(tn->data + offset, buf, size);
    } else {
        return -1;
    }
    
    return (int)size;
}

static int tmp_readdir(vfs_node_t *node, vfs_dirent_t *buf, uint32_t *count, uint32_t offset) {
    tmp_node_t *tn = (tmp_node_t*)node->fs_private;
    if (tn->type != VFS_NODE_DIR) return -1;
    
    uint32_t entries_written = 0;
    uint32_t max_entries = *count;
    
    for (uint32_t i = offset; i < (uint32_t)tn->child_count && entries_written < max_entries; i++) {
        strncpy(buf[entries_written].name, tn->children[i]->name, VFS_NAME_MAX-1);
        buf[entries_written].name[VFS_NAME_MAX-1] = '\0';
        buf[entries_written].type = tn->children[i]->type;
        entries_written++;
    }
    
    *count = entries_written;
    return 0;
}

static void tmp_release(vfs_node_t *node) {
    kernel_free(node);
}

static int tmp_unlink(vfs_node_t *parent, const char *name) {
    tmp_node_t *pt = (tmp_node_t*)parent->fs_private;
    if (pt->type != VFS_NODE_DIR) return -1;
    
    for (int i = 0; i < pt->child_count; i++) {
        if (strcmp(pt->children[i]->name, name) == 0) {
            tmp_node_t *child = pt->children[i];
            
            // Verificar si es directorio vacío
            if (child->type == VFS_NODE_DIR && child->child_count > 0) {
                return -1;  // No eliminar directorios no vacíos (como rm estándar sin -r)
            }
            
            // Liberar recursos
            if (child->data) kernel_free(child->data);
            if (child->children) kernel_free(child->children);
            kernel_free(child);
            
            // Remover de la lista de children
            for (int j = i; j < pt->child_count - 1; j++) {
                pt->children[j] = pt->children[j + 1];
            }
            pt->child_count--;
            
            return 0;
        }
    }
    return -1;  // No encontrado
}

vfs_fs_type_t tmpfs_type = {
    .name = "tmpfs",
    .mount = tmpfs_mount
};