#include "usb_core.h"
#include "io.h"
#include "kernel.h"
#include "pci.h"
#include "string.h"
#include "terminal.h"
#include "usb_ehci.h"
#include "usb_ohci.h"
#include "usb_uhci.h"


#include "usb_hid.h"
#include "usb_mass_storage.h"

// Global state
usb_controller_t usb_controllers[USB_MAX_CONTROLLERS];
uint8_t usb_controller_count = 0;
static bool usb_initialized = false;

static usb_driver_t *registered_drivers[16];
static uint8_t registered_driver_count = 0;

// ========================================================================
// INITIALIZATION
// ========================================================================

bool usb_init(void) {
  if (usb_initialized)
    return true;

  terminal_puts(&main_terminal, "Initializing USB subsystem...\r\n");

  memset(usb_controllers, 0, sizeof(usb_controllers));
  usb_controller_count = 0;
  registered_driver_count = 0;

  // Register built-in class drivers
  usb_hid_register_driver();
  usb_msc_register_driver();

  // Detect USB controllers
  if (!usb_detect_controllers()) {
    terminal_puts(&main_terminal, "No USB controllers found\r\n");
    return false;
  }

  terminal_printf(&main_terminal,
                  "USB initialization complete: %u controller(s) found\r\n",
                  usb_controller_count);

  usb_initialized = true;
  return true;
}

void usb_cleanup(void) {
  terminal_puts(&main_terminal, "Cleaning up USB subsystem...\r\n");

  // Cleanup all controllers
  for (uint8_t i = 0; i < usb_controller_count; i++) {
    usb_controller_t *ctrl = &usb_controllers[i];

    // Remove all devices
    for (uint8_t j = 0; j < ctrl->device_count; j++) {
      usb_remove_device(&ctrl->devices[j]);
    }

    // Controller-specific cleanup
    switch (ctrl->type) {
    case USB_TYPE_UHCI:
      uhci_cleanup(ctrl);
      break;
    case USB_TYPE_EHCI:
      ehci_cleanup(ctrl);
      break;
    }
  }

  usb_controller_count = 0;
}

usb_controller_t *usb_detect_controllers(void) {
  terminal_puts(&main_terminal, "Scanning for USB controllers...\r\n");

  // Scan all PCI devices
  for (uint32_t i = 0; i < pci_device_count; i++) {
    pci_device_t *pci_dev = &pci_devices[i];

    // Check if Serial Bus Controller (class 0x0C)
    if (pci_dev->class_code != PCI_CLASS_SERIAL_BUS) {
      continue;
    }

    // Check subclass for USB (0x03)
    if (pci_dev->subclass != 0x03) {
      continue;
    }

    if (usb_controller_count >= USB_MAX_CONTROLLERS) {
      terminal_puts(&main_terminal,
                    "USB: Maximum controller count reached\r\n");
      break;
    }

    usb_controller_t *ctrl = &usb_controllers[usb_controller_count];
    ctrl->pci_dev = pci_dev;
    ctrl->id = usb_controller_count;

    // Determine type by prog_if
    switch (pci_dev->prog_if) {
    case 0x00:
      ctrl->type = USB_TYPE_UHCI;
      terminal_printf(&main_terminal,
                      "Found UHCI controller at %02x:%02x.%x\r\n", pci_dev->bus,
                      pci_dev->device, pci_dev->function);
      if (uhci_init(ctrl)) {
        usb_controller_count++;
      }
      break;

    case 0x10:
      ctrl->type = USB_TYPE_OHCI;
      terminal_printf(&main_terminal,
                      "Found OHCI controller at %02x:%02x.%x\r\n", pci_dev->bus,
                      pci_dev->device, pci_dev->function);
      if (ohci_init(ctrl)) {
        usb_controller_count++;
      }
      break;

    case 0x20:
      ctrl->type = USB_TYPE_EHCI;
      terminal_printf(&main_terminal,
                      "Found EHCI controller at %02x:%02x.%x\r\n", pci_dev->bus,
                      pci_dev->device, pci_dev->function);
      if (ehci_init(ctrl)) {
        usb_controller_count++;
      }
      break;

    case 0x30:
      ctrl->type = USB_TYPE_XHCI;
      terminal_printf(
          &main_terminal,
          "Found xHCI controller at %02x:%02x.%x (not implemented)\r\n",
          pci_dev->bus, pci_dev->device, pci_dev->function);
      break;

    default:
      terminal_printf(
          &main_terminal,
          "Unknown USB controller type (prog_if=0x%02x) at %02x:%02x.%x\r\n",
          pci_dev->prog_if, pci_dev->bus, pci_dev->device, pci_dev->function);
      break;
    }
  }

  return usb_controller_count > 0 ? usb_controllers : NULL;
}

