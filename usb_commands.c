#include "usb_commands.h"
#include "usb_core.h"
#include "usb_mass_storage.h"
#include "usb_disk_wrapper.h"
#include "terminal.h"
#include "kernel.h"
#include "fat32.h"
#include "vfs.h"
#include "string.h"

// Helper para parsear argumentos
static bool parse_device_id(const char* arg, uint32_t* out_id) {
    if (!arg || arg[0] == '\0') return false;
    *out_id = strtoul(arg, NULL, 10);
    return true;
}

// Comando: usb list - Listar dispositivos USB
void cmd_usb_list(void) {
    terminal_puts(&main_terminal, "\n=== USB Controllers ===\n");
    
    for (uint8_t i = 0; i < usb_controller_count; i++) {
        usb_controller_t* ctrl = &usb_controllers[i];
        terminal_printf(&main_terminal, "Controller %u: %s at %02x:%02x.%x\n",
                       i,
                       usb_get_speed_name(ctrl->type),
                       ctrl->pci_dev->bus,
                       ctrl->pci_dev->device,
                       ctrl->pci_dev->function);
    }
    
    terminal_puts(&main_terminal, "\n=== USB Devices ===\n");
    uint32_t total_devices = 0;
    
    for (uint8_t i = 0; i < usb_controller_count; i++) {
        usb_controller_t* ctrl = &usb_controllers[i];
        
        for (uint8_t j = 0; j < USB_MAX_DEVICES; j++) {
            usb_device_t* dev = &ctrl->devices[j];
            
            if (!dev->connected) continue;
            
            terminal_printf(&main_terminal, "  Device %u.%u: %s\n",
                           i, j, usb_get_class_name(dev->class_code));
            terminal_printf(&main_terminal, "    VID:PID = %04x:%04x\n",
                           dev->descriptor.idVendor,
                           dev->descriptor.idProduct);
            terminal_printf(&main_terminal, "    Address = %u, Port = %u\n",
                           dev->address, dev->port);
            
            total_devices++;
        }
    }
    
    if (total_devices == 0) {
        terminal_puts(&main_terminal, "No USB devices connected\n");
    } else {
        terminal_printf(&main_terminal, "\nTotal: %u device(s)\n", total_devices);
    }
}

// Comando: usb storage - Listar dispositivos de almacenamiento USB
void cmd_usb_storage(void) {
    terminal_puts(&main_terminal, "\n=== USB Storage Devices ===\n");
    
    uint32_t count = usb_msc_get_device_count();
    
    if (count == 0) {
        terminal_puts(&main_terminal, "No USB storage devices found\n");
        return;
    }
    
    for (uint8_t i = 0; i < count; i++) {
        usb_msc_device_t* msc = usb_msc_get_device(i);
        if (!msc || !msc->initialized) continue;
        
        uint64_t size_mb = ((uint64_t)msc->block_count * msc->block_size) / (1024 * 1024);
        uint64_t size_gb = size_mb / 1024;
        
        terminal_printf(&main_terminal, "USB%u:\n", i);
        terminal_printf(&main_terminal, "  Capacity: %u MB", (uint32_t)size_mb);
        if (size_gb > 0) {
            terminal_printf(&main_terminal, " (%u GB)", (uint32_t)size_gb);
        }
        terminal_puts(&main_terminal, "\n");
        terminal_printf(&main_terminal, "  Block size: %u bytes\n", msc->block_size);
        terminal_printf(&main_terminal, "  Block count: %u\n", msc->block_count);
        terminal_printf(&main_terminal, "  Drive number: 0x%02x\n", USB_DISK_BASE_ID + i);
    }
}

// Comando: usb scan - Re-escanear buses USB
void cmd_usb_scan(void) {
    terminal_puts(&main_terminal, "Scanning USB buses...\n");
    usb_scan_for_storage();
    terminal_puts(&main_terminal, "Scan complete\n");
}

