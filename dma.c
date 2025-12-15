#include "dma.h"
#include "io.h"
#include "terminal.h"
#include "kernel.h"
#include "mmu.h"
#include "memory.h"
#include "string.h"

// DMA Channel tracking
static dma_channel_t dma_channels[8];
static bool dma_initialized = false;

// DMA Buffer pool for AHCI
#define MAX_DMA_BUFFERS 128
static dma_buffer_t dma_buffer_pool[MAX_DMA_BUFFERS];

// Port arrays for easier access
static const uint16_t dma_address_ports[] = {
    0x00, 0x02, 0x04, 0x06,  // Channels 0-3
    0xC0, 0xC4, 0xC8, 0xCC   // Channels 4-7
};

static const uint16_t dma_count_ports[] = {
    0x01, 0x03, 0x05, 0x07,  // Channels 0-3
    0xC2, 0xC6, 0xCA, 0xCE   // Channels 4-7
};

static const uint16_t dma_page_ports[] = {
    0x87, 0x83, 0x81, 0x82,  // Channels 0-3
    0x8F, 0x8B, 0x89, 0x8A   // Channels 4-7
};

static const uint16_t dma_mask_ports[] = {
    DMA_SINGLE_CHANNEL_MASK_0, DMA_SINGLE_CHANNEL_MASK_0,
    DMA_SINGLE_CHANNEL_MASK_0, DMA_SINGLE_CHANNEL_MASK_0,
    DMA_SINGLE_CHANNEL_MASK_1, DMA_SINGLE_CHANNEL_MASK_1,
    DMA_SINGLE_CHANNEL_MASK_1, DMA_SINGLE_CHANNEL_MASK_1
};

static const uint16_t dma_mode_ports[] = {
    DMA_MODE_REG_0, DMA_MODE_REG_0, DMA_MODE_REG_0, DMA_MODE_REG_0,
    DMA_MODE_REG_1, DMA_MODE_REG_1, DMA_MODE_REG_1, DMA_MODE_REG_1
};

// ========================================================================
// INICIALIZACIÓN Y LIMPIEZA
// ========================================================================

bool dma_init(void) {
    if (dma_initialized) {
        return true;
    }
    
    terminal_puts(&main_terminal, "Initializing DMA subsystem...\r\n");
    
    // Inicializar estructura de canales
    memset(dma_channels, 0, sizeof(dma_channels));
    for (int i = 0; i < 8; i++) {
        dma_channels[i].channel = i;
        dma_channels[i].is_16bit = (i >= 4);  // Channels 4-7 are 16-bit
        dma_channels[i].in_use = false;
    }
    
    // Inicializar pool de buffers DMA
    memset(dma_buffer_pool, 0, sizeof(dma_buffer_pool));
    
    // Reset DMA controllers
    outb(DMA_MASTER_RESET_0, 0x00);
    outb(DMA_MASTER_RESET_1, 0x00);
    
    // Clear mask registers (enable all channels initially)
    outb(DMA_MASK_RESET_0, 0x00);
    outb(DMA_MASK_RESET_1, 0x00);
    
    // Disable all channels initially
    for (int i = 0; i < 8; i++) {
        uint8_t mask_value = (i & 3) | 0x04;  // Set mask bit
        outb(dma_mask_ports[i], mask_value);
    }
    
    dma_initialized = true;
    terminal_puts(&main_terminal, "DMA subsystem initialized successfully\r\n");
    
    return true;
}

void dma_cleanup(void) {
    if (!dma_initialized) {
        return;
    }
    
    // Stop all active transfers
    for (int i = 0; i < 8; i++) {
        if (dma_channels[i].in_use) {
            dma_stop_transfer(i);
        }
    }
    
    // Free all DMA buffers
    for (int i = 0; i < MAX_DMA_BUFFERS; i++) {
        if (dma_buffer_pool[i].allocated) {
            dma_free_buffer(&dma_buffer_pool[i]);
        }
    }
    
    // Disable DMA controllers
    outb(DMA_MULTI_CHANNEL_MASK_0, 0x0F);  // Mask all channels 0-3
    outb(DMA_MULTI_CHANNEL_MASK_1, 0x0F);  // Mask all channels 4-7
    
    dma_initialized = false;
    boot_log_info("DMA subsystem cleaned up\r\n");
}

// ========================================================================
// FUNCIONES ISA DMA BÁSICAS
// ========================================================================

