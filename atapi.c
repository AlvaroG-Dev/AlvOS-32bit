#include "atapi.h"
#include "io.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"
#include "memory.h"
#include "irq.h"

// ATA register offsets
#define ATA_REG_DATA            0
#define ATA_REG_ERROR           1
#define ATA_REG_FEATURES        1
#define ATA_REG_SECTOR_COUNT    2
#define ATA_REG_LBA_LOW         3
#define ATA_REG_LBA_MID         4
#define ATA_REG_LBA_HIGH        5
#define ATA_REG_DRIVE_SELECT    6
#define ATA_REG_STATUS          7
#define ATA_REG_COMMAND         7

// ATA commands
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0

// ATA status bits
#define ATA_STATUS_ERR          0x01
#define ATA_STATUS_DRQ          0x08
#define ATA_STATUS_SRV          0x10
#define ATA_STATUS_DF           0x20
#define ATA_STATUS_RDY          0x40
#define ATA_STATUS_BSY          0x80

// Timeout values
#define ATAPI_TIMEOUT_MS        5000
#define ATAPI_SPIN_TIMEOUT      1000000

// Global ATAPI devices
static atapi_device_t atapi_devices[MAX_ATAPI_DEVICES];
static uint32_t atapi_device_count = 0;
static bool atapi_initialized = false;

// Forward declarations
static bool atapi_wait_ready(atapi_device_t* device);
static bool atapi_wait_drq(atapi_device_t* device);
static bool atapi_wait_not_busy(atapi_device_t* device);
static void atapi_select_drive(atapi_device_t* device);
static void atapi_400ns_delay(atapi_device_t* device);
static uint8_t atapi_read_status(atapi_device_t* device);

// ========================================================================
// INITIALIZATION
// ========================================================================

bool atapi_init(void) {
    if (atapi_initialized) {
        return true;
    }
    
    terminal_puts(&main_terminal, "Initializing ATAPI subsystem...\r\n");
    
    memset(atapi_devices, 0, sizeof(atapi_devices));
    atapi_device_count = 0;
    
    // Check primary bus
    terminal_puts(&main_terminal, "ATAPI: Scanning primary IDE bus...\r\n");
    if (atapi_detect_device(0, 0)) {
        terminal_puts(&main_terminal, "ATAPI: Found device on primary master\r\n");
    }
    if (atapi_detect_device(0, 1)) {
        terminal_puts(&main_terminal, "ATAPI: Found device on primary slave\r\n");
    }
    
    // Check secondary bus
    terminal_puts(&main_terminal, "ATAPI: Scanning secondary IDE bus...\r\n");
    if (atapi_detect_device(1, 0)) {
        terminal_puts(&main_terminal, "ATAPI: Found device on secondary master\r\n");
    }
    if (atapi_detect_device(1, 1)) {
        terminal_puts(&main_terminal, "ATAPI: Found device on secondary slave\r\n");
    }
    
    atapi_initialized = true;
    terminal_printf(&main_terminal, "ATAPI: Initialized %u device(s)\r\n", atapi_device_count);
    
    return true;
}

void atapi_cleanup(void) {
    if (!atapi_initialized) {
        return;
    }
    
    terminal_puts(&main_terminal, "Cleaning up ATAPI subsystem...\r\n");
    
    // Eject all media
    for (uint32_t i = 0; i < atapi_device_count; i++) {
        if (atapi_devices[i].present && atapi_devices[i].media_present) {
            atapi_eject(i);
        }
    }
    
    memset(atapi_devices, 0, sizeof(atapi_devices));
    atapi_device_count = 0;
    atapi_initialized = false;
    
    terminal_puts(&main_terminal, "ATAPI: Cleanup complete\r\n");
}