// ========================================================================
// DEVICE MANAGEMENT
// ========================================================================

usb_device_t *usb_allocate_device(usb_controller_t *controller) {
  for (uint8_t i = 0; i < USB_MAX_DEVICES; i++) {
    if (!controller->devices[i].connected) {
      memset(&controller->devices[i], 0, sizeof(usb_device_t));
      controller->devices[i].controller_id = controller->id;
      controller->device_count++;
      return &controller->devices[i];
    }
  }
  return NULL;
}

bool usb_enumerate_device(usb_controller_t *controller, uint8_t port) {
  terminal_printf(&main_terminal,
                  "USB: Enumerating device on controller %u, port %u...\r\n",
                  controller->id, port);

  usb_device_t *device = usb_allocate_device(controller);
  if (!device) {
    terminal_puts(&main_terminal,
                  "USB: Failed to allocate device structure\r\n");
    return false;
  }

  device->port = port;
  device->address = 0;
  device->connected = true;
  device->controller_id = controller->id;

  if (controller->type == USB_TYPE_EHCI) {
    device->speed = USB_SPEED_HIGH;
  } else {
    device->speed = USB_SPEED_FULL;
  }

  terminal_printf(&main_terminal, "USB: Device speed: %s\r\n",
                  device->speed == USB_SPEED_HIGH ? "High" : "Full/Low");

  for (volatile int i = 0; i < 1000000; i++)
    ;

  terminal_puts(&main_terminal, "USB: Getting device descriptor...\r\n");
  if (!usb_get_device_descriptor(device)) {
    terminal_puts(&main_terminal,
                  "USB: Failed to get initial device descriptor\r\n");
    usb_remove_device(device);
    return false;
  }

  terminal_printf(&main_terminal,
                  "USB: Initial descriptor: VID=%04x PID=%04x, Class=%02x, "
                  "Subclass=%02x, Protocol=%02x\r\n",
                  device->descriptor.idVendor, device->descriptor.idProduct,
                  device->descriptor.bDeviceClass,
                  device->descriptor.bDeviceSubClass,
                  device->descriptor.bDeviceProtocol);

  // CAMBIO CRÍTICO: Si es hub, enumerarlo pero marcar especialmente
  if (device->descriptor.bDeviceClass == USB_CLASS_HUB) {
    terminal_puts(&main_terminal,
                  "USB: Hub detected - configuring as pass-through\r\n");

    // Asignar dirección al hub
    uint8_t new_address = controller->device_count + 1;
    if (!usb_set_address(device, new_address)) {
      terminal_puts(&main_terminal, "USB: Failed to set hub address\r\n");
      usb_remove_device(device);
      return false;
    }
    device->address = new_address;

    for (volatile int i = 0; i < 1000000; i++)
      ;

    // Get config y configurar el hub
    uint8_t config_buffer[256];
    if (usb_get_config_descriptor(device, 0, config_buffer,
                                  sizeof(config_buffer))) {
      usb_config_descriptor_t *config =
          (usb_config_descriptor_t *)config_buffer;
      usb_set_configuration(device, config->bConfigurationValue);
      device->config_value = config->bConfigurationValue;
    }

    // IMPORTANTE: Intentar detectar puertos del hub y escanearlos
    terminal_puts(&main_terminal,
                  "USB: Hub configured - attempting to scan hub ports\r\n");

    // Request hub descriptor para saber cuántos puertos tiene
    usb_setup_packet_t hub_desc_req = {
        .bmRequestType = 0xA0, // Class, IN, Device
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (0x29 << 8), // Hub descriptor type
        .wIndex = 0,
        .wLength = 9};

    uint8_t hub_desc[9];
    if (usb_control_transfer(device, &hub_desc_req, hub_desc, 9)) {
      uint8_t num_ports = hub_desc[2];
      terminal_printf(&main_terminal, "USB: Hub has %u ports\r\n", num_ports);

      // Power on todos los puertos del hub
      for (uint8_t hub_port = 1; hub_port <= num_ports; hub_port++) {
        usb_setup_packet_t power_req = {.bmRequestType =
                                            0x23, // Class, OUT, Other (port)
                                        .bRequest = 0x03, // SET_FEATURE
                                        .wValue = 8,      // PORT_POWER
                                        .wIndex = hub_port,
                                        .wLength = 0};
        usb_control_transfer(device, &power_req, NULL, 0);
      }

      // Esperar a que se enciendan
      for (volatile int i = 0; i < 2000000; i++)
        ;

      // Escanear cada puerto del hub
      for (uint8_t hub_port = 1; hub_port <= num_ports; hub_port++) {
        usb_setup_packet_t status_req = {.bmRequestType =
                                             0xA3, // Class, IN, Other (port)
                                         .bRequest = 0x00, // GET_STATUS
                                         .wValue = 0,
                                         .wIndex = hub_port,
                                         .wLength = 4};

        uint32_t port_status = 0;
        if (usb_control_transfer(device, &status_req, &port_status, 4)) {
          terminal_printf(&main_terminal,
                          "USB: Hub port %u status = 0x%08x\r\n", hub_port,
                          port_status);

          // Bit 0 = CCS (Current Connect Status)
          if (port_status & 0x01) {
            terminal_printf(&main_terminal,
                            "USB: Device detected on hub port %u\r\n",
                            hub_port);

            // Reset el puerto del hub
            usb_setup_packet_t reset_req = {.bmRequestType = 0x23,
                                            .bRequest = 0x03, // SET_FEATURE
                                            .wValue = 4,      // PORT_RESET
                                            .wIndex = hub_port,
                                            .wLength = 0};

            if (usb_control_transfer(device, &reset_req, NULL, 0)) {
              terminal_printf(&main_terminal,
                              "USB: Hub port %u reset initiated\r\n", hub_port);

              // Esperar reset (100ms)
              for (volatile int i = 0; i < 10000000; i++)
                ;

              // Clear PORT_RESET feature
              usb_setup_packet_t clear_reset = {.bmRequestType = 0x23,
                                                .bRequest =
                                                    0x01,    // CLEAR_FEATURE
                                                .wValue = 4, // PORT_RESET
                                                .wIndex = hub_port,
                                                .wLength = 0};
              usb_control_transfer(device, &clear_reset, NULL, 0);

              for (volatile int i = 0; i < 1000000; i++)
                ;

              // Verificar si el puerto está habilitado
              if (usb_control_transfer(device, &status_req, &port_status, 4)) {
                terminal_printf(
                    &main_terminal,
                    "USB: Hub port %u status after reset = 0x%08x\r\n",
                    hub_port, port_status);

                // Bit 1 = PES (Port Enable Status)
                if (port_status & 0x02) {
                  terminal_printf(
                      &main_terminal,
                      "USB: Hub port %u enabled, enumerating device...\r\n",
                      hub_port);
                  // RECURSIÓN: enumerar dispositivo conectado al hub
                  // Nota: esto es simplificado, no pasamos hub_port
                  // correctamente
                  usb_enumerate_device(controller, port);
                }
              }
            }
          }
        }
      }
    }

    // Marcar hub como configurado pero no intentar drivers
    return true;
  }

  // Resto del código normal para dispositivos no-hub...
  uint8_t new_address = controller->device_count + 1;
  terminal_printf(&main_terminal, "USB: Setting address to %u...\r\n",
                  new_address);

  if (!usb_set_address(device, new_address)) {
    terminal_puts(&main_terminal, "USB: Failed to set device address\r\n");
    usb_remove_device(device);
    return false;
  }

  device->address = new_address;
  for (volatile int i = 0; i < 1000000; i++)
    ;

  if (!usb_get_device_descriptor(device)) {
    terminal_puts(&main_terminal,
                  "USB: Failed to get device descriptor after address set\r\n");
    usb_remove_device(device);
    return false;
  }

  uint8_t config_buffer[512];
  if (!usb_get_config_descriptor(device, 0, config_buffer,
                                 sizeof(config_buffer))) {
    terminal_puts(&main_terminal,
                  "USB: Failed to get configuration descriptor\r\n");
    usb_remove_device(device);
    return false;
  }

  usb_config_descriptor_t *config = (usb_config_descriptor_t *)config_buffer;

  // Parse interfaces para obtener clase real
  uint8_t *ptr = config_buffer + sizeof(usb_config_descriptor_t);
  uint8_t *end = config_buffer + config->wTotalLength;

  while (ptr < end - 2) {
    uint8_t length = ptr[0];
    uint8_t type = ptr[1];

    if (length == 0 || ptr + length > end)
      break;

    if (type == USB_DESC_INTERFACE) {
      usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)ptr;
      device->class_code = iface->bInterfaceClass;
      device->subclass = iface->bInterfaceSubClass;
      device->protocol = iface->bInterfaceProtocol;
      break;
    }

    ptr += length;
  }

  if (!usb_set_configuration(device, config->bConfigurationValue)) {
    terminal_puts(&main_terminal, "USB: Failed to set configuration\r\n");
    usb_remove_device(device);
    return false;
  }

  device->config_value = config->bConfigurationValue;
  memset(device->ep_toggles, 0, sizeof(device->ep_toggles));

  terminal_printf(&main_terminal,
                  "USB: Device enumerated: VID=%04x PID=%04x Class=%02x\r\n",
                  device->descriptor.idVendor, device->descriptor.idProduct,
                  device->class_code);

  usb_scan_for_drivers(device);

  return true;
}