// Comando: usb info <id> - Información detallada del dispositivo
void cmd_usb_info(const char* device_str) {
    uint32_t device_id;
    
    if (!parse_device_id(device_str, &device_id)) {
        terminal_puts(&main_terminal, "Usage: usb info <device_id>\n");
        return;
    }
    
    if (device_id >= usb_msc_get_device_count()) {
        terminal_printf(&main_terminal, "Error: Device USB%u not found\n", device_id);
        return;
    }
    
    usb_msc_device_t* msc = usb_msc_get_device(device_id);
    if (!msc || !msc->initialized) {
        terminal_puts(&main_terminal, "Error: Device not initialized\n");
        return;
    }
    
    usb_device_t* usb_dev = msc->usb_device;
    
    terminal_printf(&main_terminal, "\n=== USB Device %u Information ===\n", device_id);
    terminal_printf(&main_terminal, "Vendor ID:  0x%04x\n", usb_dev->descriptor.idVendor);
    terminal_printf(&main_terminal, "Product ID: 0x%04x\n", usb_dev->descriptor.idProduct);
    terminal_printf(&main_terminal, "Class:      %s (0x%02x)\n", 
                   usb_get_class_name(usb_dev->class_code), usb_dev->class_code);
    terminal_printf(&main_terminal, "Subclass:   0x%02x\n", usb_dev->subclass);
    terminal_printf(&main_terminal, "Protocol:   0x%02x\n", usb_dev->protocol);
    terminal_printf(&main_terminal, "Address:    %u\n", usb_dev->address);
    terminal_printf(&main_terminal, "Port:       %u\n", usb_dev->port);
    
    terminal_puts(&main_terminal, "\n--- Storage Information ---\n");
    uint64_t size_mb = ((uint64_t)msc->block_count * msc->block_size) / (1024 * 1024);
    terminal_printf(&main_terminal, "Capacity:   %u MB\n", (uint32_t)size_mb);
    terminal_printf(&main_terminal, "Block size: %u bytes\n", msc->block_size);
    terminal_printf(&main_terminal, "Blocks:     %u\n", msc->block_count);
    terminal_printf(&main_terminal, "Endpoints:  IN=0x%02x, OUT=0x%02x\n", 
                   msc->ep_in, msc->ep_out);
    terminal_printf(&main_terminal, "Max LUN:    %u\n", msc->max_lun);
}

// Handler principal para comandos USB
void cmd_usb(const char* args) {
    if (!args || args[0] == '\0') {
        terminal_puts(&main_terminal, "USB Commands:\n");
        terminal_puts(&main_terminal, "  usb list              - List all USB devices\n");
        terminal_puts(&main_terminal, "  usb storage           - List USB storage devices\n");
        terminal_puts(&main_terminal, "  usb scan              - Scan for new devices\n");
        terminal_puts(&main_terminal, "  usb info <id>         - Show device information\n");
        terminal_puts(&main_terminal, "  usb mount <id> <path> - Mount USB device\n");
        terminal_puts(&main_terminal, "  usb unmount <path>    - Unmount device\n");
        terminal_puts(&main_terminal, "  usb format <id> [lbl] - Format device as FAT32\n");
        return;
    }
    
    char subcommand[32];
    char subargs[256];
    
    // Parsear subcomando
    int parsed = sscanf(args, "%31s %255[^\n]", subcommand, subargs);
    
    if (parsed < 1) return;
    
    if (strcmp(subcommand, "list") == 0) {
        cmd_usb_list();
    } else if (strcmp(subcommand, "storage") == 0) {
        cmd_usb_storage();
    } else if (strcmp(subcommand, "scan") == 0) {
        cmd_usb_scan();
    } else if (strcmp(subcommand, "info") == 0) {
        cmd_usb_info(parsed > 1 ? subargs : "");
    } else {
        terminal_printf(&main_terminal, "Unknown USB command: %s\n", subcommand);
        terminal_puts(&main_terminal, "Type 'usb' for help\n");
    }
}