bool atapi_detect_device(uint8_t bus, uint8_t drive) {
    if (atapi_device_count >= MAX_ATAPI_DEVICES) {
        return false;
    }
    
    atapi_device_t* device = &atapi_devices[atapi_device_count];
    memset(device, 0, sizeof(atapi_device_t));
    
    device->device_id = atapi_device_count;
    device->bus = bus;
    device->drive = drive;
    device->io_base = (bus == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    device->io_ctrl = (bus == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    
    // Select drive
    atapi_select_drive(device);
    atapi_400ns_delay(device);
    
    // NUEVO: Primero hacer DEVICE RESET para obtener signature
    outb(device->io_base + ATA_REG_COMMAND, 0x08);  // DEVICE RESET
    
    // Esperar un momento
    for (volatile int i = 0; i < 100000; i++);
    
    // Leer signature bytes
    uint8_t lba_mid = inb(device->io_base + ATA_REG_LBA_MID);
    uint8_t lba_high = inb(device->io_base + ATA_REG_LBA_HIGH);
    
    terminal_printf(&main_terminal, "ATAPI: Checking device on bus %u, drive %u: sig=0x%02x%02x\r\n",
                   bus, drive, lba_mid, lba_high);
    
    // VERIFICAR: Solo continuar si es realmente ATAPI
    if (lba_mid != 0x14 || lba_high != 0xEB) {
        terminal_printf(&main_terminal, "ATAPI: Not an ATAPI device (expected 0x14EB, got 0x%02x%02x)\r\n",
                       lba_mid, lba_high);
        return false;
    }
    
    terminal_puts(&main_terminal, "ATAPI: Valid ATAPI signature detected\r\n");
    
    // Ahora sí, enviar IDENTIFY PACKET DEVICE
    outb(device->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    atapi_400ns_delay(device);
    
    // Check if device exists
    uint8_t status = inb(device->io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        terminal_puts(&main_terminal, "ATAPI: Device not responding\r\n");
        return false;
    }
    
    // Wait for BSY to clear
    if (!atapi_wait_not_busy(device)) {
        terminal_puts(&main_terminal, "ATAPI: Timeout waiting for ready\r\n");
        return false;
    }
    
    // Check for error
    status = atapi_read_status(device);
    if (status & ATA_STATUS_ERR) {
        terminal_puts(&main_terminal, "ATAPI: Error during identification\r\n");
        return false;
    }
    
    // Wait for DRQ
    if (!atapi_wait_drq(device)) {
        terminal_puts(&main_terminal, "ATAPI: Timeout waiting for data\r\n");
        return false;
    }
    
    // Read identification data
    if (!atapi_identify_device(device)) {
        terminal_puts(&main_terminal, "ATAPI: Failed to read identify data\r\n");
        return false;
    }
    
    // NUEVA VERIFICACIÓN: Confirmar que es un CD/DVD
    if (device->device_type != ATAPI_TYPE_CDROM) {
        terminal_printf(&main_terminal, "ATAPI: Device type 0x%02x is not CDROM, rejecting\r\n",
                       device->device_type);
        return false;
    }
    
    device->present = true;
    device->initialized = true;
    atapi_device_count++;
    
    terminal_printf(&main_terminal, "ATAPI: Successfully initialized device: %s\r\n", device->model);
    
    return true;
}

bool atapi_identify_device(atapi_device_t* device) {
    uint16_t identify_data[256];
    
    // Read 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(device->io_base + ATA_REG_DATA);
    }
    
    // Extract device type (word 0)
    uint16_t config = identify_data[0];
    device->device_type = (config >> 8) & 0x1F;
    
    // Extract model string (words 27-46, byte-swapped)
    for (int i = 0; i < 20; i++) {
        uint16_t word = identify_data[27 + i];
        device->model[i * 2] = (word >> 8) & 0xFF;
        device->model[i * 2 + 1] = word & 0xFF;
    }
    device->model[40] = '\0';
    
    // Trim trailing spaces
    for (int i = 39; i >= 0 && (device->model[i] == ' ' || device->model[i] == '\0'); i--) {
        device->model[i] = '\0';
    }
    
    // Extract serial number (words 10-19, byte-swapped)
    for (int i = 0; i < 10; i++) {
        uint16_t word = identify_data[10 + i];
        device->serial[i * 2] = (word >> 8) & 0xFF;
        device->serial[i * 2 + 1] = word & 0xFF;
    }
    device->serial[20] = '\0';
    
    // Trim trailing spaces
    for (int i = 19; i >= 0 && (device->serial[i] == ' ' || device->serial[i] == '\0'); i--) {
        device->serial[i] = '\0';
    }
    
    // Extract firmware revision (words 23-26, byte-swapped)
    for (int i = 0; i < 4; i++) {
        uint16_t word = identify_data[23 + i];
        device->firmware[i * 2] = (word >> 8) & 0xFF;
        device->firmware[i * 2 + 1] = word & 0xFF;
    }
    device->firmware[8] = '\0';
    
    // Trim trailing spaces
    for (int i = 7; i >= 0 && (device->firmware[i] == ' ' || device->firmware[i] == '\0'); i--) {
        device->firmware[i] = '\0';
    }
    
    // Set default sector size
    device->sector_size = ATAPI_SECTOR_SIZE;
    
    return true;
}

// ========================================================================
// PACKET INTERFACE
// ========================================================================

atapi_err_t atapi_send_packet(atapi_device_t* device, uint8_t* packet,
                              void* buffer, uint32_t buffer_size, bool read) {
    if (!device || !packet) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    // Select drive
    atapi_select_drive(device);
    atapi_400ns_delay(device);
    
    // Wait for device ready
    if (!atapi_wait_ready(device)) {
        return ATAPI_ERR_TIMEOUT;
    }
    
    // Set up PIO transfer
    outb(device->io_base + ATA_REG_FEATURES, 0);  // No DMA, no overlap
    outb(device->io_base + ATA_REG_LBA_MID, buffer_size & 0xFF);
    outb(device->io_base + ATA_REG_LBA_HIGH, (buffer_size >> 8) & 0xFF);
    
    // Send PACKET command
    outb(device->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);
    atapi_400ns_delay(device);
    
    // Wait for device to request packet
    if (!atapi_wait_drq(device)) {
        return ATAPI_ERR_TIMEOUT;
    }
    
    // Send packet (12 bytes = 6 words)
    uint16_t* packet_words = (uint16_t*)packet;
    for (int i = 0; i < 6; i++) {
        outw(device->io_base + ATA_REG_DATA, packet_words[i]);
    }
    
    // If no data transfer expected, we're done
    if (!buffer || buffer_size == 0) {
        atapi_wait_not_busy(device);
        return ATAPI_ERR_NONE;
    }
    
    // Handle data transfer
    if (read) {
        // Read data
        uint16_t* buf16 = (uint16_t*)buffer;
        uint32_t words_to_read = buffer_size / 2;
        
        if (!atapi_wait_drq(device)) {
            return ATAPI_ERR_TIMEOUT;
        }
        
        for (uint32_t i = 0; i < words_to_read; i++) {
            buf16[i] = inw(device->io_base + ATA_REG_DATA);
        }
    } else {
        // ATAPI is typically read-only, but handle write if needed
        uint16_t* buf16 = (uint16_t*)buffer;
        uint32_t words_to_write = buffer_size / 2;
        
        if (!atapi_wait_drq(device)) {
            return ATAPI_ERR_TIMEOUT;
        }
        
        for (uint32_t i = 0; i < words_to_write; i++) {
            outw(device->io_base + ATA_REG_DATA, buf16[i]);
        }
    }
    
    // Wait for command completion
    if (!atapi_wait_not_busy(device)) {
        return ATAPI_ERR_TIMEOUT;
    }
    
    // Check for errors
    uint8_t status = atapi_read_status(device);
    if (status & ATA_STATUS_ERR) {
        return ATAPI_ERR_IO_ERROR;
    }
    
    return ATAPI_ERR_NONE;
}

// ========================================================================
// DEVICE OPERATIONS
// ========================================================================

atapi_err_t atapi_test_unit_ready(uint32_t device_id) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    uint8_t packet[ATAPI_PACKET_SIZE] = {0};
    packet[0] = ATAPI_CMD_TEST_UNIT_READY;
    
    atapi_err_t result = atapi_send_packet(device, packet, NULL, 0, false);
    
    if (result == ATAPI_ERR_NONE) {
        device->media_present = true;
    } else if (result == ATAPI_ERR_IO_ERROR) {
        // Check sense data to determine if media is present
        atapi_sense_data_t sense;
        if (atapi_request_sense(device_id, &sense) == ATAPI_ERR_NONE) {
            if (sense.sense_key == ATAPI_SENSE_NOT_READY) {
                device->media_present = false;
                return ATAPI_ERR_NO_MEDIA;
            }
        }
    }
    
    return result;
}

atapi_err_t atapi_request_sense(uint32_t device_id, atapi_sense_data_t* sense) {
    if (!atapi_initialized || device_id >= atapi_device_count || !sense) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    uint8_t packet[ATAPI_PACKET_SIZE] = {0};
    packet[0] = ATAPI_CMD_REQUEST_SENSE;
    packet[4] = 18;  // Allocation length
    
    uint8_t sense_buffer[18];
    atapi_err_t result = atapi_send_packet(device, packet, sense_buffer, 18, true);
    
    if (result == ATAPI_ERR_NONE) {
        sense->error_code = sense_buffer[0];
        sense->sense_key = sense_buffer[2] & 0x0F;
        sense->information = (sense_buffer[3] << 24) | (sense_buffer[4] << 16) |
                           (sense_buffer[5] << 8) | sense_buffer[6];
        sense->asc = sense_buffer[12];
        sense->ascq = sense_buffer[13];
    }
    
    return result;
}

atapi_err_t atapi_read_capacity(uint32_t device_id, uint32_t* sector_count, uint32_t* sector_size) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    // First check if media is present
    atapi_err_t ready_result = atapi_test_unit_ready(device_id);
    if (ready_result == ATAPI_ERR_NO_MEDIA) {
        return ATAPI_ERR_NO_MEDIA;
    }
    
    uint8_t packet[ATAPI_PACKET_SIZE] = {0};
    packet[0] = ATAPI_CMD_READ_CAPACITY;
    
    uint8_t capacity_buffer[8];
    atapi_err_t result = atapi_send_packet(device, packet, capacity_buffer, 8, true);
    
    if (result == ATAPI_ERR_NONE) {
        uint32_t last_lba = (capacity_buffer[0] << 24) | (capacity_buffer[1] << 16) |
                           (capacity_buffer[2] << 8) | capacity_buffer[3];
        uint32_t block_size = (capacity_buffer[4] << 24) | (capacity_buffer[5] << 16) |
                             (capacity_buffer[6] << 8) | capacity_buffer[7];
        
        if (sector_count) {
            *sector_count = last_lba + 1;
            device->sector_count = last_lba + 1;
        }
        
        if (sector_size) {
            *sector_size = block_size;
            device->sector_size = block_size;
        }
    }
    
    return result;
}

