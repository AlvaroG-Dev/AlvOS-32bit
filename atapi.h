#ifndef ATAPI_H
#define ATAPI_H

#include <stdbool.h>
#include <stdint.h>


// ATAPI Command Codes
#define ATAPI_CMD_TEST_UNIT_READY 0x00
#define ATAPI_CMD_REQUEST_SENSE 0x03
#define ATAPI_CMD_READ_10 0x28
#define ATAPI_CMD_READ_12 0xA8
#define ATAPI_CMD_READ_CAPACITY 0x25
#define ATAPI_CMD_READ_TOC 0x43
#define ATAPI_CMD_GET_CONFIGURATION 0x46
#define ATAPI_CMD_START_STOP_UNIT 0x1B
#define ATAPI_CMD_PREVENT_ALLOW 0x1E
#define ATAPI_CMD_INQUIRY 0x12

// ATAPI Packet sizes
#define ATAPI_PACKET_SIZE 12
#define ATAPI_SECTOR_SIZE 2048 // CD/DVD sector size

// ATAPI Device types
#define ATAPI_TYPE_CDROM 0x05
#define ATAPI_TYPE_TAPE 0x01
#define ATAPI_TYPE_DIRECT_ACCESS 0x00

// Maximum ATAPI devices
#define MAX_ATAPI_DEVICES 4

// IDE/ATA ports
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
// ATAPI Error codes
typedef enum {
  ATAPI_ERR_NONE = 0,
  ATAPI_ERR_INVALID_PARAM,
  ATAPI_ERR_NOT_INITIALIZED,
  ATAPI_ERR_NO_MEDIA,
  ATAPI_ERR_TIMEOUT,
  ATAPI_ERR_IO_ERROR,
  ATAPI_ERR_DEVICE_NOT_READY,
  ATAPI_ERR_LBA_OUT_OF_RANGE,
  ATAPI_ERR_NOT_SUPPORTED
} atapi_err_t;

// ATAPI Sense Key codes
typedef enum {
  ATAPI_SENSE_NO_SENSE = 0x0,
  ATAPI_SENSE_RECOVERED_ERROR = 0x1,
  ATAPI_SENSE_NOT_READY = 0x2,
  ATAPI_SENSE_MEDIUM_ERROR = 0x3,
  ATAPI_SENSE_HARDWARE_ERROR = 0x4,
  ATAPI_SENSE_ILLEGAL_REQUEST = 0x5,
  ATAPI_SENSE_UNIT_ATTENTION = 0x6,
  ATAPI_SENSE_DATA_PROTECT = 0x7,
  ATAPI_SENSE_ABORTED_COMMAND = 0xB
} atapi_sense_key_t;

// ATAPI Device structure
typedef struct {
  uint8_t device_id;  // Device ID (0-3)
  uint8_t bus;        // IDE bus (0 or 1)
  uint8_t drive;      // Drive select (0=master, 1=slave)
  bool present;       // Device present
  bool initialized;   // Device initialized
  bool media_present; // Media inserted

  // Device information
  uint8_t device_type; // ATAPI device type
  char model[41];      // Device model
  char serial[21];     // Device serial number
  char firmware[9];    // Firmware version

  // Media information
  uint32_t sector_count; // Total sectors on media
  uint32_t sector_size;  // Sector size (usually 2048)
  bool media_changed;    // Media change flag

  // I/O ports
  uint16_t io_base; // Base I/O port
  uint16_t io_ctrl; // Control port

  // Statistics
  uint64_t read_count;  // Read operations
  uint64_t error_count; // Error count
} atapi_device_t;

// ATAPI Sense data structure
typedef struct {
  uint8_t error_code;
  uint8_t sense_key;
  uint32_t information;
  uint8_t asc;  // Additional Sense Code
  uint8_t ascq; // Additional Sense Code Qualifier
} atapi_sense_data_t;

// Function prototypes

// Initialization and detection
bool atapi_init(void);
void atapi_cleanup(void);
bool atapi_detect_device(uint8_t bus, uint8_t drive);
bool atapi_identify_device(atapi_device_t *device);

// Device operations
atapi_err_t atapi_read_sectors(uint32_t device_id, uint32_t lba, uint32_t count,
                               void *buffer);
atapi_err_t atapi_test_unit_ready(uint32_t device_id);
atapi_err_t atapi_read_capacity(uint32_t device_id, uint32_t *sector_count,
                                uint32_t *sector_size);
atapi_err_t atapi_request_sense(uint32_t device_id, atapi_sense_data_t *sense);

// Media control
atapi_err_t atapi_eject(uint32_t device_id);
atapi_err_t atapi_load(uint32_t device_id);
bool atapi_check_media(uint32_t device_id);

// Information and utilities
uint32_t atapi_get_device_count(void);
atapi_device_t *atapi_get_device_info(uint32_t device_id);
void atapi_list_devices(void);
const char *atapi_get_error_string(atapi_err_t error);
const char *atapi_get_sense_key_string(atapi_sense_key_t sense_key);
bool atapi_verify_device_signature(uint8_t bus, uint8_t drive);
// Low-level packet interface
atapi_err_t atapi_send_packet(atapi_device_t *device, uint8_t *packet,
                              void *buffer, uint32_t buffer_size, bool read);

// Driver registration functions
int atapi_driver_register_type(void);
struct driver_instance *atapi_driver_create(const char *name);

#endif // ATAPI_H