void usb_remove_device(usb_device_t *device) {
  if (!device || !device->connected) {
    return;
  }

  // Call driver cleanup if present
  if (device->driver_data) {
    // Driver should have registered cleanup callback
    // For now, just free the data
    device->driver_data = NULL;
  }

  device->connected = false;

  usb_controller_t *ctrl = &usb_controllers[device->controller_id];
  if (ctrl->device_count > 0) {
    ctrl->device_count--;
  }
}

// ========================================================================
// DESCRIPTOR FUNCTIONS
// ========================================================================

bool usb_get_device_descriptor(usb_device_t *device) {
  // Estrategia más robusta: pedir 18 bytes directamente en una sola
  // transferencia Si el dispositivo no puede manejar 18 bytes inicialmente,
  // caerá a 8

  usb_setup_packet_t setup = {
      .bmRequestType = 0x80,
      .bRequest = USB_REQ_GET_DESCRIPTOR,
      .wValue = (USB_DESC_DEVICE << 8) | 0,
      .wIndex = 0,
      .wLength = 18 // Pedir descriptor completo
  };

  // Para dispositivos con address 0 (enumeración inicial), usar buffer temporal
  if (device->address == 0) {
    uint8_t temp_desc[18];

    // Primera vez: pedir 8 bytes para obtener bMaxPacketSize0
    setup.wLength = 8;
    if (!usb_control_transfer(device, &setup, temp_desc, 8)) {
      terminal_puts(&main_terminal, "USB: Failed to get initial 8 bytes\n");
      return false;
    }

    // Copiar solo los primeros 8 bytes
    memcpy(&device->descriptor, temp_desc, 8);

    // Validar bMaxPacketSize0
    uint8_t max_packet = device->descriptor.bMaxPacketSize0;
    if ((device->speed == USB_SPEED_HIGH && max_packet != 64) ||
        (device->speed != USB_SPEED_HIGH && max_packet != 8 &&
         max_packet != 16 && max_packet != 32 && max_packet != 64)) {
      terminal_printf(&main_terminal,
                      "USB: Invalid bMaxPacketSize0=%u for speed=%u\n",
                      max_packet, device->speed);
      return false;
    }

    terminal_printf(&main_terminal, "USB: bMaxPacketSize0=%u detected\n",
                    max_packet);

    // CRÍTICO: Resetear endpoint 0 toggle antes de segunda transferencia
    device->ep_toggles[0] = 0;  // OUT EP0
    device->ep_toggles[16] = 0; // IN EP0

    // Esperar un poco para que el dispositivo se estabilice
    for (volatile int i = 0; i < 100000; i++)
      ;

    // Segunda transferencia: descriptor completo
    setup.wLength = 18;
    if (!usb_control_transfer(device, &setup, temp_desc, 18)) {
      terminal_puts(&main_terminal, "USB: Failed to get full descriptor\n");
      return false;
    }

    memcpy(&device->descriptor, temp_desc, 18);

  } else {
    // Dispositivo ya tiene dirección asignada, pedir directamente 18 bytes
    if (!usb_control_transfer(device, &setup, &device->descriptor, 18)) {
      terminal_puts(&main_terminal, "USB: Failed to get device descriptor\n");
      return false;
    }
  }

  terminal_printf(&main_terminal, "USB: VID=%04x PID=%04x Class=%02x\n",
                  device->descriptor.idVendor, device->descriptor.idProduct,
                  device->descriptor.bDeviceClass);

  return true;
}