atapi_err_t atapi_read_sectors(uint32_t device_id, uint32_t lba, uint32_t count, void* buffer) {
    if (!atapi_initialized || device_id >= atapi_device_count || !buffer || count == 0) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    // Check if media is present
    if (!device->media_present) {
        atapi_err_t ready_result = atapi_test_unit_ready(device_id);
        if (ready_result != ATAPI_ERR_NONE) {
            return ready_result;
        }
    }
    
    // Read sectors one at a time or in small chunks
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t sectors_read = 0;
    
    while (sectors_read < count) {
        uint32_t sectors_to_read = (count - sectors_read > 16) ? 16 : (count - sectors_read);
        
        uint8_t packet[ATAPI_PACKET_SIZE] = {0};
        packet[0] = ATAPI_CMD_READ_10;
        packet[2] = (lba >> 24) & 0xFF;
        packet[3] = (lba >> 16) & 0xFF;
        packet[4] = (lba >> 8) & 0xFF;
        packet[5] = lba & 0xFF;
        packet[7] = (sectors_to_read >> 8) & 0xFF;
        packet[8] = sectors_to_read & 0xFF;
        
        uint32_t transfer_size = sectors_to_read * device->sector_size;
        atapi_err_t result = atapi_send_packet(device, packet, buf, transfer_size, true);
        
        if (result != ATAPI_ERR_NONE) {
            device->error_count++;
            return result;
        }
        
        buf += transfer_size;
        lba += sectors_to_read;
        sectors_read += sectors_to_read;
    }
    
    device->read_count++;
    return ATAPI_ERR_NONE;
}

