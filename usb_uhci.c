#include "usb_uhci.h"
#include "usb_ehci.h"
#include "io.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"
#include "mmu.h"
#include "irq.h"

#define UHCI_TIMEOUT_MS 10000

// ========================================================================
// INITIALIZATION
// ========================================================================

bool uhci_init(usb_controller_t* controller) {
    terminal_puts(&main_terminal, "Initializing UHCI controller...\r\n");
    
    pci_device_t* pci_dev = controller->pci_dev;
    
    // Enable bus mastering and I/O space
    pci_enable_bus_mastering(pci_dev);
    pci_enable_io_space(pci_dev);
    
    // Get I/O base from BAR4
    if (!pci_dev->bars[4].is_valid || pci_dev->bars[4].type != PCI_BAR_TYPE_IO) {
        terminal_puts(&main_terminal, "UHCI: BAR4 not valid or not I/O type\r\n");
        return false;
    }
    
    // Allocate UHCI-specific data
    uhci_data_t* uhci = (uhci_data_t*)kernel_malloc(sizeof(uhci_data_t));
    if (!uhci) {
        terminal_puts(&main_terminal, "UHCI: Failed to allocate controller data\r\n");
        return false;
    }
    memset(uhci, 0, sizeof(uhci_data_t));
    
    uhci->io_base = (uint16_t)pci_dev->bars[4].address;
    controller->regs = uhci;
    
    terminal_printf(&main_terminal, "UHCI: I/O base = 0x%04x\r\n", uhci->io_base);
    
    // Reset controller
    outw(uhci->io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    
    // Wait for reset to complete
    uint32_t timeout = 1000000;
    while ((inw(uhci->io_base + UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) && timeout--) {
        __asm__ volatile("pause");
    }
    
    if (inw(uhci->io_base + UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) {
        terminal_puts(&main_terminal, "UHCI: Reset timeout\r\n");
        kernel_free(uhci);
        return false;
    }
    
    // Allocate frame list (1024 entries, 4KB aligned)
    uhci->frame_list_buffer = dma_alloc_buffer(UHCI_FRAME_LIST_SIZE * 4, 4096);
    if (!uhci->frame_list_buffer) {
        terminal_puts(&main_terminal, "UHCI: Failed to allocate frame list\r\n");
        kernel_free(uhci);
        return false;
    }
    
    uhci->frame_list = (uint32_t*)uhci->frame_list_buffer->virtual_address;
    memset(uhci->frame_list, 0, UHCI_FRAME_LIST_SIZE * 4);
    
    // Allocate Queue Heads
    uhci->qh_buffer = dma_alloc_buffer(sizeof(uhci_qh_t) * 2, 16);
    if (!uhci->qh_buffer) {
        terminal_puts(&main_terminal, "UHCI: Failed to allocate QHs\r\n");
        dma_free_buffer(uhci->frame_list_buffer);
        kernel_free(uhci);
        return false;
    }
    
    uhci->control_qh = (uhci_qh_t*)uhci->qh_buffer->virtual_address;
    uhci->bulk_qh = uhci->control_qh + 1;
    
    memset(uhci->control_qh, 0, sizeof(uhci_qh_t) * 2);
    
    // Setup queue heads
    uint32_t bulk_qh_phys = uhci->qh_buffer->physical_address + sizeof(uhci_qh_t);
    uhci->control_qh->head_link_ptr = bulk_qh_phys | 0x2; // QH pointer
    uhci->control_qh->element_link_ptr = 0x1; // Terminate
    
    uhci->bulk_qh->head_link_ptr = 0x1; // Terminate
    uhci->bulk_qh->element_link_ptr = 0x1; // Terminate
    
    // Allocate TD pool
    uhci->td_pool_buffer = dma_alloc_buffer(sizeof(uhci_td_t) * 64, 16);
    if (!uhci->td_pool_buffer) {
        terminal_puts(&main_terminal, "UHCI: Failed to allocate TD pool\r\n");
        dma_free_buffer(uhci->qh_buffer);
        dma_free_buffer(uhci->frame_list_buffer);
        kernel_free(uhci);
        return false;
    }
    
    uhci->td_pool = (uhci_td_t*)uhci->td_pool_buffer->virtual_address;
    memset(uhci->td_pool, 0, sizeof(uhci_td_t) * 64);
    
    // Initialize frame list to point to control QH
    uint32_t control_qh_phys = uhci->qh_buffer->physical_address;
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) {
        uhci->frame_list[i] = control_qh_phys | 0x2; // QH pointer
    }
    
    // Set frame list base address
    outl(uhci->io_base + UHCI_REG_FRBASEADD, uhci->frame_list_buffer->physical_address);
    
    // Set frame number to 0
    outw(uhci->io_base + UHCI_REG_FRNUM, 0);
    
    // Set SOF timing to 64
    outb(uhci->io_base + UHCI_REG_SOFMOD, 64);
    
    // Enable interrupts
    outw(uhci->io_base + UHCI_REG_USBINTR, 0x0F);
    
    // Start controller
    outw(uhci->io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    
    controller->initialized = true;
    
    terminal_puts(&main_terminal, "UHCI: Controller started\r\n");
    
    // Detect devices on ports
    uhci_detect_ports(controller);
    
    return true;
}

void uhci_cleanup(usb_controller_t* controller) {
    if (!controller->regs) return;
    
    uhci_data_t* uhci = (uhci_data_t*)controller->regs;
    
    // Stop controller
    outw(uhci->io_base + UHCI_REG_USBCMD, 0);
    
    // Free DMA buffers
    if (uhci->td_pool_buffer) dma_free_buffer(uhci->td_pool_buffer);
    if (uhci->qh_buffer) dma_free_buffer(uhci->qh_buffer);
    if (uhci->frame_list_buffer) dma_free_buffer(uhci->frame_list_buffer);
    
    kernel_free(uhci);
    controller->regs = NULL;
}

bool uhci_detect_ports(usb_controller_t* controller) {
    uhci_data_t* uhci = (uhci_data_t*)controller->regs;
    
    terminal_puts(&main_terminal, "UHCI: Detecting ports...\r\n");
    
    // UHCI typically has 2 ports
    for (uint8_t port = 0; port < 2; port++) {
        uint16_t port_reg = UHCI_REG_PORTSC1 + (port * 2);
        uint16_t status = inw(uhci->io_base + port_reg);
        
        if (status & UHCI_PORT_CCS) {
            terminal_printf(&main_terminal, "UHCI: Device detected on port %u\r\n", port);
            
            // Reset port
            if (uhci_reset_port(controller, port)) {
                // Enumerate device
                usb_enumerate_device(controller, port);
            }
        }
    }
    
    return true;
}

bool uhci_reset_port(usb_controller_t* controller, uint8_t port) {
    uhci_data_t* uhci = (uhci_data_t*)controller->regs;
    uint16_t port_reg = UHCI_REG_PORTSC1 + (port * 2);
    
    // Set port reset
    uint16_t status = inw(uhci->io_base + port_reg);
    outw(uhci->io_base + port_reg, status | UHCI_PORT_PR);
    
    // Wait 50ms
    for (volatile int i = 0; i < 5000000; i++);
    
    // Clear port reset
    status = inw(uhci->io_base + port_reg);
    outw(uhci->io_base + port_reg, status & ~UHCI_PORT_PR);
    
    // Wait for port to become enabled
    for (volatile int i = 0; i < 1000000; i++);
    
    status = inw(uhci->io_base + port_reg);
    
    // Enable port
    outw(uhci->io_base + port_reg, status | UHCI_PORT_PE);
    
    // Wait
    for (volatile int i = 0; i < 1000000; i++);
    
    status = inw(uhci->io_base + port_reg);
    if (!(status & UHCI_PORT_PE)) {
        terminal_printf(&main_terminal, "UHCI: Failed to enable port %u\r\n", port);
        return false;
    }
    
    return true;
}

// ========================================================================
// TRANSFER FUNCTIONS
// ========================================================================

uhci_td_t* uhci_alloc_td(uhci_data_t* uhci) {
    for (int i = 0; i < 64; i++) {
        if (!uhci->td_used[i]) {
            uhci->td_used[i] = true;
            uhci_td_t* td = &uhci->td_pool[i];
            memset(td, 0, sizeof(uhci_td_t));
            return td;
        }
    }
    return NULL;
}

void uhci_free_td(uhci_data_t* uhci, uhci_td_t* td) {
    int index = td - uhci->td_pool;
    if (index >= 0 && index < 64) {
        uhci->td_used[index] = false;
    }
}

bool uhci_wait_for_td(uhci_td_t* td, uint32_t timeout_ms) {
    uint32_t start = ticks_since_boot;
    
    while (td->status & UHCI_TD_ACTIVE) {
        if ((ticks_since_boot - start) > (timeout_ms / 10)) {
            return false; // Timeout
        }
        __asm__ volatile("pause");
    }
    
    // Check for errors
    if (td->status & (UHCI_TD_STALLED | UHCI_TD_DBUF | UHCI_TD_BABBLE | 
                      UHCI_TD_CRCTO | UHCI_TD_BITSTUFF)) {
        return false;
    }
    
    return true;
}

bool uhci_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                           void* data, uint16_t length) {
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    uhci_data_t* uhci = (uhci_data_t*)controller->regs;
    
    // Allocate TDs
    uhci_td_t* setup_td = uhci_alloc_td(uhci);
    uhci_td_t* status_td = uhci_alloc_td(uhci);
    uhci_td_t* data_td = NULL;
    
    if (!setup_td || !status_td) {
        if (setup_td) uhci_free_td(uhci, setup_td);
        if (status_td) uhci_free_td(uhci, status_td);
        return false;
    }
    
    if (length > 0) {
        data_td = uhci_alloc_td(uhci);
        if (!data_td) {
            uhci_free_td(uhci, setup_td);
            uhci_free_td(uhci, status_td);
            return false;
        }
    }
    
    // Get physical addresses
    uint32_t setup_phys = mmu_virtual_to_physical((uint32_t)setup);
    uint32_t data_phys = data ? mmu_virtual_to_physical((uint32_t)data) : 0;
    
    // Build SETUP TD
    uint32_t td_phys_base = uhci->td_pool_buffer->physical_address;
    uint32_t setup_td_phys = td_phys_base + ((uint8_t*)setup_td - (uint8_t*)uhci->td_pool);
    uint32_t data_td_phys = data_td ? td_phys_base + ((uint8_t*)data_td - (uint8_t*)uhci->td_pool) : 0;
    uint32_t status_td_phys = td_phys_base + ((uint8_t*)status_td - (uint8_t*)uhci->td_pool);
    
    setup_td->link_ptr = data_td ? (data_td_phys | 0x4) : (status_td_phys | 0x4);
    setup_td->status = UHCI_TD_ACTIVE | (3 << 27); // 3 errors allowed
    setup_td->token = (7 << 21) | (device->address << 8) | UHCI_PID_SETUP;
    setup_td->buffer = setup_phys;
    
    // Build DATA TD if present
    if (data_td) {
        data_td->link_ptr = status_td_phys | 0x4;
        data_td->status = UHCI_TD_ACTIVE | (3 << 27);
        
        uint8_t pid = (setup->bmRequestType & 0x80) ? UHCI_PID_IN : UHCI_PID_OUT;
        data_td->token = ((length - 1) << 21) | (device->address << 8) | (1 << 19) | pid;
        data_td->buffer = data_phys;
    }
    
    // Build STATUS TD
    status_td->link_ptr = 0x1; // Terminate
    status_td->status = UHCI_TD_ACTIVE | UHCI_TD_IOC | (3 << 27);
    
    uint8_t status_pid = (length > 0 && (setup->bmRequestType & 0x80)) ? UHCI_PID_OUT : UHCI_PID_IN;
    status_td->token = (0x7FF << 21) | (device->address << 8) | (1 << 19) | status_pid;
    status_td->buffer = 0;
    
    // Add to control queue
    uhci->control_qh->element_link_ptr = setup_td_phys | 0x4;
    
    // Wait for completion
    bool result = uhci_wait_for_td(status_td, UHCI_TIMEOUT_MS);
    
    // Clean up
    uhci->control_qh->element_link_ptr = 0x1;
    uhci_free_td(uhci, setup_td);
    uhci_free_td(uhci, status_td);
    if (data_td) uhci_free_td(uhci, data_td);
    
    return result;
}

bool uhci_bulk_transfer(usb_device_t* device, uint8_t endpoint,
                        void* data, uint32_t length, bool is_in) {
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    uhci_data_t* uhci = (uhci_data_t*)controller->regs;
    
    if (length == 0) return true;
    
    // For simplicity, transfer one TD at a time (max 1280 bytes)
    uint32_t max_packet = 64;
    uint32_t transferred = 0;
    
    while (transferred < length) {
        uint32_t transfer_size = length - transferred;
        if (transfer_size > max_packet) transfer_size = max_packet;
        
        uhci_td_t* td = uhci_alloc_td(uhci);
        if (!td) return false;
        
        uint32_t data_phys = mmu_virtual_to_physical((uint32_t)((uint8_t*)data + transferred));
        uint32_t td_phys = uhci->td_pool_buffer->physical_address + 
                          ((uint8_t*)td - (uint8_t*)uhci->td_pool);
        
        td->link_ptr = 0x1; // Terminate
        td->status = UHCI_TD_ACTIVE | UHCI_TD_IOC | (3 << 27);
        
        uint8_t pid = is_in ? UHCI_PID_IN : UHCI_PID_OUT;
        uint8_t toggle = (transferred / max_packet) & 1;
        td->token = ((transfer_size - 1) << 21) | (endpoint << 15) | 
                    (device->address << 8) | (toggle << 19) | pid;
        td->buffer = data_phys;
        
        // Add to bulk queue
        uhci->bulk_qh->element_link_ptr = td_phys | 0x4;
        
        // Wait for completion
        if (!uhci_wait_for_td(td, UHCI_TIMEOUT_MS)) {
            uhci->bulk_qh->element_link_ptr = 0x1;
            uhci_free_td(uhci, td);
            return false;
        }
        
        uhci->bulk_qh->element_link_ptr = 0x1;
        uhci_free_td(uhci, td);
        
        transferred += transfer_size;
    }
    
    return true;
}

// Implement the dispatch functions from usb_core.h
bool usb_control_transfer(usb_device_t* device, usb_setup_packet_t* setup, 
                          void* data, uint16_t length) {
    if (!device || !setup) {
        return false;
    }
    
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    
    // Intentar con el controlador específico primero
    bool result = false;
    switch (controller->type) {
        case USB_TYPE_UHCI:
            result = uhci_control_transfer(device, setup, data, length);
            break;
        case USB_TYPE_EHCI:
            result = ehci_control_transfer(device, setup, data, length);
            if (!result) {
                terminal_puts(&main_terminal, "EHCI control transfer failed, trying UHCI companion\r\n");
                // Buscar controlador UHCI companion
                for (uint8_t i = 0; i < usb_controller_count; i++) {
                    if (usb_controllers[i].type == USB_TYPE_UHCI) {
                        device->controller_id = i; // Temporalmente cambiar controlador
                        result = uhci_control_transfer(device, setup, data, length);
                        device->controller_id = controller->id; // Restaurar
                        if (result) break;
                    }
                }
            }
            break;
        default:
            break;
    }
    
    return result;
}

bool usb_bulk_transfer(usb_device_t* device, uint8_t endpoint,
                       void* data, uint32_t length, bool is_in) {
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    
    switch (controller->type) {
        case USB_TYPE_UHCI:
            return uhci_bulk_transfer(device, endpoint, data, length, is_in);
        case USB_TYPE_EHCI:
            return ehci_bulk_transfer(device, endpoint, data, length, is_in);
        default:
            return false;
    }
}