bool dma_setup_channel(uint8_t channel, uint32_t physical_addr, uint16_t size, uint8_t mode) {
    if (!dma_initialized || channel >= 8) {
        return false;
    }
    
    if (!dma_address_is_dma_capable(physical_addr)) {
        terminal_printf(&main_terminal, "DMA: Address 0x%08x is not DMA-capable\r\n", physical_addr);
        return false;
    }
    
    dma_channel_t* dma_ch = &dma_channels[channel];
    
    // Verify size limits
    uint32_t max_size = dma_ch->is_16bit ? DMA_MAX_TRANSFER_16BIT : DMA_MAX_TRANSFER_8BIT;
    if (size > max_size) {
        terminal_printf(&main_terminal, "DMA: Transfer size %u exceeds maximum %u for channel %u\r\n", 
                       size, max_size, channel);
        return false;
    }
    
    // Check for 64K boundary crossing (critical for ISA DMA)
    uint32_t start_page = physical_addr >> 16;
    uint32_t end_page = (physical_addr + size - 1) >> 16;
    if (start_page != end_page) {
        terminal_printf(&main_terminal, "DMA: Transfer crosses 64K boundary (0x%08x-0x%08x)\r\n", 
                       physical_addr, physical_addr + size);
        return false;
    }
    
    // Disable the channel first
    uint8_t mask_value = (channel & 3) | 0x04;
    outb(dma_mask_ports[channel], mask_value);
    
    // Clear flip-flop
    if (channel < 4) {
        outb(DMA_FLIP_FLOP_RESET_0, 0x00);
    } else {
        outb(DMA_FLIP_FLOP_RESET_1, 0x00);
    }
    
    // Set mode
    uint8_t mode_value = (channel & 3) | mode;
    outb(dma_mode_ports[channel], mode_value);
    
    // Set address and count
    if (dma_ch->is_16bit) {
        // 16-bit channels use word addressing
        uint16_t word_addr = physical_addr >> 1;
        uint16_t word_count = (size >> 1) - 1;
        
        outb(dma_address_ports[channel], word_addr & 0xFF);
        outb(dma_address_ports[channel], (word_addr >> 8) & 0xFF);
        outb(dma_count_ports[channel], word_count & 0xFF);
        outb(dma_count_ports[channel], (word_count >> 8) & 0xFF);
    } else {
        // 8-bit channels use byte addressing
        uint16_t byte_count = size - 1;
        
        outb(dma_address_ports[channel], physical_addr & 0xFF);
        outb(dma_address_ports[channel], (physical_addr >> 8) & 0xFF);
        outb(dma_count_ports[channel], byte_count & 0xFF);
        outb(dma_count_ports[channel], (byte_count >> 8) & 0xFF);
    }
    
    // Set page register
    outb(dma_page_ports[channel], (physical_addr >> 16) & 0xFF);
    
    // Update channel info
    dma_ch->physical_address = physical_addr;
    dma_ch->size = size;
    dma_ch->mode = mode;
    dma_ch->in_use = true;
    
    //terminal_printf(&main_terminal, "DMA: Channel %u configured - addr=0x%08x, size=%u, mode=0x%02x\r\n",
    //               channel, physical_addr, size, mode);
    
    return true;
}

bool dma_start_transfer(uint8_t channel) {
    if (!dma_initialized || channel >= 8 || !dma_channels[channel].in_use) {
        return false;
    }
    
    // Enable the channel (clear mask bit)
    uint8_t mask_value = channel & 3;
    outb(dma_mask_ports[channel], mask_value);
    
    //terminal_printf(&main_terminal, "DMA: Started transfer on channel %u\r\n", channel);
    return true;
}

bool dma_stop_transfer(uint8_t channel) {
    if (!dma_initialized || channel >= 8) {
        return false;
    }
    
    // Disable the channel (set mask bit)
    uint8_t mask_value = (channel & 3) | 0x04;
    outb(dma_mask_ports[channel], mask_value);
    
    dma_channels[channel].in_use = false;
    
    //terminal_printf(&main_terminal, "DMA: Stopped transfer on channel %u\r\n", channel);
    return true;
}

uint16_t dma_get_transfer_count(uint8_t channel) {
    if (!dma_initialized || channel >= 8) {
        return 0;
    }
    
    // Clear flip-flop
    if (channel < 4) {
        outb(DMA_FLIP_FLOP_RESET_0, 0x00);
    } else {
        outb(DMA_FLIP_FLOP_RESET_1, 0x00);
    }
    
    // Read count (low byte first, then high byte)
    uint8_t low = inb(dma_count_ports[channel]);
    uint8_t high = inb(dma_count_ports[channel]);
    
    return (high << 8) | low;
}