// ========================================================================
// MEDIA CONTROL
// ========================================================================

atapi_err_t atapi_eject(uint32_t device_id) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    // First unlock the drive
    uint8_t prevent_packet[ATAPI_PACKET_SIZE] = {0};
    prevent_packet[0] = ATAPI_CMD_PREVENT_ALLOW;
    prevent_packet[4] = 0;  // Allow removal
    
    atapi_send_packet(device, prevent_packet, NULL, 0, false);
    
    // Then eject
    uint8_t eject_packet[ATAPI_PACKET_SIZE] = {0};
    eject_packet[0] = ATAPI_CMD_START_STOP_UNIT;
    eject_packet[4] = 0x02;  // Eject bit
    
    atapi_err_t result = atapi_send_packet(device, eject_packet, NULL, 0, false);
    
    if (result == ATAPI_ERR_NONE) {
        device->media_present = false;
        device->media_changed = true;
    }
    
    return result;
}

atapi_err_t atapi_load(uint32_t device_id) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return ATAPI_ERR_INVALID_PARAM;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return ATAPI_ERR_NOT_INITIALIZED;
    }
    
    uint8_t packet[ATAPI_PACKET_SIZE] = {0};
    packet[0] = ATAPI_CMD_START_STOP_UNIT;
    packet[4] = 0x03;  // Start and load
    
    atapi_err_t result = atapi_send_packet(device, packet, NULL, 0, false);
    
    if (result == ATAPI_ERR_NONE) {
        // Wait a bit for media to load
        for (volatile int i = 0; i < 1000000; i++);
        
        // Check if media is now present
        atapi_test_unit_ready(device_id);
    }
    
    return result;
}

