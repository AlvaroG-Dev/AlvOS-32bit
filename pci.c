#include "pci.h"
#include "io.h"
#include "kernel.h"
#include "string.h"
#include "terminal.h"

// Tabla global de dispositivos PCI
pci_device_t pci_devices[MAX_PCI_DEVICES];
uint32_t pci_device_count = 0;
static bool pci_initialized = false;
// Nombres de clases PCI (simplificado)
static const char *class_names[] = {
    [0x00] = "Unclassified",    [0x01] = "Mass Storage",
    [0x02] = "Network",         [0x03] = "Display",
    [0x04] = "Multimedia",      [0x05] = "Memory",
    [0x06] = "Bridge",          [0x07] = "Communication",
    [0x08] = "System",          [0x09] = "Input",
    [0x0A] = "Docking Station", [0x0B] = "Processor",
    [0x0C] = "Serial Bus",      [0x0D] = "Wireless",
    [0x0E] = "Intelligent I/O", [0x0F] = "Satellite",
    [0x10] = "Encryption",      [0x11] = "Signal Processing"};
// Algunos vendor IDs conocidos
static struct {
  uint16_t id;
  const char *name;
} vendor_names[] = {{0x8086, "Intel"},
                    {0x1022, "AMD"},
                    {0x10DE, "NVIDIA"},
                    {0x1002, "ATI/AMD"},
                    {0x8000, "Trigem Computer"},
                    {0x10EC, "Realtek"},
                    {0x1106, "VIA"},
                    {0x1274, "Ensoniq"},
                    {0x1234, "QEMU"},
                    {0x15AD, "VMware"},
                    {0x80EE, "VirtualBox"},
                    {0x0000, NULL}};