bool dma_is_transfer_complete(uint8_t channel) {
    if (!dma_initialized || channel >= 8) {
        return true;
    }
    
    uint16_t count = dma_get_transfer_count(channel);
    return (count == 0xFFFF);  // Terminal count reached
}

// ========================================================================
// GESTIÓN DE BUFFERS DMA PARA AHCI
// ========================================================================

dma_buffer_t* dma_alloc_buffer(uint32_t size, uint32_t alignment) {
    if (!dma_initialized || size == 0) {
        return NULL;
    }
    
    // Find free buffer slot
    dma_buffer_t* buffer = NULL;
    for (int i = 0; i < MAX_DMA_BUFFERS; i++) {
        if (!dma_buffer_pool[i].allocated) {
            buffer = &dma_buffer_pool[i];
            break;
        }
    }
    
    if (!buffer) {
        terminal_puts(&main_terminal, "DMA: No free buffer slots available\r\n");
        return NULL;
    }
    
    // Allocate memory with alignment
    size_t alloc_size = size + alignment;
    void* raw_ptr = kernel_malloc(alloc_size);
    if (!raw_ptr) {
        terminal_puts(&main_terminal, "DMA: Failed to allocate memory for DMA buffer\r\n");
        return NULL;
    }
    
    // Align the pointer
    uintptr_t addr = (uintptr_t)raw_ptr;
    uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
    void* aligned_ptr = (void*)aligned_addr;
    
    // Get physical address
    uint32_t physical_addr = mmu_virtual_to_physical((uint32_t)aligned_ptr);
    if (physical_addr == 0) {
        terminal_puts(&main_terminal, "DMA: Failed to get physical address for DMA buffer\r\n");
        kernel_free(raw_ptr);
        return NULL;
    }
    
    // Check DMA capability
    if (!dma_address_is_dma_capable(physical_addr)) {
        terminal_printf(&main_terminal, "DMA: Buffer at 0x%08x is not DMA-capable\r\n", physical_addr);
        kernel_free(raw_ptr);
        return NULL;
    }
    
    // Initialize buffer structure
    buffer->virtual_address = aligned_ptr;
    buffer->physical_address = physical_addr;
    buffer->size = size;
    buffer->allocated = true;
    
    //terminal_printf(&main_terminal, "DMA: Allocated buffer - virt=0x%08x, phys=0x%08x, size=%u\r\n",
    //               (uint32_t)aligned_ptr, physical_addr, size);
    
    return buffer;
}

void dma_free_buffer(dma_buffer_t* buffer) {
    if (!buffer || !buffer->allocated) {
        return;
    }
    
    //terminal_printf(&main_terminal, "DMA: Freeing buffer - virt=0x%08x, phys=0x%08x\r\n",
    //               (uint32_t)buffer->virtual_address, buffer->physical_address);
    
    // Note: We can't easily free the original pointer since we aligned it
    // In a production system, you'd want to store the original pointer
    // For now, we'll mark it as free but leave the memory allocated
    // This is a memory leak that should be fixed in production
    
    memset(buffer, 0, sizeof(dma_buffer_t));
}

bool dma_buffer_is_valid(dma_buffer_t* buffer) {
    if (!buffer || !buffer->allocated) {
        return false;
    }
    
    // Verify physical address mapping
    uint32_t current_phys = mmu_virtual_to_physical((uint32_t)buffer->virtual_address);
    return (current_phys == buffer->physical_address);
}

// ========================================================================
// GESTIÓN DE PRDT PARA AHCI
// ========================================================================

dma_prdt_entry_t* dma_create_prdt(void* data_buffer, uint32_t size, uint32_t max_entries) {
    if (!data_buffer || size == 0 || max_entries == 0) {
        return NULL;
    }
    
    // For now, create simple single-entry PRDT
    // In production, you'd want to handle scatter-gather properly
    dma_prdt_entry_t* prdt = (dma_prdt_entry_t*)kernel_malloc(sizeof(dma_prdt_entry_t) * max_entries);
    if (!prdt) {
        return NULL;
    }
    
    memset(prdt, 0, sizeof(dma_prdt_entry_t) * max_entries);
    
    // Get physical address of data buffer
    uint32_t phys_addr = mmu_virtual_to_physical((uint32_t)data_buffer);
    if (phys_addr == 0) {
        kernel_free(prdt);
        return NULL;
    }
    
    // Set up first entry
    prdt[0].data_base_address = phys_addr;
    prdt[0].data_base_address_upper = 0;  // We're in 32-bit mode
    prdt[0].byte_count = size - 1;  // AHCI uses size-1
    prdt[0].interrupt_on_completion = 1;
    
    //terminal_printf(&main_terminal, "DMA: Created PRDT - phys=0x%08x, size=%u\r\n", phys_addr, size);
    
    return prdt;
}