bool usb_get_config_descriptor(usb_device_t *device, uint8_t config_index,
                               void *buffer, uint16_t length) {
  usb_setup_packet_t setup = {.bmRequestType = 0x80,
                              .bRequest = USB_REQ_GET_DESCRIPTOR,
                              .wValue =
                                  (USB_DESC_CONFIGURATION << 8) | config_index,
                              .wIndex = 0,
                              .wLength = length};

  return usb_control_transfer(device, &setup, buffer, length);
}

bool usb_set_configuration(usb_device_t *device, uint8_t config_value) {
  usb_setup_packet_t setup = {.bmRequestType = 0x00, // Host to device
                              .bRequest = USB_REQ_SET_CONFIGURATION,
                              .wValue = config_value,
                              .wIndex = 0,
                              .wLength = 0};

  return usb_control_transfer(device, &setup, NULL, 0);
}

bool usb_set_address(usb_device_t *device, uint8_t address) {
  usb_setup_packet_t setup = {.bmRequestType = 0x00,
                              .bRequest = USB_REQ_SET_ADDRESS,
                              .wValue = address,
                              .wIndex = 0,
                              .wLength = 0};

  bool result = usb_control_transfer(device, &setup, NULL, 0);

  // Wait for device to accept new address
  for (volatile int i = 0; i < 100000; i++)
    ;

  return result;
}

