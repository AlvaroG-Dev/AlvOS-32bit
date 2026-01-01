// chardev.c - Dispositivos de carácter simples unificados
#include "chardev.h"
#include "kernel.h"
#include "memutils.h"
#include "string.h"
#include "vfs.h"
#include "serial.h"
#include "irq.h"
#include <stdint.h>

// ==================== VARIABLES GLOBALES ====================
chardev_t *special_devices[32] = {0};
int special_devices_count = 0;

// ==================== /dev/null ====================
int null_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return 0; // Siempre EOF
}

int null_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return (int)size; // Acepta todo, descarta datos
}

int null_ioctl(uint32_t cmd, void *arg, void *priv) {
    (void)arg; (void)priv;
    switch (cmd) {
        case 0x01: // TIOCGETD
            return 0;
        case 0x02: // TIOCGWINSZ
            return -1;
        default:
            return -1;
    }
}

void null_release(void *priv) {
    (void)priv;
}

chardev_ops_t null_ops = {
    .read = null_read,
    .write = null_write,
    .ioctl = null_ioctl,
    .poll = NULL,
    .release = null_release
};

// ==================== /dev/zero ====================
int zero_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)offset; (void)priv;
    if (!buf || size == 0) return 0;
    memset(buf, 0, size);
    return (int)size;
}

int zero_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return (int)size; // Acepta escritura pero ignora
}

chardev_ops_t zero_ops = {
    .read = zero_read,
    .write = zero_write,
    .ioctl = null_ioctl,
    .poll = NULL,
    .release = null_release
};

// ==================== /dev/full ====================
int full_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return 0; // Siempre EOF
}

int full_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return -1; // Siempre falla con ENOSPC
}

chardev_ops_t full_ops = {
    .read = full_read,
    .write = full_write,
    .ioctl = null_ioctl,
    .poll = NULL,
    .release = null_release
};

// ==================== GENERADOR DE NÚMEROS ALEATORIOS ====================
uint32_t random_seed = 0x12345678;

uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#define ENTROPY_POOL_SIZE 4096
uint8_t entropy_pool[ENTROPY_POOL_SIZE];
uint32_t entropy_pool_index = 0;
uint32_t entropy_estimate = 0;

void refill_entropy_pool(void) {
    for (uint32_t i = 0; i < ENTROPY_POOL_SIZE; i++) {
        uint32_t value = xorshift32(&random_seed);
        value ^= (uint32_t)ticks_since_boot;
        asm volatile("rdtsc" : "=A"(value));
        entropy_pool[i] = (uint8_t)(value & 0xFF);
    }
    entropy_pool_index = 0;
    entropy_estimate = ENTROPY_POOL_SIZE * 8;
}

int random_read_internal(uint8_t *buf, uint32_t size, uint32_t offset, 
                                void *priv, int blocking) {
    (void)offset; (void)priv;
    
    if (!buf || size == 0) return 0;
    
    uint32_t bytes_read = 0;
    
    while (bytes_read < size) {
        if (entropy_pool_index >= ENTROPY_POOL_SIZE) {
            refill_entropy_pool();
        }
        
        uint32_t available = ENTROPY_POOL_SIZE - entropy_pool_index;
        uint32_t to_read = size - bytes_read;
        if (to_read > available) to_read = available;
        
        memcpy(buf + bytes_read, entropy_pool + entropy_pool_index, to_read);
        bytes_read += to_read;
        entropy_pool_index += to_read;
        
        if (entropy_estimate > (to_read * 8)) {
            entropy_estimate -= (to_read * 8);
        } else {
            entropy_estimate = 0;
        }
        
        if (!blocking && entropy_estimate == 0) break;
        if (blocking && entropy_estimate == 0) refill_entropy_pool();
    }
    
    return (int)bytes_read;
}

// ==================== /dev/random ====================
int random_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    return random_read_internal(buf, size, offset, priv, 1);
}

int random_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)buf; (void)offset; (void)priv;
    return -1; // No acepta escritura
}

int random_ioctl(uint32_t cmd, void *arg, void *priv) {
    switch (cmd) {
        case 0x80045200: // RNDGETENTCNT
            if (arg) *(int *)arg = (int)entropy_estimate;
            return 0;
        case 0x40085201: // RNDADDTOENTCNT
            if (arg) {
                entropy_estimate += *(int *)arg;
                if (entropy_estimate > ENTROPY_POOL_SIZE * 8)
                    entropy_estimate = ENTROPY_POOL_SIZE * 8;
            }
            return 0;
        default:
            return null_ioctl(cmd, arg, priv);
    }
}