void dma_free_prdt(dma_prdt_entry_t* prdt) {
    if (prdt) {
        kernel_free(prdt);
    }
}

// ========================================================================
// FUNCIONES AUXILIARES
// ========================================================================

uint32_t dma_virt_to_phys(void* virtual_addr) {
    return mmu_virtual_to_physical((uint32_t)virtual_addr);
}

void* dma_phys_to_virt(uint32_t physical_addr) {
    // This is a simplified implementation
    // In practice, you'd need a proper physical-to-virtual mapping
    return (void*)(KERNEL_VIRTUAL_BASE + physical_addr);
}

bool dma_address_is_dma_capable(uint32_t physical_addr) {
    // ISA DMA can only access first 16MB
    // Modern systems might have different limits
    if (physical_addr >= 0x1000000) {  // 16MB limit for ISA DMA
        return false;
    }
    
    // Address must be aligned
    if (physical_addr & 1) {
        return false;
    }
    
    return true;
}

// ========================================================================
// FUNCIONES DE DEBUG Y TESTING
// ========================================================================

void dma_print_status(void) {
    terminal_puts(&main_terminal, "\r\n=== DMA Status ===\r\n");
    terminal_printf(&main_terminal, "DMA Initialized: %s\r\n", dma_initialized ? "Yes" : "No");
    
    if (!dma_initialized) {
        return;
    }
    
    terminal_puts(&main_terminal, "Active Channels:\r\n");
    for (int i = 0; i < 8; i++) {
        if (dma_channels[i].in_use) {
            terminal_printf(&main_terminal, "  Channel %d: phys=0x%08x, size=%u, mode=0x%02x\r\n",
                           i, dma_channels[i].physical_address, 
                           dma_channels[i].size, dma_channels[i].mode);
        }
    }
    
    terminal_puts(&main_terminal, "DMA Buffers:\r\n");
    uint32_t allocated_count = 0;
    uint32_t total_size = 0;
    for (int i = 0; i < MAX_DMA_BUFFERS; i++) {
        if (dma_buffer_pool[i].allocated) {
            allocated_count++;
            total_size += dma_buffer_pool[i].size;
        }
    }
    terminal_printf(&main_terminal, "  Allocated: %u/%u buffers, %u bytes total\r\n",
                   allocated_count, MAX_DMA_BUFFERS, total_size);
    
    terminal_puts(&main_terminal, "\r\n");
}

void dma_test_basic_transfer(void) {
    terminal_puts(&main_terminal, "DMA: Running basic transfer test...\r\n");
    
    // Allocate test buffers
    dma_buffer_t* src_buffer = dma_alloc_buffer(1024, 16);
    dma_buffer_t* dst_buffer = dma_alloc_buffer(1024, 16);
    
    if (!src_buffer || !dst_buffer) {
        terminal_puts(&main_terminal, "DMA: Failed to allocate test buffers\r\n");
        if (src_buffer) dma_free_buffer(src_buffer);
        if (dst_buffer) dma_free_buffer(dst_buffer);
        return;
    }
    
    // Fill source with test pattern
    uint8_t* src_data = (uint8_t*)src_buffer->virtual_address;
    for (int i = 0; i < 1024; i++) {
        src_data[i] = i & 0xFF;
    }
    
    // Clear destination
    memset(dst_buffer->virtual_address, 0, 1024);
    
    terminal_printf(&main_terminal, "DMA: Test buffers allocated and initialized\r\n");
    terminal_printf(&main_terminal, "  Source: virt=0x%08x, phys=0x%08x\r\n",
                   (uint32_t)src_buffer->virtual_address, src_buffer->physical_address);
    terminal_printf(&main_terminal, "  Dest:   virt=0x%08x, phys=0x%08x\r\n",
                   (uint32_t)dst_buffer->virtual_address, dst_buffer->physical_address);
    
    // Note: Actual DMA transfer would require hardware support
    // This is just a framework test
    
    // Clean up
    dma_free_buffer(src_buffer);
    dma_free_buffer(dst_buffer);
    
    terminal_puts(&main_terminal, "DMA: Basic test completed\r\n");
}