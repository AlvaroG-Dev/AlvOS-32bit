// driver_system.c - Generic driver system implementation
#include "driver_system.h"
#include "drivers/keyboard_driver.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "serial.h"
#include "vfs.h"
#include "boot_log.h"

extern Terminal main_terminal;

// Global driver system state
static struct {
    driver_type_info_t type_registry[DRIVER_MAX_TYPES];
    int type_count;
    driver_instance_t *driver_list;
    uint32_t next_id;
    int initialized;
} driver_system;

// Internal helpers
static driver_instance_t *allocate_driver_instance(void) {
    return (driver_instance_t *)kernel_malloc(sizeof(driver_instance_t));
}

static void free_driver_instance(driver_instance_t *drv) {
    if (drv) {
        if (drv->private_data) {
            kernel_free(drv->private_data);
        }
        kernel_free(drv);
    }
}

static void remove_from_list(driver_instance_t *drv) {
    if (!drv || !driver_system.driver_list) return;
    
    if (driver_system.driver_list == drv) {
        driver_system.driver_list = drv->next;
        return;
    }
    
    driver_instance_t *current = driver_system.driver_list;
    while (current->next && current->next != drv) {
        current = current->next;
    }
    
    if (current->next == drv) {
        current->next = drv->next;
    }
}

// Public API implementation
int driver_system_init(void) {
    if (driver_system.initialized) {
        terminal_printf(&main_terminal, "Driver system already initialized\r\n");
        return 0;
    }
    
    terminal_printf(&main_terminal, "Initializing generic driver system...\r\n");
    
    memset(&driver_system, 0, sizeof(driver_system));
    driver_system.next_id = 1;
    driver_system.initialized = 1;
    
    // Register built-in driver types
    if (keyboard_driver_register_type() != 0) {
        terminal_printf(&main_terminal, "WARNING: Failed to register keyboard driver type\r\n");
    }
    
    terminal_printf(&main_terminal, "Driver system initialized with %d driver types\r\n", 
                   driver_system.type_count);
    return 0;
}

void driver_system_cleanup(void) {
    if (!driver_system.initialized) return;
    
    
    // Stop and destroy all driver instances
    driver_instance_t *current = driver_system.driver_list;
    while (current) {
        driver_instance_t *next = current->next;
        
        if (current->state == DRIVER_STATE_ACTIVE) {
            driver_stop(current);
        }
        
        if (current->ops && current->ops->cleanup) {
            current->ops->cleanup(current);
        }
        
        boot_log_info("Destroyed driver: %s\r\n", current->name);
        free_driver_instance(current);
        current = next;
    }
    
    memset(&driver_system, 0, sizeof(driver_system));
    boot_log_info("Driver system cleaned up\r\n");
}

int driver_register_type(const driver_type_info_t *type_info) {
    if (!type_info || !driver_system.initialized) {
        return -1;
    }
    
    if (driver_system.type_count >= DRIVER_MAX_TYPES) {
        terminal_printf(&main_terminal, "ERROR: Driver type registry full\r\n");
        return -1;
    }
    
    // Check for duplicate types
    for (int i = 0; i < driver_system.type_count; i++) {
        if (driver_system.type_registry[i].type == type_info->type) {
            terminal_printf(&main_terminal, "WARNING: Driver type %d already registered\r\n", 
                           type_info->type);
            return -1;
        }
    }
    
    driver_system.type_registry[driver_system.type_count] = *type_info;
    driver_system.type_count++;
    
    terminal_printf(&main_terminal, "Registered driver type: %s (%d)\r\n", 
                   type_info->type_name, type_info->type);
    return 0;
}

driver_type_info_t *driver_get_type_info(driver_type_t type) {
    for (int i = 0; i < driver_system.type_count; i++) {
        if (driver_system.type_registry[i].type == type) {
            return &driver_system.type_registry[i];
        }
    }
    return NULL;
}