chardev_ops_t random_ops = {
    .read = random_read,
    .write = random_write,
    .ioctl = random_ioctl,
    .poll = NULL,
    .release = null_release
};

// ==================== /dev/urandom ====================
int urandom_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    return random_read_internal(buf, size, offset, priv, 0);
}

int urandom_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)offset; (void)priv;
    
    if (!buf || size == 0) return 0;
    
    // Mezclar entropía adicional
    for (uint32_t i = 0; i < size && i < ENTROPY_POOL_SIZE; i++) {
        entropy_pool[(entropy_pool_index + i) % ENTROPY_POOL_SIZE] ^= buf[i];
    }
    
    entropy_estimate += size * 8;
    if (entropy_estimate > ENTROPY_POOL_SIZE * 8)
        entropy_estimate = ENTROPY_POOL_SIZE * 8;
    
    return (int)size;
}

chardev_ops_t urandom_ops = {
    .read = urandom_read,
    .write = urandom_write,
    .ioctl = random_ioctl,
    .poll = NULL,
    .release = null_release
};

// ==================== FUNCIONES DE GESTIÓN ====================
chardev_t *chardev_create(chardev_type_t type, const char *name) {
    chardev_t *dev = (chardev_t *)kernel_malloc(sizeof(chardev_t));
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(chardev_t));
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->type = type;
    dev->refcount = 1;
    
    // Asignar operaciones según el tipo
    switch (type) {
        case CHARDEV_NULL:
            dev->ops = &null_ops;
            break;
        case CHARDEV_ZERO:
            dev->ops = &zero_ops;
            break;
        case CHARDEV_RANDOM:
            dev->ops = &random_ops;
            break;
        case CHARDEV_URANDOM:
            dev->ops = &urandom_ops;
            break;
        case CHARDEV_FULL:
            dev->ops = &full_ops;
            break;
        case CHARDEV_TTY:
        case CHARDEV_CONSOLE:
        case CHARDEV_PORT:
            // Estos se manejan en sus propios archivos
            kernel_free(dev);
            return NULL;
        default:
            kernel_free(dev);
            return NULL;
    }
    
    return dev;
}

void chardev_destroy(chardev_t *dev) {
    if (!dev) return;
    
    if (dev->ops && dev->ops->release) {
        dev->ops->release(dev->priv);
    }
    
    kernel_free(dev);
}

int chardev_register(chardev_t *dev) {
    if (!dev || special_devices_count >= 32) {
        return -1;
    }
    
    special_devices[special_devices_count++] = dev;
    return 0;
}

chardev_t *chardev_find(const char *name) {
    for (int i = 0; i < special_devices_count; i++) {
        if (special_devices[i] && strcmp(special_devices[i]->name, name) == 0) {
            special_devices[i]->refcount++;
            return special_devices[i];
        }
    }
    return NULL;
}

// ==================== INICIALIZACIÓN ====================
void chardev_init(void) {
    serial_printf(COM1_BASE, "chardev_init: Initializing character devices\n");
    
    // Inicializar pool de entropía
    refill_entropy_pool();
    
    // Crear dispositivos básicos
    chardev_t *null_dev = chardev_create(CHARDEV_NULL, "null");
    chardev_t *zero_dev = chardev_create(CHARDEV_ZERO, "zero");
    chardev_t *random_dev = chardev_create(CHARDEV_RANDOM, "random");
    chardev_t *urandom_dev = chardev_create(CHARDEV_URANDOM, "urandom");
    chardev_t *full_dev = chardev_create(CHARDEV_FULL, "full");
    
    // Registrar todos
    if (null_dev) {
        chardev_register(null_dev);
        serial_printf(COM1_BASE, "chardev_init: Registered /dev/null\n");
    }
    if (zero_dev) {
        chardev_register(zero_dev);
        serial_printf(COM1_BASE, "chardev_init: Registered /dev/zero\n");
    }
    if (random_dev) {
        chardev_register(random_dev);
        serial_printf(COM1_BASE, "chardev_init: Registered /dev/random\n");
    }
    if (urandom_dev) {
        chardev_register(urandom_dev);
        serial_printf(COM1_BASE, "chardev_init: Registered /dev/urandom\n");
    }
    if (full_dev) {
        chardev_register(full_dev);
        serial_printf(COM1_BASE, "chardev_init: Registered /dev/full\n");
    }
    // Los dispositivos seriales (COM1/COM2) se inicializan en serial.c
    // Los dispositivos TTY/consola se inicializan en sus respectivos archivos
}