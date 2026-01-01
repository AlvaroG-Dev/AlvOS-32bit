// chardev_vfs.h
#ifndef CHARDEV_VFS_H
#define CHARDEV_VFS_H

#include "chardev.h"
#include "vfs.h"

/* Adaptador para convertir chardev a vfs_node_t */
vfs_node_t *chardev_to_vfs_node(chardev_t *cdev);
int chardev_vfs_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset);
int chardev_vfs_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset);
int chardev_vfs_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);
void chardev_vfs_release(vfs_node_t *node);
vfs_node_t *chardev_vfs_find(const char *name);

#endif // CHARDEV_VFS_H