driver_instance_t *driver_create(driver_type_t type, const char *name) {
    if (!name || !driver_system.initialized) {
        return NULL;
    }
    
    driver_type_info_t *type_info = driver_get_type_info(type);
    if (!type_info) {
        terminal_printf(&main_terminal, "ERROR: Unknown driver type: %d\r\n", type);
        return NULL;
    }
    
    // Check for duplicate names
    if (driver_find_by_name(name)) {
        terminal_printf(&main_terminal, "ERROR: Driver '%s' already exists\r\n", name);
        return NULL;
    }
    
    driver_instance_t *drv = allocate_driver_instance();
    if (!drv) {
        terminal_printf(&main_terminal, "ERROR: Failed to allocate driver instance\r\n");
        return NULL;
    }
    
    memset(drv, 0, sizeof(driver_instance_t));
    drv->id = driver_system.next_id++;
    strncpy(drv->name, name, DRIVER_NAME_MAX - 1);
    drv->name[DRIVER_NAME_MAX - 1] = '\0';
    drv->type = type;
    drv->state = DRIVER_STATE_UNLOADED;
    drv->type_info = type_info;
    drv->ops = type_info->default_ops;
    if (type_info->version[0]) {
        strncpy(drv->version, type_info->version, DRIVER_VERSION_MAX - 1);
        drv->version[DRIVER_VERSION_MAX - 1] = '\0';
    }
    // Allocate private data if needed
    if (type_info->private_data_size > 0) {
        drv->private_data = kernel_malloc(type_info->private_data_size);
        if (!drv->private_data) {
            terminal_printf(&main_terminal, "ERROR: Failed to allocate private data\r\n");
            free_driver_instance(drv);
            return NULL;
        }
        memset(drv->private_data, 0, type_info->private_data_size);
    }
    
    // Add to list
    drv->next = driver_system.driver_list;
    driver_system.driver_list = drv;
    
    terminal_printf(&main_terminal, "Created driver: %s (ID: %u, Type: %s)\r\n", 
                   drv->name, drv->id, type_info->type_name);
    return drv;
}

int driver_destroy(driver_instance_t *drv) {
    if (!drv) return -1;
    
    // Stop driver if active
    if (drv->state == DRIVER_STATE_ACTIVE) {
        driver_stop(drv);
    }
    
    // Call cleanup
    if (drv->ops && drv->ops->cleanup) {
        drv->ops->cleanup(drv);
    }
    
    // Remove from list
    remove_from_list(drv);
    
    terminal_printf(&main_terminal, "Destroyed driver: %s\r\n", drv->name);
    free_driver_instance(drv);
    return 0;
}

