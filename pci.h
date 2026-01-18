#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stdint.h>

// Puertos PCI Configuration Space
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// Offsets en el Configuration Space
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_REVISION_ID 0x08
#define PCI_PROG_IF 0x09
#define PCI_SUBCLASS 0x0A
#define PCI_CLASS_CODE 0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER 0x0D
#define PCI_HEADER_TYPE 0x0E
#define PCI_BIST 0x0F
#define PCI_BAR0 0x10
#define PCI_BAR1 0x14
#define PCI_BAR2 0x18
#define PCI_BAR3 0x1C
#define PCI_BAR4 0x20
#define PCI_BAR5 0x24
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN 0x3D

// Bits del comando PCI
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_COMMAND_SPECIAL 0x0008
#define PCI_COMMAND_INVALIDATE 0x0010
#define PCI_COMMAND_VGA_PALETTE 0x0020
#define PCI_COMMAND_PARITY 0x0040
#define PCI_COMMAND_WAIT 0x0080
#define PCI_COMMAND_SERR 0x0100
#define PCI_COMMAND_FAST_BACK 0x0200
#define PCI_COMMAND_INTX_DISABLE 0x0400

// Tipos de BAR
#define PCI_BAR_TYPE_MEMORY 0x00
#define PCI_BAR_TYPE_IO 0x01
#define PCI_BAR_MEMORY_32BIT 0x00
#define PCI_BAR_MEMORY_64BIT 0x04
#define PCI_BAR_PREFETCHABLE 0x08

// Clases de dispositivos PCI comunes
#define PCI_CLASS_UNCLASSIFIED 0x00
#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_NETWORK 0x02
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY 0x05
#define PCI_CLASS_BRIDGE 0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_SYSTEM 0x08
#define PCI_CLASS_INPUT 0x09
#define PCI_CLASS_DOCKING 0x0A
#define PCI_CLASS_PROCESSOR 0x0B
#define PCI_CLASS_SERIAL_BUS 0x0C

// Máximo número de dispositivos PCI a enumerar
#define MAX_PCI_DEVICES 256

// Estructura para información de BAR
typedef struct {
  uint64_t address;
  uint32_t size;
  uint8_t type; // 0 = memory, 1 = I/O
  bool is_64bit;
  bool is_prefetchable;
  bool is_valid;
} pci_bar_t;

// Estructura de dispositivo PCI
typedef struct {
  uint8_t bus;
  uint8_t device;
  uint8_t function;

  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t revision_id;

  uint8_t header_type;
  uint8_t interrupt_line;
  uint8_t interrupt_pin;

  pci_bar_t bars[6];

  bool present;
} pci_device_t;

// Tabla global de dispositivos PCI
extern pci_device_t pci_devices[MAX_PCI_DEVICES];
extern uint32_t pci_device_count;

// Funciones principales
void pci_init(void);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t offset);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t offset);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint32_t data);
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint16_t data);
void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint8_t data);

// Funciones de enumeración
void pci_scan_all_buses(void);
void pci_scan_bus(uint8_t bus);
void pci_scan_device(uint8_t bus, uint8_t device);
void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function);

// Funciones de utilidad
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t *pci_find_device_by_class(uint8_t class_code, uint8_t subclass);
void pci_enable_bus_mastering(pci_device_t *device);
void pci_enable_memory_space(pci_device_t *device);
void pci_enable_io_space(pci_device_t *device);
uint32_t pci_get_bar_size(uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t bar_num);
void pci_read_bars(pci_device_t *device);

// Funciones de información
void pci_list_devices(void);
const char *pci_get_class_name(uint8_t class_code);
const char *pci_get_vendor_name(uint16_t vendor_id);

// Driver registration functions
int pci_driver_register_type(void);
struct driver_instance *pci_driver_create(const char *name);

#endif // PCI_H