bool atapi_check_media(uint32_t device_id) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return false;
    }
    
    atapi_device_t* device = &atapi_devices[device_id];
    if (!device->present) {
        return false;
    }
    
    atapi_err_t result = atapi_test_unit_ready(device_id);
    return (result == ATAPI_ERR_NONE);
}

// ========================================================================
// INFORMATION AND UTILITIES
// ========================================================================

uint32_t atapi_get_device_count(void) {
    return atapi_device_count;
}

atapi_device_t* atapi_get_device_info(uint32_t device_id) {
    if (!atapi_initialized || device_id >= atapi_device_count) {
        return NULL;
    }
    
    return &atapi_devices[device_id];
}

void atapi_list_devices(void) {
    terminal_puts(&main_terminal, "\r\n=== ATAPI Devices ===\r\n");
    
    if (!atapi_initialized) {
        terminal_puts(&main_terminal, "ATAPI subsystem not initialized\r\n");
        return;
    }
    
    if (atapi_device_count == 0) {
        terminal_puts(&main_terminal, "No ATAPI devices found\r\n");
        return;
    }
    
    for (uint32_t i = 0; i < atapi_device_count; i++) {
        atapi_device_t* device = &atapi_devices[i];
        
        const char* bus_name = (device->bus == 0) ? "Primary" : "Secondary";
        const char* drive_name = (device->drive == 0) ? "Master" : "Slave";
        
        terminal_printf(&main_terminal, "Device %u: %s %s\r\n", i, bus_name, drive_name);
        terminal_printf(&main_terminal, "  Model: %s\r\n", 
                       device->model[0] ? device->model : "Unknown");
        terminal_printf(&main_terminal, "  Serial: %s\r\n", 
                       device->serial[0] ? device->serial : "Unknown");
        terminal_printf(&main_terminal, "  Firmware: %s\r\n", 
                       device->firmware[0] ? device->firmware : "Unknown");
        terminal_printf(&main_terminal, "  Type: 0x%02x\r\n", device->device_type);
        terminal_printf(&main_terminal, "  Media: %s\r\n", 
                       device->media_present ? "Present" : "Not present");
        
        if (device->media_present && device->sector_count > 0) {
            uint32_t size_mb = (device->sector_count * device->sector_size) / (1024 * 1024);
            terminal_printf(&main_terminal, "  Capacity: %u sectors (%u MB)\r\n",
                           device->sector_count, size_mb);
            terminal_printf(&main_terminal, "  Sector size: %u bytes\r\n", 
                           device->sector_size);
        }
        
        terminal_printf(&main_terminal, "  Reads: %llu, Errors: %llu\r\n",
                       device->read_count, device->error_count);
        terminal_puts(&main_terminal, "\r\n");
    }
}