driver_instance_t *driver_find_by_name(const char *name) {
    if (!name) return NULL;
    
    driver_instance_t *current = driver_system.driver_list;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

driver_instance_t *driver_find_by_type(driver_type_t type) {
    driver_instance_t *current = driver_system.driver_list;
    while (current) {
        if (current->type == type) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int driver_init(driver_instance_t *drv, void *config) {
    if (!drv || !drv->ops) return -1;
    
    if (drv->state != DRIVER_STATE_UNLOADED && drv->state != DRIVER_STATE_LOADED) {
        terminal_printf(&main_terminal, "ERROR: Driver %s not in correct state for init\r\n", 
                       drv->name);
        return -1;
    }
    
    drv->state = DRIVER_STATE_LOADING;
    
    int result = 0;
    if (drv->ops->init) {
        result = drv->ops->init(drv, config);
    }
    
    if (result == 0) {
        drv->state = DRIVER_STATE_LOADED;
        terminal_printf(&main_terminal, "Driver %s initialized\r\n", drv->name);
    } else {
        drv->state = DRIVER_STATE_ERROR;
        terminal_printf(&main_terminal, "ERROR: Failed to initialize driver %s\r\n", drv->name);
    }
    
    return result;
}

int driver_start(driver_instance_t *drv) {
    if (!drv || !drv->ops) return -1;
    
    if (drv->state != DRIVER_STATE_LOADED) {
        terminal_printf(&main_terminal, "ERROR: Driver %s not loaded\r\n", drv->name);
        return -1;
    }
    
    int result = 0;
    if (drv->ops->start) {
        result = drv->ops->start(drv);
    }
    
    if (result == 0) {
        drv->state = DRIVER_STATE_ACTIVE;
        terminal_printf(&main_terminal, "Driver %s started\r\n", drv->name);
    } else {
        drv->state = DRIVER_STATE_ERROR;
        terminal_printf(&main_terminal, "ERROR: Failed to start driver %s\r\n", drv->name);
    }
    
    return result;
}

int driver_stop(driver_instance_t *drv) {
    if (!drv || !drv->ops) return -1;
    
    if (drv->state != DRIVER_STATE_ACTIVE) {
        boot_log_warn("WARNING: Driver %s not active\r\n", drv->name);
        return 0;
    }
    
    int result = 0;
    if (drv->ops->stop) {
        result = drv->ops->stop(drv);
    }
    
    drv->state = DRIVER_STATE_LOADED;
    if (boot_state.boot_phase) {
        boot_log_info("Driver %s stopped\r\n", drv->name);
    } else {
        terminal_printf(&main_terminal, "Driver %s stopped\r\n", drv->name);
    } 
    return result;
}

int driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
    if (!drv || !drv->ops || !drv->ops->ioctl) return -1;
    
    if (drv->state != DRIVER_STATE_ACTIVE) {
        terminal_printf(&main_terminal, "ERROR: Driver %s not active for ioctl\r\n", drv->name);
        return -1;
    }
    
    return drv->ops->ioctl(drv, cmd, arg);
}

void *driver_load_binary_file(const char *filename, size_t *out_size) {
    if (!filename || !out_size) return NULL;
    
    terminal_printf(&main_terminal, "Loading driver file: %s\r\n", filename);
    
    int fd = vfs_open(filename, VFS_O_RDONLY);
    if (fd < 0) {
        terminal_printf(&main_terminal, "ERROR: Cannot open driver file %s\r\n", filename);
        return NULL;
    }
    
    // Calculate file size by reading in chunks
    uint8_t buffer[512];
    size_t total_size = 0;
    int bytes_read;
    
    while ((bytes_read = vfs_read(fd, buffer, sizeof(buffer))) > 0) {
        total_size += bytes_read;
    }
    
    if (total_size == 0) {
        terminal_printf(&main_terminal, "ERROR: Empty driver file %s\r\n", filename);
        vfs_close(fd);
        return NULL;
    }
    
    // Reopen file to read from beginning
    vfs_close(fd);
    fd = vfs_open(filename, VFS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    
    // Allocate memory for entire file
    void *file_data = kernel_malloc(total_size);
    if (!file_data) {
        terminal_printf(&main_terminal, "ERROR: Cannot allocate %u bytes for driver file\r\n", 
                       (uint32_t)total_size);
        vfs_close(fd);
        return NULL;
    }
    
    // Read entire file
    size_t bytes_remaining = total_size;
    uint8_t *data_ptr = (uint8_t*)file_data;
    
    while (bytes_remaining > 0) {
        bytes_read = vfs_read(fd, data_ptr, 
                            bytes_remaining > sizeof(buffer) ? sizeof(buffer) : bytes_remaining);
        if (bytes_read <= 0) {
            terminal_printf(&main_terminal, "ERROR: Read failed at %u/%u bytes\r\n", 
                           (uint32_t)(total_size - bytes_remaining), (uint32_t)total_size);
            kernel_free(file_data);
            vfs_close(fd);
            return NULL;
        }
        data_ptr += bytes_read;
        bytes_remaining -= bytes_read;
    }
    
    vfs_close(fd);
    *out_size = total_size;
    
    terminal_printf(&main_terminal, "Successfully loaded driver file %s (%u bytes)\r\n", 
                   filename, (uint32_t)total_size);
    return file_data;
}

int driver_unload_binary_file(void *data, size_t size) {
    (void)size;  // Unused parameter
    if (!data) return -1;
    kernel_free(data);
    return 0;
}

int driver_load_from_file(driver_instance_t *drv, const char *filename) {
    if (!drv || !filename) return -1;
    
    size_t file_size;
    void *file_data = driver_load_binary_file(filename, &file_size);
    if (!file_data) {
        return -1;
    }
    
    // Validate data if type info provides validator
    if (drv->type_info && drv->type_info->validate_data) {
        if (drv->type_info->validate_data(file_data, file_size) != 0) {
            terminal_printf(&main_terminal, "ERROR: Invalid data in file %s\r\n", filename);
            kernel_free(file_data);
            return -1;
        }
    }
    
    int result = -1;
    if (drv->ops && drv->ops->load_data) {
        result = drv->ops->load_data(drv, file_data, file_size);
    }
    
    kernel_free(file_data);
    return result;
}

void driver_list_all(void) {
    terminal_printf(&main_terminal, "\r\n=== Driver List ===\r\n");
    
    if (!driver_system.driver_list) {
        terminal_printf(&main_terminal, "No drivers loaded\r\n");
        return;
    }
    
    driver_instance_t *current = driver_system.driver_list;
    int count = 0;
    
    while (current) {
        const char *state_str;
        switch (current->state) {
            case DRIVER_STATE_UNLOADED: state_str = "UNLOADED"; break;
            case DRIVER_STATE_LOADING:  state_str = "LOADING";  break;
            case DRIVER_STATE_LOADED:   state_str = "LOADED";   break;
            case DRIVER_STATE_ACTIVE:   state_str = "ACTIVE";   break;
            case DRIVER_STATE_ERROR:    state_str = "ERROR";    break;
            default:                    state_str = "UNKNOWN";  break;
        }
        
        terminal_printf(&main_terminal, "%d. %s [%s] - %s (%s)\r\n",
                       ++count, current->name, state_str,
                       current->type_info ? current->type_info->type_name : "Unknown",
                       current->version[0] ? current->version : "No version");
        
        // Print driver-specific info if available
        if (current->type_info && current->type_info->print_info) {
            current->type_info->print_info(current);
        }
        
        current = current->next;
    }
    
    terminal_printf(&main_terminal, "Total: %d drivers\r\n", count);
}

void driver_list_by_type(driver_type_t type) {
    driver_type_info_t *type_info = driver_get_type_info(type);
    const char *type_name = type_info ? type_info->type_name : "Unknown";
    
    terminal_printf(&main_terminal, "\r\n=== %s Drivers ===\r\n", type_name);
    
    driver_instance_t *current = driver_system.driver_list;
    int count = 0;
    
    while (current) {
        if (current->type == type) {
            const char *state_str;
            switch (current->state) {
                case DRIVER_STATE_UNLOADED: state_str = "UNLOADED"; break;
                case DRIVER_STATE_LOADING:  state_str = "LOADING";  break;
                case DRIVER_STATE_LOADED:   state_str = "LOADED";   break;
                case DRIVER_STATE_ACTIVE:   state_str = "ACTIVE";   break;
                case DRIVER_STATE_ERROR:    state_str = "ERROR";    break;
                default:                    state_str = "UNKNOWN";  break;
            }
            
            terminal_printf(&main_terminal, "%d. %s [%s] - %s\r\n",
                           ++count, current->name, state_str,
                           current->version[0] ? current->version : "No version");
            
            if (current->type_info && current->type_info->print_info) {
                current->type_info->print_info(current);
            }
        }
        current = current->next;
    }
    
    if (count == 0) {
        terminal_printf(&main_terminal, "No %s drivers loaded\r\n", type_name);
    } else {
        terminal_printf(&main_terminal, "Total: %d %s drivers\r\n", count, type_name);
    }
}