// ========================================================================
// FUNCIONES DE ACCESO AL CONFIGURATION SPACE
// ========================================================================
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t offset) {
  uint32_t address = 0x80000000 | ((uint32_t)bus << 16) |
                     ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                     (offset & 0xFC);

  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t offset) {
  uint32_t dword = pci_config_read_dword(bus, device, function, offset);
  return (uint16_t)(dword >> ((offset & 2) * 8));
}
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset) {
  uint32_t dword = pci_config_read_dword(bus, device, function, offset);
  return (uint8_t)(dword >> ((offset & 3) * 8));
}
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint32_t data) {
  uint32_t address = 0x80000000 | ((uint32_t)bus << 16) |
                     ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                     (offset & 0xFC);

  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, data);
}
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint16_t data) {
  uint32_t address = 0x80000000 | ((uint32_t)bus << 16) |
                     ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                     (offset & 0xFC);

  outl(PCI_CONFIG_ADDRESS, address);
  outw(PCI_CONFIG_DATA + (offset & 2), data);
}
void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint8_t data) {
  uint32_t address = 0x80000000 | ((uint32_t)bus << 16) |
                     ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                     (offset & 0xFC);

  outl(PCI_CONFIG_ADDRESS, address);
  outb(PCI_CONFIG_DATA + (offset & 3), data);
}
// ========================================================================
// FUNCIONES DE ENUMERACIÓN
// ========================================================================
void pci_init(void) {
  if (pci_initialized) return;

  terminal_puts(&main_terminal, "Initializing PCI subsystem...\r\n");

  pci_device_count = 0;
  memset(pci_devices, 0, sizeof(pci_devices));

  // Escanear todos los buses
  pci_scan_all_buses();

  pci_initialized = true;
  terminal_printf(&main_terminal,
                  "PCI initialization complete. Found %u devices.\r\n",
                  pci_device_count);
}
void pci_scan_all_buses(void) {
  // Primero verificar si el host bridge es multi-function
  uint8_t header_type = pci_config_read_byte(0, 0, 0, PCI_HEADER_TYPE);

  if ((header_type & 0x80) == 0) {
    // Single PCI host controller
    pci_scan_bus(0);
  } else {
    // Multiple PCI host controllers
    for (uint16_t function = 0; function < 8; function++) {
      uint16_t vendor_id = pci_config_read_word(0, 0, function, PCI_VENDOR_ID);
      if (vendor_id != 0xFFFF) {
        pci_scan_bus(function);
      }
    }
  }
}
void pci_scan_bus(uint8_t bus) {
  for (uint8_t device = 0; device < 32; device++) {
    pci_scan_device(bus, device);
  }
}
void pci_scan_device(uint8_t bus, uint8_t device) {
  uint16_t vendor_id = pci_config_read_word(bus, device, 0, PCI_VENDOR_ID);

  if (vendor_id == 0xFFFF) {
    return; // No device present
  }

  pci_scan_function(bus, device, 0);

  // Check if multi-function device
  uint8_t header_type = pci_config_read_byte(bus, device, 0, PCI_HEADER_TYPE);
  if (header_type & 0x80) {
    // Multi-function device
    for (uint8_t function = 1; function < 8; function++) {
      vendor_id = pci_config_read_word(bus, device, function, PCI_VENDOR_ID);
      if (vendor_id != 0xFFFF) {
        pci_scan_function(bus, device, function);
      }
    }
  }
}
void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function) {
  if (pci_device_count >= MAX_PCI_DEVICES) {
    return;
  }

  pci_device_t *pci_dev = &pci_devices[pci_device_count];

  // Leer información básica
  pci_dev->bus = bus;
  pci_dev->device = device;
  pci_dev->function = function;
  pci_dev->present = true;

  pci_dev->vendor_id =
      pci_config_read_word(bus, device, function, PCI_VENDOR_ID);
  pci_dev->device_id =
      pci_config_read_word(bus, device, function, PCI_DEVICE_ID);
  pci_dev->class_code =
      pci_config_read_byte(bus, device, function, PCI_CLASS_CODE);
  pci_dev->subclass = pci_config_read_byte(bus, device, function, PCI_SUBCLASS);
  pci_dev->prog_if = pci_config_read_byte(bus, device, function, PCI_PROG_IF);
  pci_dev->revision_id =
      pci_config_read_byte(bus, device, function, PCI_REVISION_ID);
  pci_dev->header_type =
      pci_config_read_byte(bus, device, function, PCI_HEADER_TYPE) & 0x7F;
  pci_dev->interrupt_line =
      pci_config_read_byte(bus, device, function, PCI_INTERRUPT_LINE);
  pci_dev->interrupt_pin =
      pci_config_read_byte(bus, device, function, PCI_INTERRUPT_PIN);

  // Leer BARs
  pci_read_bars(pci_dev);

  pci_device_count++;

  // Si es un bridge PCI-to-PCI, escanear el bus secundario
  if (pci_dev->class_code == PCI_CLASS_BRIDGE && pci_dev->subclass == 0x04) {
    uint8_t secondary_bus = pci_config_read_byte(bus, device, function, 0x19);
    if (secondary_bus > 0) {
      pci_scan_bus(secondary_bus);
    }
  }
}
// ========================================================================
// FUNCIONES DE UTILIDAD PARA BARS
// ========================================================================
uint32_t pci_get_bar_size(uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t bar_num) {
  uint8_t bar_offset = PCI_BAR0 + (bar_num * 4);

  // Leer valor actual
  uint32_t original_value =
      pci_config_read_dword(bus, device, function, bar_offset);

  // Escribir todos los bits a 1
  pci_config_write_dword(bus, device, function, bar_offset, 0xFFFFFFFF);

  // Leer de vuelta para ver qué bits son implementados
  uint32_t size_mask = pci_config_read_dword(bus, device, function, bar_offset);

  // Restaurar valor original
  pci_config_write_dword(bus, device, function, bar_offset, original_value);

  if (size_mask == 0) {
    return 0; // BAR no implementado
  }

  // Calcular tamaño
  if (original_value & 1) {
    // I/O BAR
    size_mask &= 0xFFFFFFFC; // Mask out type bits
    return (~size_mask) + 1;
  } else {
    // Memory BAR
    size_mask &= 0xFFFFFFF0; // Mask out type bits
    return (~size_mask) + 1;
  }
}
void pci_read_bars(pci_device_t *device) {
  uint8_t max_bars = (device->header_type == 0) ? 6 : 2;

  for (uint8_t i = 0; i < max_bars; i++) {
    uint8_t bar_offset = PCI_BAR0 + (i * 4);
    uint32_t bar_value = pci_config_read_dword(device->bus, device->device,
                                               device->function, bar_offset);

    device->bars[i].is_valid = false;

    if (bar_value == 0) {
      continue; // BAR no implementado
    }

    device->bars[i].is_valid = true;
    device->bars[i].size =
        pci_get_bar_size(device->bus, device->device, device->function, i);

    if (bar_value & 1) {
      // I/O BAR
      device->bars[i].type = PCI_BAR_TYPE_IO;
      device->bars[i].address = bar_value & 0xFFFFFFFC;
      device->bars[i].is_64bit = false;
      device->bars[i].is_prefetchable = false;
    } else {
      // Memory BAR
      device->bars[i].type = PCI_BAR_TYPE_MEMORY;
      device->bars[i].address = bar_value & 0xFFFFFFF0;
      device->bars[i].is_prefetchable = (bar_value & PCI_BAR_PREFETCHABLE) != 0;

      // Check if 64-bit
      uint8_t memory_type = (bar_value >> 1) & 3;
      if (memory_type == 2) {
        device->bars[i].is_64bit = true;
        // Para BARs de 64 bits, el siguiente BAR contiene los 32 bits
        // superiores
        if (i + 1 < max_bars) {
          uint32_t high_dword = pci_config_read_dword(
              device->bus, device->device, device->function, bar_offset + 4);
          device->bars[i].address |= ((uint64_t)high_dword << 32);
          i++; // Skip next BAR as it's part of this 64-bit BAR
          device->bars[i].is_valid = false; // Mark next BAR as invalid/used
        }
      } else {
        device->bars[i].is_64bit = false;
      }
    }
  }
}
// ========================================================================
// FUNCIONES DE BÚSQUEDA Y CONFIGURACIÓN
// ========================================================================
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
  for (uint32_t i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].vendor_id == vendor_id &&
        pci_devices[i].device_id == device_id) {
      return &pci_devices[i];
    }
  }
  return NULL;
}
pci_device_t *pci_find_device_by_class(uint8_t class_code, uint8_t subclass) {
  for (uint32_t i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].class_code == class_code &&
        (subclass == 0xFF || pci_devices[i].subclass == subclass)) {
      return &pci_devices[i];
    }
  }
  return NULL;
}
void pci_enable_bus_mastering(pci_device_t *device) {
  uint16_t command = pci_config_read_word(device->bus, device->device,
                                          device->function, PCI_COMMAND);
  command |= PCI_COMMAND_MASTER;
  pci_config_write_word(device->bus, device->device, device->function,
                        PCI_COMMAND, command);
}
void pci_enable_memory_space(pci_device_t *device) {
  uint16_t command = pci_config_read_word(device->bus, device->device,
                                          device->function, PCI_COMMAND);
  command |= PCI_COMMAND_MEMORY;
  pci_config_write_word(device->bus, device->device, device->function,
                        PCI_COMMAND, command);
}
void pci_enable_io_space(pci_device_t *device) {
  uint16_t command = pci_config_read_word(device->bus, device->device,
                                          device->function, PCI_COMMAND);
  command |= PCI_COMMAND_IO;
  pci_config_write_word(device->bus, device->device, device->function,
                        PCI_COMMAND, command);
}
// ========================================================================
// FUNCIONES DE INFORMACIÓN
// ========================================================================
void pci_list_devices(void) {
  terminal_puts(&main_terminal, "\r\n=== PCI Device List ===\r\n");
  terminal_printf(&main_terminal, "Total devices: %u\r\n\r\n",
                  pci_device_count);

  for (uint32_t i = 0; i < pci_device_count; i++) {
    pci_device_t *dev = &pci_devices[i];

    terminal_printf(&main_terminal, "%02x:%02x.%x %04x:%04x %s (%s)\r\n",
                    dev->bus, dev->device, dev->function, dev->vendor_id,
                    dev->device_id, pci_get_class_name(dev->class_code),
                    pci_get_vendor_name(dev->vendor_id));

    // Mostrar BARs válidos
    for (int j = 0; j < 6; j++) {
      if (dev->bars[j].is_valid && dev->bars[j].size > 0) {
        const char *type =
            (dev->bars[j].type == PCI_BAR_TYPE_IO) ? "I/O" : "MEM";
        terminal_printf(&main_terminal, "  BAR%d: %s 0x%08x (size: %u%s%s)\r\n",
                        j, type, dev->bars[j].address, dev->bars[j].size,
                        dev->bars[j].is_64bit ? ", 64-bit" : "",
                        dev->bars[j].is_prefetchable ? ", prefetchable" : "");
      }
    }

    if (dev->interrupt_pin > 0) {
      terminal_printf(&main_terminal, "  IRQ: %u (pin %u)\r\n",
                      dev->interrupt_line, dev->interrupt_pin);
    }

    terminal_puts(&main_terminal, "\r\n");
  }
}
const char *pci_get_class_name(uint8_t class_code) {
  if (class_code < sizeof(class_names) / sizeof(class_names[0]) &&
      class_names[class_code]) {
    return class_names[class_code];
  }
  return "Unknown";
}
const char *pci_get_vendor_name(uint16_t vendor_id) {
  if (vendor_id == 0x8086)
    return "Intel Corporation";
  if (vendor_id == 0x10EC)
    return "Realtek Semiconductor Co., Ltd.";
  if (vendor_id == 0x1AF4)
    return "VirtIO";
  if (vendor_id == 0x1022)
    return "AMD";
  if (vendor_id == 0x1234)
    return "QEMU Virtual Video Controller";
  return "Unknown Vendor";
}

