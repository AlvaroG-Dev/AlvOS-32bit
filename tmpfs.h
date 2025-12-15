/* tmpfs.h */
#ifndef TMPFS_H
#define TMPFS_H

#include "vfs.h"

/* Declaración de la instancia global del FS */
extern vfs_fs_type_t tmpfs_type;

/* Prototipo de la función mount */
int tmpfs_mount(void *device, vfs_superblock_t **out_sb);
void test_tmpfs(void);

#endif