const char* atapi_get_error_string(atapi_err_t error) {
    switch (error) {
        case ATAPI_ERR_NONE:            return "No error";
        case ATAPI_ERR_INVALID_PARAM:   return "Invalid parameter";
        case ATAPI_ERR_NOT_INITIALIZED: return "Device not initialized";
        case ATAPI_ERR_NO_MEDIA:        return "No media present";
        case ATAPI_ERR_TIMEOUT:         return "Operation timeout";
        case ATAPI_ERR_IO_ERROR:        return "I/O error";
        case ATAPI_ERR_DEVICE_NOT_READY: return "Device not ready";
        case ATAPI_ERR_LBA_OUT_OF_RANGE: return "LBA out of range";
        case ATAPI_ERR_NOT_SUPPORTED:   return "Operation not supported";
        default:                        return "Unknown error";
    }
}

const char* atapi_get_sense_key_string(atapi_sense_key_t sense_key) {
    switch (sense_key) {
        case ATAPI_SENSE_NO_SENSE:        return "No sense";
        case ATAPI_SENSE_RECOVERED_ERROR: return "Recovered error";
        case ATAPI_SENSE_NOT_READY:       return "Not ready";
        case ATAPI_SENSE_MEDIUM_ERROR:    return "Medium error";
        case ATAPI_SENSE_HARDWARE_ERROR:  return "Hardware error";
        case ATAPI_SENSE_ILLEGAL_REQUEST: return "Illegal request";
        case ATAPI_SENSE_UNIT_ATTENTION:  return "Unit attention";
        case ATAPI_SENSE_DATA_PROTECT:    return "Data protect";
        case ATAPI_SENSE_ABORTED_COMMAND: return "Aborted command";
        default:                          return "Unknown sense key";
    }
}

// ========================================================================
// LOW-LEVEL HELPER FUNCTIONS
// ========================================================================

static void atapi_select_drive(atapi_device_t* device) {
    uint8_t drive_select = 0xA0 | (device->drive << 4);
    outb(device->io_base + ATA_REG_DRIVE_SELECT, drive_select);
}

static void atapi_400ns_delay(atapi_device_t* device) {
    // Read status 4 times for ~400ns delay
    for (int i = 0; i < 4; i++) {
        inb(device->io_ctrl);
    }
}

static uint8_t atapi_read_status(atapi_device_t* device) {
    return inb(device->io_base + ATA_REG_STATUS);
}

static bool atapi_wait_not_busy(atapi_device_t* device) {
    uint32_t timeout = ATAPI_SPIN_TIMEOUT;
    
    while (timeout--) {
        uint8_t status = atapi_read_status(device);
        if (!(status & ATA_STATUS_BSY)) {
            return true;
        }
        __asm__ volatile("pause");
    }
    
    return false;
}

static bool atapi_wait_ready(atapi_device_t* device) {
    uint32_t timeout = ATAPI_SPIN_TIMEOUT;
    
    while (timeout--) {
        uint8_t status = atapi_read_status(device);
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_RDY)) {
            return true;
        }
        __asm__ volatile("pause");
    }
    
    return false;
}

static bool atapi_wait_drq(atapi_device_t* device) {
    uint32_t timeout = ATAPI_SPIN_TIMEOUT;
    
    while (timeout--) {
        uint8_t status = atapi_read_status(device);
        if (status & ATA_STATUS_ERR) {
            return false;
        }
        if (status & ATA_STATUS_DRQ) {
            return true;
        }
        __asm__ volatile("pause");
    }
    
    return false;
}

bool atapi_verify_device_signature(uint8_t bus, uint8_t drive) {
    uint16_t io_base = (bus == 0) ? 0x1F0 : 0x170;
    
    // Seleccionar drive
    outb(io_base + 6, 0xA0 | (drive << 4));
    
    // Esperar 400ns
    for (int i = 0; i < 4; i++) {
        inb((bus == 0) ? 0x3F6 : 0x376);
    }
    
    // Enviar DEVICE RESET
    outb(io_base + 7, 0x08);
    
    // Esperar
    for (volatile int i = 0; i < 100000; i++);
    
    // Leer signature
    uint8_t lba_mid = inb(io_base + 4);
    uint8_t lba_high = inb(io_base + 5);
    
    return (lba_mid == 0x14 && lba_high == 0xEB);
}