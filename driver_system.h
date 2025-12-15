// driver_system.h - Generic driver system
#ifndef DRIVER_SYSTEM_H
#define DRIVER_SYSTEM_H

#include <stdint.h>
#include "vfs.h"

#define DRIVER_MAX_TYPES 16
#define DRIVER_MAX_INSTANCES 32
#define DRIVER_NAME_MAX 32
#define DRIVER_VERSION_MAX 16

// Driver types
typedef enum {
    DRIVER_TYPE_UNKNOWN = 0,
    DRIVER_TYPE_KEYBOARD,
    DRIVER_TYPE_MOUSE,
    DRIVER_TYPE_AUDIO,
    DRIVER_TYPE_NETWORK,
    DRIVER_TYPE_STORAGE,
    DRIVER_TYPE_VIDEO,
    DRIVER_TYPE_USB,
    DRIVER_TYPE_MAX
} driver_type_t;

// Driver state
typedef enum {
    DRIVER_STATE_UNLOADED = 0,
    DRIVER_STATE_LOADING,
    DRIVER_STATE_LOADED,
    DRIVER_STATE_ACTIVE,
    DRIVER_STATE_ERROR
} driver_state_t;

// Forward declarations
struct driver_instance;
struct driver_type_info;

// Generic driver operations
typedef struct driver_ops {
    int (*init)(struct driver_instance *drv, void *config);
    int (*start)(struct driver_instance *drv);
    int (*stop)(struct driver_instance *drv);
    int (*cleanup)(struct driver_instance *drv);
    int (*ioctl)(struct driver_instance *drv, uint32_t cmd, void *arg);
    int (*load_data)(struct driver_instance *drv, const void *data, size_t size);
} driver_ops_t;

// Driver instance
typedef struct driver_instance {
    uint32_t id;
    char name[DRIVER_NAME_MAX];
    char version[DRIVER_VERSION_MAX];
    driver_type_t type;
    driver_state_t state;
    void *private_data;
    const driver_ops_t *ops;
    struct driver_type_info *type_info;
    struct driver_instance *next;
} driver_instance_t;

// Driver type information
typedef struct driver_type_info {
    driver_type_t type;
    char type_name[DRIVER_NAME_MAX];
    char version[DRIVER_VERSION_MAX];
    size_t private_data_size;
    const driver_ops_t *default_ops;
    int (*validate_data)(const void *data, size_t size);
    void (*print_info)(const driver_instance_t *drv);
} driver_type_info_t;

// Public API
int driver_system_init(void);
void driver_system_cleanup(void);

// Driver type registration
int driver_register_type(const driver_type_info_t *type_info);
driver_type_info_t *driver_get_type_info(driver_type_t type);

// Driver instance management
driver_instance_t *driver_create(driver_type_t type, const char *name);
int driver_destroy(driver_instance_t *drv);
driver_instance_t *driver_find_by_name(const char *name);
driver_instance_t *driver_find_by_type(driver_type_t type);

// Driver operations
int driver_init(driver_instance_t *drv, void *config);
int driver_start(driver_instance_t *drv);
int driver_stop(driver_instance_t *drv);
int driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg);

// File loading utilities
void *driver_load_binary_file(const char *filename, size_t *out_size);
int driver_unload_binary_file(void *data, size_t size);
int driver_load_from_file(driver_instance_t *drv, const char *filename);

// List and debug functions
void driver_list_all(void);
void driver_list_by_type(driver_type_t type);

#endif // DRIVER_SYSTEM_H