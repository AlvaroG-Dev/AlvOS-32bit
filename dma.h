#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stdbool.h>

// DMA Channel registers (ISA DMA)
#define DMA_CHANNEL_0_ADDRESS    0x00
#define DMA_CHANNEL_0_COUNT      0x01
#define DMA_CHANNEL_1_ADDRESS    0x02
#define DMA_CHANNEL_1_COUNT      0x03
#define DMA_CHANNEL_2_ADDRESS    0x04
#define DMA_CHANNEL_2_COUNT      0x05
#define DMA_CHANNEL_3_ADDRESS    0x06
#define DMA_CHANNEL_3_COUNT      0x07
#define DMA_CHANNEL_4_ADDRESS    0xC0
#define DMA_CHANNEL_4_COUNT      0xC2
#define DMA_CHANNEL_5_ADDRESS    0xC4
#define DMA_CHANNEL_5_COUNT      0xC6
#define DMA_CHANNEL_6_ADDRESS    0xC8
#define DMA_CHANNEL_6_COUNT      0xCA
#define DMA_CHANNEL_7_ADDRESS    0xCC
#define DMA_CHANNEL_7_COUNT      0xCE

// DMA Command registers
#define DMA_COMMAND_REG_0        0x08
#define DMA_COMMAND_REG_1        0xD0
#define DMA_STATUS_REG_0         0x08
#define DMA_STATUS_REG_1         0xD0
#define DMA_REQUEST_REG_0        0x09
#define DMA_REQUEST_REG_1        0xD2
#define DMA_SINGLE_CHANNEL_MASK_0 0x0A
#define DMA_SINGLE_CHANNEL_MASK_1 0xD4
#define DMA_MODE_REG_0           0x0B
#define DMA_MODE_REG_1           0xD6
#define DMA_FLIP_FLOP_RESET_0    0x0C
#define DMA_FLIP_FLOP_RESET_1    0xD8
#define DMA_INTERMEDIATE_REG_0   0x0D
#define DMA_INTERMEDIATE_REG_1   0xDA
#define DMA_MASTER_RESET_0       0x0D
#define DMA_MASTER_RESET_1       0xDA
#define DMA_MASK_RESET_0         0x0E
#define DMA_MASK_RESET_1         0xDC
#define DMA_MULTI_CHANNEL_MASK_0 0x0F
#define DMA_MULTI_CHANNEL_MASK_1 0xDE

// Page registers for DMA
#define DMA_PAGE_0               0x87  // Channel 0
#define DMA_PAGE_1               0x83  // Channel 1
#define DMA_PAGE_2               0x81  // Channel 2
#define DMA_PAGE_3               0x82  // Channel 3
#define DMA_PAGE_4               0x8F  // Channel 4
#define DMA_PAGE_5               0x8B  // Channel 5
#define DMA_PAGE_6               0x89  // Channel 6
#define DMA_PAGE_7               0x8A  // Channel 7

// DMA Transfer modes
#define DMA_MODE_DEMAND          0x00
#define DMA_MODE_SINGLE          0x40
#define DMA_MODE_BLOCK           0x80
#define DMA_MODE_CASCADE         0xC0

// DMA Transfer directions
#define DMA_MODE_VERIFY          0x00
#define DMA_MODE_WRITE           0x04  // Write to memory (read from device)
#define DMA_MODE_READ            0x08  // Read from memory (write to device)

// DMA Auto-init
#define DMA_MODE_AUTO_INIT       0x10

// DMA Address increment/decrement
#define DMA_MODE_INCREMENT       0x00
#define DMA_MODE_DECREMENT       0x20

// Maximum DMA transfer sizes
#define DMA_MAX_TRANSFER_8BIT    65536
#define DMA_MAX_TRANSFER_16BIT   131072

// DMA Buffer alignment requirements
#define DMA_BUFFER_ALIGNMENT     4096
#define DMA_MAX_BUFFER_SIZE      65536

// DMA Channel structure
typedef struct {
    uint8_t channel;
    bool is_16bit;
    bool in_use;
    uint32_t physical_address;
    uint32_t size;
    uint8_t mode;
    void (*completion_callback)(void* data);
    void* callback_data;
} dma_channel_t;

// DMA Buffer for AHCI/SATA operations
typedef struct {
    void* virtual_address;
    uint32_t physical_address;
    uint32_t size;
    bool allocated;
    uint8_t alignment_padding[16];  // Para alineación de 16 bytes
} dma_buffer_t;

// Physical Region Descriptor Table (PRDT) entry for DMA operations
typedef struct __attribute__((packed)) {
    uint32_t data_base_address;     // Data Base Address
    uint32_t data_base_address_upper; // Data Base Address Upper 32-bits
    uint32_t reserved;              // Reserved
    uint32_t byte_count : 22;       // Byte count (max 4MB)
    uint32_t reserved2 : 9;         // Reserved
    uint32_t interrupt_on_completion : 1; // Interrupt on completion
} dma_prdt_entry_t;

// DMA Descriptor for AHCI command
typedef struct {
    uint8_t command_fis[64];        // Command FIS
    uint8_t atapi_command[16];      // ATAPI Command
    uint8_t reserved[48];           // Reserved
    dma_prdt_entry_t prdt[1];         // Physical Region Descriptor Table
} ahci_command_table_t;

// Function prototypes
bool dma_init(void);
void dma_cleanup(void);

// ISA DMA functions
bool dma_setup_channel(uint8_t channel, uint32_t physical_addr, uint16_t size, uint8_t mode);
bool dma_start_transfer(uint8_t channel);
bool dma_stop_transfer(uint8_t channel);
uint16_t dma_get_transfer_count(uint8_t channel);
bool dma_is_transfer_complete(uint8_t channel);

// DMA Buffer management for AHCI
dma_buffer_t* dma_alloc_buffer(uint32_t size, uint32_t alignment);
void dma_free_buffer(dma_buffer_t* buffer);
bool dma_buffer_is_valid(dma_buffer_t* buffer);

// PRDT management
dma_prdt_entry_t* dma_create_prdt(void* data_buffer, uint32_t size, uint32_t max_entries);
void dma_free_prdt(dma_prdt_entry_t* prdt);

// Utility functions
uint32_t dma_virt_to_phys(void* virtual_addr);
void* dma_phys_to_virt(uint32_t physical_addr);
bool dma_address_is_dma_capable(uint32_t physical_addr);

// Debug functions
void dma_print_status(void);
void dma_test_basic_transfer(void);

#endif // DMA_H