// ========================================================================
// DRIVER REGISTRATION
// ========================================================================

bool usb_register_driver(usb_driver_t *driver) {
  if (registered_driver_count >= 16) {
    return false;
  }

  registered_drivers[registered_driver_count++] = driver;
  terminal_printf(&main_terminal, "Registered USB driver: %s\r\n",
                  driver->name);

  return true;
}

void usb_scan_for_drivers(usb_device_t *device) {
  for (uint8_t i = 0; i < registered_driver_count; i++) {
    usb_driver_t *driver = registered_drivers[i];

    // Check if driver matches device class
    if (driver->class_code == device->class_code &&
        (driver->subclass == 0xFF || driver->subclass == device->subclass) &&
        (driver->protocol == 0xFF || driver->protocol == device->protocol)) {

      terminal_printf(&main_terminal, "Found matching driver: %s\r\n",
                      driver->name);

      if (driver->probe && driver->probe(device)) {
        if (driver->init && driver->init(device)) {
          terminal_printf(&main_terminal,
                          "Driver %s initialized successfully\r\n",
                          driver->name);
          return;
        }
      }
    }
  }

  terminal_puts(&main_terminal, "No driver found for device\r\n");
}

// ========================================================================
// UTILITY FUNCTIONS
// ========================================================================

void usb_list_devices(void) {
  terminal_puts(&main_terminal, "\r\n=== USB Devices ===\r\n");

  uint32_t total_devices = 0;

  for (uint8_t i = 0; i < usb_controller_count; i++) {
    usb_controller_t *ctrl = &usb_controllers[i];

    terminal_printf(&main_terminal, "Controller %u (%s):\r\n", i,
                    usb_get_speed_name(ctrl->type));

    for (uint8_t j = 0; j < USB_MAX_DEVICES; j++) {
      usb_device_t *dev = &ctrl->devices[j];

      if (!dev->connected)
        continue;

      terminal_printf(&main_terminal, "  Port %u: %04x:%04x %s\r\n", dev->port,
                      dev->descriptor.idVendor, dev->descriptor.idProduct,
                      usb_get_class_name(dev->class_code));

      total_devices++;
    }
  }

  if (total_devices == 0) {
    terminal_puts(&main_terminal, "No USB devices connected\r\n");
  }

  terminal_puts(&main_terminal, "\r\n");
}

