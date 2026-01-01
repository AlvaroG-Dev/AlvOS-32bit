// chardev.h
#ifndef CHARDEV_H
#define CHARDEV_H

#include <stdint.h>
#include <stddef.h>

// Tipos de dispositivos de carácter
typedef enum {
    CHARDEV_NULL = 0,
    CHARDEV_ZERO = 1,
    CHARDEV_RANDOM = 2,
    CHARDEV_URANDOM = 3,
    CHARDEV_FULL = 4,     // /dev/full (siempre devuelve ENOSPC)
    CHARDEV_TTY = 5,      // Terminal virtual
    CHARDEV_CONSOLE = 6,  // Consola del sistema
    CHARDEV_PORT = 7      // Puerto (COM1/COM2)
} chardev_type_t;

// Operaciones de dispositivo de carácter
typedef struct chardev_ops {
    int (*read)(uint8_t *buf, uint32_t size, uint32_t offset, void *priv);
    int (*write)(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv);
    int (*ioctl)(uint32_t cmd, void *arg, void *priv);
    int (*poll)(void *priv);  // Para operaciones no bloqueantes
    void (*release)(void *priv);
} chardev_ops_t;

// Dispositivo de carácter
typedef struct chardev {
    char name[16];
    chardev_type_t type;
    chardev_ops_t *ops;
    void *priv;
    uint32_t refcount;
} chardev_t;

extern int special_devices_count;
extern chardev_t *special_devices[];

// Prototipos
void chardev_init(void);
chardev_t *chardev_create(chardev_type_t type, const char *name);
void chardev_destroy(chardev_t *dev);
int chardev_register(chardev_t *dev);
chardev_t *chardev_find(const char *name);

// Operaciones de dispositivos especiales
int null_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv);
int null_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv);

int zero_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv);
int zero_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv);

int random_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv);
int urandom_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv);

#endif // CHARDEV_H