// ========================================================================
// DRIVER SYSTEM INTEGRATION
// ========================================================================
#include "driver_system.h"

static int pci_driver_init(driver_instance_t *drv, void *config) {
  (void)config;
  if (!drv)
    return -1;
  pci_init();
  return 0;
}

static int pci_driver_start(driver_instance_t *drv) {
  if (!drv)
    return -1;
  terminal_printf(&main_terminal,
                  "PCI driver: Started. Enumerated %u devices.\r\n",
                  pci_device_count);
  return 0;
}

static int pci_driver_stop(driver_instance_t *drv) {
  if (!drv)
    return -1;
  return 0;
}

static int pci_driver_cleanup(driver_instance_t *drv) {
  if (!drv)
    return -1;
  return 0;
}

static int pci_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
  if (!drv)
    return -1;

  switch (cmd) {
  case 0x3001: // List devices
    pci_list_devices();
    return 0;
  case 0x3002: { // Find device by ID
    struct {
      uint16_t v;
      uint16_t d;
      pci_device_t **out;
    } *f = arg;
    if (f && f->out) {
      *f->out = pci_find_device(f->v, f->d);
      return 0;
    }
    return -1;
  }
  default:
    return -1;
  }
}

static driver_ops_t pci_driver_ops = {.init = pci_driver_init,
                                      .start = pci_driver_start,
                                      .stop = pci_driver_stop,
                                      .cleanup = pci_driver_cleanup,
                                      .ioctl = pci_driver_ioctl,
                                      .load_data = NULL};

static driver_type_info_t pci_driver_type = {.type = DRIVER_TYPE_BUS,
                                             .type_name = "pci_bus",
                                             .version = "1.0.0",
                                             .priv_data_size = 0,
                                             .default_ops = &pci_driver_ops,
                                             .validate_data = NULL,
                                             .print_info = NULL};

int pci_driver_register_type(void) {
  return driver_register_type(&pci_driver_type);
}

driver_instance_t *pci_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_BUS, name);
}