bool usb_clear_endpoint_halt(usb_device_t *device, uint8_t endpoint) {
  usb_setup_packet_t setup = {
      .bmRequestType = 0x02, // Host-to-device, standard, endpoint recipient
      .bRequest = USB_REQ_CLEAR_FEATURE,
      .wValue = 0,        // Feature: ENDPOINT_HALT
      .wIndex = endpoint, // Endpoint address (with direction bit)
      .wLength = 0};

  bool result = usb_control_transfer(device, &setup, NULL, 0);

  if (result) {
    // CRÍTICO: Clear HALT resetea el data toggle a 0
    uint8_t ep_num = endpoint & 0x0F;
    bool is_in = (endpoint & 0x80) != 0;
    uint8_t toggle_idx = ep_num + (is_in ? 16 : 0);
    device->ep_toggles[toggle_idx] = 0;

    terminal_printf(&main_terminal,
                    "USB: Endpoint 0x%02x halt cleared, toggle reset to 0\n",
                    endpoint);
  }

  return result;
}

const char *usb_get_class_name(uint8_t class_code) {
  switch (class_code) {
  case 0x00:
    return "Device";
  case 0x01:
    return "Audio";
  case 0x02:
    return "Communications";
  case 0x03:
    return "HID";
  case 0x05:
    return "Physical";
  case 0x06:
    return "Image";
  case 0x07:
    return "Printer";
  case USB_CLASS_MASS_STORAGE:
    return "Mass Storage";
  case USB_CLASS_HUB:
    return "Hub";
  case 0x0A:
    return "CDC-Data";
  case 0x0B:
    return "Smart Card";
  case 0x0D:
    return "Content Security";
  case 0x0E:
    return "Video";
  case 0x0F:
    return "Personal Healthcare";
  case 0xDC:
    return "Diagnostic";
  case 0xE0:
    return "Wireless";
  case 0xEF:
    return "Miscellaneous";
  case 0xFE:
    return "Application Specific";
  case 0xFF:
    return "Vendor Specific";
  default:
    return "Unknown";
  }
}

const char *usb_get_speed_name(uint8_t speed) {
  switch (speed) {
  case USB_TYPE_UHCI:
    return "USB 1.1 (UHCI)";
  case USB_TYPE_OHCI:
    return "USB 1.1 (OHCI)";
  case USB_TYPE_EHCI:
    return "USB 2.0 (EHCI)";
  case USB_TYPE_XHCI:
    return "USB 3.0 (xHCI)";
  default:
    return "Unknown";
  }
}

// ========================================================================
// DRIVER SYSTEM INTEGRATION
// ========================================================================
#include "driver_system.h"

static int usb_subsystem_driver_init(driver_instance_t *drv, void *config) {
  (void)config;
  if (!drv)
    return -1;
  if (!usb_init())
    return -1;
  return 0;
}

static int usb_subsystem_driver_start(driver_instance_t *drv) {
  if (!drv)
    return -1;
  terminal_printf(&main_terminal, "USB driver: Subsystem started.\r\n");
  return 0;
}

static int usb_subsystem_driver_stop(driver_instance_t *drv) {
  if (!drv)
    return -1;
  return 0;
}

static int usb_subsystem_driver_cleanup(driver_instance_t *drv) {
  if (!drv)
    return -1;
  usb_cleanup();
  return 0;
}

static int usb_subsystem_driver_ioctl(driver_instance_t *drv, uint32_t cmd,
                                      void *arg) {
  if (!drv)
    return -1;

  switch (cmd) {
  case 0x5001: // List devices
    usb_list_devices();
    return 0;
  default:
    return -1;
  }
}

static driver_ops_t usb_subsystem_driver_ops = {
    .init = usb_subsystem_driver_init,
    .start = usb_subsystem_driver_start,
    .stop = usb_subsystem_driver_stop,
    .cleanup = usb_subsystem_driver_cleanup,
    .ioctl = usb_subsystem_driver_ioctl,
    .load_data = NULL};

static driver_type_info_t usb_subsystem_driver_type = {
    .type = DRIVER_TYPE_USB,
    .type_name = "usb_subsystem",
    .version = "1.0.0",
    .priv_data_size = 0,
    .default_ops = &usb_subsystem_driver_ops,
    .validate_data = NULL,
    .print_info = NULL};

int usb_driver_register_type(void) {
  return driver_register_type(&usb_subsystem_driver_type);
}

driver_instance_t *usb_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_USB, name);
}