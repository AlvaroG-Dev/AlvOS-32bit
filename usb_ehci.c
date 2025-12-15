#include "usb_ehci.h"
#include "usb_core.h"
#include "io.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"
#include "mmu.h"
#include "pci.h"
#include "irq.h"

#define EHCI_TIMEOUT_MS 5000

// Helper macros for register access
#define EHCI_READ32(ehci, reg) (*((volatile uint32_t*)((ehci)->op_regs + (reg))))
#define EHCI_WRITE32(ehci, reg, val) (*((volatile uint32_t*)((ehci)->op_regs + (reg))) = (val))

// ========================================================================
// INITIALIZATION
// ========================================================================

bool ehci_init(usb_controller_t* controller) {
    terminal_puts(&main_terminal, "Initializing EHCI controller...\r\n");
    
    pci_device_t* pci_dev = controller->pci_dev;
    
    // Enable bus mastering and memory space
    pci_enable_bus_mastering(pci_dev);
    pci_enable_memory_space(pci_dev);
    
    // Get MMIO base from BAR0
    if (!pci_dev->bars[0].is_valid || pci_dev->bars[0].type != PCI_BAR_TYPE_MEMORY) {
        terminal_puts(&main_terminal, "EHCI: BAR0 not valid or not memory type\r\n");
        return false;
    }
    
    uint32_t mmio_base = pci_dev->bars[0].address;
    uint32_t mmio_size = pci_dev->bars[0].size;
    
    if (mmio_size < 0x200) {
        terminal_puts(&main_terminal, "EHCI: MMIO region too small\r\n");
        return false;
    }
    
    terminal_printf(&main_terminal, "EHCI: MMIO base = 0x%08x, size = %u\r\n", 
                   mmio_base, mmio_size);
    
    // Map MMIO region to virtual memory
    uint32_t mmio_pages = (mmio_size + 4095) / 4096;
    for (uint32_t i = 0; i < mmio_pages; i++) {
        mmu_map_page(mmio_base + (i * 4096), mmio_base + (i * 4096), 
                    PAGE_PRESENT | PAGE_RW | PAGE_CACHE_DISABLE);
    }
    
    // Allocate EHCI-specific data
    ehci_data_t* ehci = (ehci_data_t*)kernel_malloc(sizeof(ehci_data_t));
    if (!ehci) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate controller data\r\n");
        return false;
    }
    memset(ehci, 0, sizeof(ehci_data_t));
    
    ehci->cap_regs = (uint8_t*)mmio_base;
    ehci->cap_regs_phys = mmio_base;
    
    // Read capability registers
    ehci->cap_length = *((volatile uint8_t*)(ehci->cap_regs + EHCI_CAP_CAPLENGTH));
    uint16_t hci_version = *((volatile uint16_t*)(ehci->cap_regs + EHCI_CAP_HCIVERSION));
    uint32_t hcs_params = *((volatile uint32_t*)(ehci->cap_regs + EHCI_CAP_HCSPARAMS));
    
    ehci->num_ports = hcs_params & 0x0F;
    
    terminal_printf(&main_terminal, "EHCI: Version %x.%02x, %u ports, cap_length=%u\r\n",
                   (hci_version >> 8) & 0xFF, hci_version & 0xFF, 
                   ehci->num_ports, ehci->cap_length);
    
    // Calculate operational registers address
    ehci->op_regs = ehci->cap_regs + ehci->cap_length;
    ehci->op_regs_phys = mmio_base + ehci->cap_length;
    
    controller->regs = ehci;
    
    // Check if controller is running, stop it if so
    uint32_t usbcmd = EHCI_READ32(ehci, EHCI_OP_USBCMD);
    if (usbcmd & EHCI_CMD_RS) {
        terminal_puts(&main_terminal, "EHCI: Controller is running, stopping...\r\n");
        EHCI_WRITE32(ehci, EHCI_OP_USBCMD, usbcmd & ~EHCI_CMD_RS);
        
        // Wait for halt
        uint32_t timeout = 1000000;
        while (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) && timeout--) {
            __asm__ volatile("pause");
        }
        
        if (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED)) {
            terminal_puts(&main_terminal, "EHCI: Failed to halt controller\r\n");
            kernel_free(ehci);
            return false;
        }
    }
    
    // Reset controller
    terminal_puts(&main_terminal, "EHCI: Resetting controller...\r\n");
    EHCI_WRITE32(ehci, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    
    uint32_t timeout = 1000000;
    while ((EHCI_READ32(ehci, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET) && timeout--) {
        __asm__ volatile("pause");
    }
    
    if (EHCI_READ32(ehci, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET) {
        terminal_puts(&main_terminal, "EHCI: Reset timeout\r\n");
        kernel_free(ehci);
        return false;
    }
    
    terminal_puts(&main_terminal, "EHCI: Reset complete\r\n");
    
    uint32_t hcc_params = *((volatile uint32_t*)(ehci->cap_regs + EHCI_CAP_HCCPARAMS));
    uint8_t frame_interval = (hcc_params & (1 << 1)) ? 8 : 1; // 8 para 64KB, 1 para 4KB
    EHCI_WRITE32(ehci, EHCI_OP_FRINDEX, 0);

    // Allocate frame list (1024 entries, 4KB aligned)
    ehci->framelist_buffer = dma_alloc_buffer(EHCI_FRAMELIST_SIZE * 4, 4096);
    if (!ehci->framelist_buffer) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate frame list\r\n");
        kernel_free(ehci);
        return false;
    }
    
    ehci->framelist = (uint32_t*)ehci->framelist_buffer->virtual_address;
    memset(ehci->framelist, 0, EHCI_FRAMELIST_SIZE * 4);
    
    // Mark all entries as invalid
    for (int i = 0; i < EHCI_FRAMELIST_SIZE; i++) {
        ehci->framelist[i] = EHCI_QH_TERMINATE;
    }
    
    // Allocate QH pool
    ehci->qh_pool_buffer = dma_alloc_buffer(sizeof(ehci_qh_t) * EHCI_MAX_QH, 32);
    if (!ehci->qh_pool_buffer) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate QH pool\r\n");
        dma_free_buffer(ehci->framelist_buffer);
        kernel_free(ehci);
        return false;
    }
    
    ehci->qh_pool = (ehci_qh_t*)ehci->qh_pool_buffer->virtual_address;
    memset(ehci->qh_pool, 0, sizeof(ehci_qh_t) * EHCI_MAX_QH);
    memset(ehci->qh_used, 0, sizeof(ehci->qh_used));
    
    // Allocate qTD pool
    ehci->qtd_pool_buffer = dma_alloc_buffer(sizeof(ehci_qtd_t) * EHCI_MAX_QTD, 32);
    if (!ehci->qtd_pool_buffer) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate qTD pool\r\n");
        dma_free_buffer(ehci->qh_pool_buffer);
        dma_free_buffer(ehci->framelist_buffer);
        kernel_free(ehci);
        return false;
    }
    
    ehci->qtd_pool = (ehci_qtd_t*)ehci->qtd_pool_buffer->virtual_address;
    memset(ehci->qtd_pool, 0, sizeof(ehci_qtd_t) * EHCI_MAX_QTD);
    memset(ehci->qtd_used, 0, sizeof(ehci->qtd_used));
    
    // Create async schedule QH (dummy head)
    ehci->async_qh_buffer = dma_alloc_buffer(sizeof(ehci_qh_t), 32);
    if (!ehci->async_qh_buffer) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate async QH\r\n");
        dma_free_buffer(ehci->qtd_pool_buffer);
        dma_free_buffer(ehci->qh_pool_buffer);
        dma_free_buffer(ehci->framelist_buffer);
        kernel_free(ehci);
        return false;
    }
    
    ehci->async_qh = (ehci_qh_t*)ehci->async_qh_buffer->virtual_address;
    memset(ehci->async_qh, 0, sizeof(ehci_qh_t));
    
    // Configure async QH as circular list pointing to itself
    uint32_t async_qh_phys = ehci->async_qh_buffer->physical_address;
    ehci->async_qh->qh_link_ptr = async_qh_phys | EHCI_QH_TYPE_QH;
    ehci->async_qh->ep_characteristics = (1 << 15); // H bit (head of reclamation list)
    ehci->async_qh->next_qtd_ptr = EHCI_QH_TERMINATE;
    ehci->async_qh->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
    ehci->async_qh->token = (1 << 6); // Halted
    
    // Set periodic list base
    EHCI_WRITE32(ehci, EHCI_OP_PERIODICLISTBASE, ehci->framelist_buffer->physical_address & 0xFFFFF000);
    
    // Set async list address
    EHCI_WRITE32(ehci, EHCI_OP_ASYNCLISTADDR, async_qh_phys);
    
    // Clear USB interrupts
    EHCI_WRITE32(ehci, EHCI_OP_USBSTS, 0x3F);
    
    // Enable interrupts
    EHCI_WRITE32(ehci, EHCI_OP_USBINTR, 
                EHCI_STS_USBINT | EHCI_STS_USBERRINT | EHCI_STS_PCD | EHCI_STS_IAA);
    
    // Turn on ports and set routing to EHCI
    EHCI_WRITE32(ehci, EHCI_OP_CONFIGFLAG, 1);
    
    // Wait for routing
    for (volatile int i = 0; i < 100000; i++);
    
    // Start controller
    EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                EHCI_CMD_RS | EHCI_CMD_ASE | (8 << 16)); // Run, async enable, frame list size 1024
    
    // Wait for controller to start
    timeout = 1000000;
    while ((EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) && timeout--) {
        __asm__ volatile("pause");
    }
    
    if (EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) {
        terminal_puts(&main_terminal, "EHCI: Failed to start controller\r\n");
        return false;
    }
    
    controller->initialized = true;
    
    terminal_puts(&main_terminal, "EHCI: Controller started successfully\r\n");
    
    // Power on and detect ports
    for (uint8_t i = 0; i < ehci->num_ports; i++) {
        uint32_t port_offset = EHCI_OP_PORTSC_BASE + (i * 4);
        uint32_t portsc = EHCI_READ32(ehci, port_offset);
        
        // Power on port if needed
        if (!(portsc & EHCI_PORT_PP)) {
            EHCI_WRITE32(ehci, port_offset, portsc | EHCI_PORT_PP);
        }
    }
    
    // Wait for ports to power up
    for (volatile int i = 0; i < 1000000; i++);
    
    // Detect devices
    ehci_detect_ports(controller);
    
    return true;
}

void ehci_cleanup(usb_controller_t* controller) {
    if (!controller->regs) return;
    
    ehci_data_t* ehci = (ehci_data_t*)controller->regs;
    
    // Stop controller
    uint32_t usbcmd = EHCI_READ32(ehci, EHCI_OP_USBCMD);
    EHCI_WRITE32(ehci, EHCI_OP_USBCMD, usbcmd & ~EHCI_CMD_RS);
    
    // Wait for halt
    uint32_t timeout = 1000000;
    while (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) && timeout--) {
        __asm__ volatile("pause");
    }
    
    // Free DMA buffers
    if (ehci->qtd_pool_buffer) dma_free_buffer(ehci->qtd_pool_buffer);
    if (ehci->qh_pool_buffer) dma_free_buffer(ehci->qh_pool_buffer);
    if (ehci->async_qh_buffer) dma_free_buffer(ehci->async_qh_buffer);
    if (ehci->framelist_buffer) dma_free_buffer(ehci->framelist_buffer);
    
    kernel_free(ehci);
    controller->regs = NULL;
}

bool ehci_detect_ports(usb_controller_t* controller) {
    ehci_data_t* ehci = (ehci_data_t*)controller->regs;
    
    terminal_puts(&main_terminal, "EHCI: Detecting ports...\r\n");
    
    for (uint8_t port = 0; port < ehci->num_ports; port++) {
        uint32_t port_offset = EHCI_OP_PORTSC_BASE + (port * 4);
        uint32_t portsc = EHCI_READ32(ehci, port_offset);
        
        terminal_printf(&main_terminal, "EHCI: Port %u status = 0x%08x\r\n", port, portsc);
        
        if (!(portsc & EHCI_PORT_CCS)) {
            terminal_printf(&main_terminal, "EHCI: Port %u - no device connected\r\n", port);
            continue;
        }
        
        // Check if port is owned by companion (UHCI/OHCI)
        if (portsc & EHCI_PORT_OWNER) {
            terminal_printf(&main_terminal, "EHCI: Port %u owned by companion controller\r\n", port);
            continue;
        }
        
        // Check line status
        uint32_t line_status = (portsc & EHCI_PORT_LS_MASK) >> 10;
        
        if (line_status == 0x01) {
            // Low-speed or full-speed device
            terminal_printf(&main_terminal, "EHCI: Port %u has low/full-speed device, routing to companion\r\n", port);
            EHCI_WRITE32(ehci, port_offset, portsc | EHCI_PORT_OWNER);
            continue;
        }
        
        terminal_printf(&main_terminal, "EHCI: High-speed device detected on port %u\r\n", port);
        
        // Reset port
        if (ehci_reset_port(controller, port)) {
            // Enumerate device
            usb_enumerate_device(controller, port);
        }
    }
    
    return true;
}

bool ehci_reset_port(usb_controller_t* controller, uint8_t port) {
    ehci_data_t* ehci = (ehci_data_t*)controller->regs;
    uint32_t port_reg = EHCI_OP_PORTSC_BASE + (port * 4);
    
    // Leer estado inicial
    uint32_t port_status = EHCI_READ32(ehci, port_reg);
    terminal_printf(&main_terminal, "EHCI: Port %u initial status: 0x%08x\r\n", port, port_status);
    
    // Limpiar bits de cambio
    EHCI_WRITE32(ehci, port_reg, port_status | (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC));
    
    // Esperar a que el puerto esté estable
    for (volatile int i = 0; i < 10000; i++);
    
    // Iniciar reset
    EHCI_WRITE32(ehci, port_reg, port_status | EHCI_PORT_RESET);
    
    // Esperar 50ms (especificación USB)
    for (volatile int i = 0; i < 5000000; i++);
    
    // Limpiar reset
    port_status = EHCI_READ32(ehci, port_reg);
    EHCI_WRITE32(ehci, port_reg, port_status & ~EHCI_PORT_RESET);
    
    // Esperar a que el reset se limpie
    uint32_t reset_timeout = 1000000;
    while ((EHCI_READ32(ehci, port_reg) & EHCI_PORT_RESET) && reset_timeout--) {
        __asm__ volatile("pause");
    }
    
    if (EHCI_READ32(ehci, port_reg) & EHCI_PORT_RESET) {
        terminal_printf(&main_terminal, "EHCI: Port %u reset clear timeout\r\n", port);
        return false;
    }
    
    // Esperar a que el puerto se habilite
    for (volatile int i = 0; i < 1000000; i++);
    
    port_status = EHCI_READ32(ehci, port_reg);
    //terminal_printf(&main_terminal, "EHCI: Port %u final status: 0x%08x\r\n", port, port_status);
    
    if (!(port_status & EHCI_PORT_CCS)) {
        terminal_printf(&main_terminal, "EHCI: Port %u no device connected after reset\r\n", port);
        return false;
    }
    
    if (!(port_status & EHCI_PORT_PE)) {
        terminal_printf(&main_terminal, "EHCI: Port %u not enabled after reset\r\n", port);
        return false;
    }
    
    return true;
}

// ========================================================================
// MEMORY MANAGEMENT
// ========================================================================

ehci_qh_t* ehci_alloc_qh(ehci_data_t* ehci) {
    for (int i = 0; i < EHCI_MAX_QH; i++) {
        if (!ehci->qh_used[i]) {
            ehci->qh_used[i] = true;
            ehci_qh_t* qh = &ehci->qh_pool[i];
            memset(qh, 0, sizeof(ehci_qh_t));
            return qh;
        }
    }
    return NULL;
}

void ehci_free_qh(ehci_data_t* ehci, ehci_qh_t* qh) {
    int index = qh - ehci->qh_pool;
    if (index >= 0 && index < EHCI_MAX_QH) {
        ehci->qh_used[index] = false;
    }
}

ehci_qtd_t* ehci_alloc_qtd(ehci_data_t* ehci) {
    for (int i = 0; i < EHCI_MAX_QTD; i++) {
        if (!ehci->qtd_used[i]) {
            ehci->qtd_used[i] = true;
            ehci_qtd_t* qtd = &ehci->qtd_pool[i];
            memset(qtd, 0, sizeof(ehci_qtd_t));
            return qtd;
        }
    }
    return NULL;
}

void ehci_free_qtd(ehci_data_t* ehci, ehci_qtd_t* qtd) {
    int index = qtd - ehci->qtd_pool;
    if (index >= 0 && index < EHCI_MAX_QTD) {
        ehci->qtd_used[index] = false;
    }
}

bool ehci_wait_for_qtd(ehci_qtd_t* qtd, uint32_t timeout_ms) {
    uint32_t start_time = ticks_since_boot;
    uint32_t last_print = 0;
    
    while (qtd->token & EHCI_QTD_STATUS_ACTIVE) {
        uint32_t current_time = ticks_since_boot - start_time;
        
        // Print cada 2 segundos para debugging
        if (current_time - last_print > 200) {
            terminal_printf(&main_terminal, "EHCI: Waiting for qTD... %u ms, token=0x%08x\n", 
                           current_time * 10, qtd->token);
            last_print = current_time;
        }
        
        // Verificar timeout
        if (current_time > (timeout_ms / 10)) {
            terminal_printf(&main_terminal, "EHCI: qTD timeout after %u ms\n", timeout_ms);
            
            // Debug detallado del estado del qTD
            uint32_t token = qtd->token;
            //terminal_printf(&main_terminal, "EHCI: qTD final token: 0x%08x\n", token);
            //terminal_printf(&main_terminal, "EHCI: Status bits - Active:%d Halted:%d DataBuf:%d Babble:%d XactErr:%d MissedMF:%d\n",
            //               (token >> 7) & 1, (token >> 6) & 1,
            //               (token >> 5) & 1, (token >> 4) & 1,
            //               (token >> 3) & 1, (token >> 2) & 1);
            //terminal_printf(&main_terminal, "EHCI: PID: %d, CERR: %d, Bytes: %d, Toggle: %d\n",
            //               (token >> 8) & 3, (token >> 10) & 3,
            //               (token >> 16) & 0x7FFF, (token >> 0) & 1);
            return false;
        }
        
        // Verificar errores
        if (qtd->token & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_DBERR | 
                          EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_XACTERR)) {
            terminal_printf(&main_terminal, "EHCI: qTD error detected: 0x%08x\n", qtd->token);
            return false;
        }
        
        __asm__ volatile("pause");
    }
    
    //terminal_printf(&main_terminal, "EHCI: qTD completed, final token: 0x%08x\n", qtd->token);
    return true;
}

// ========================================================================
// TRANSFER FUNCTIONS
// ========================================================================

bool ehci_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                           void* data, uint16_t length) {
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    ehci_data_t* ehci = (ehci_data_t*)controller->regs;
    
    if (!ehci) {
        terminal_puts(&main_terminal, "EHCI: Controller not initialized\r\n");
        return false;
    }
    
    uint32_t usbsts = EHCI_READ32(ehci, EHCI_OP_USBSTS);
    if (usbsts & EHCI_STS_HCHALTED) {
        terminal_puts(&main_terminal, "EHCI: Controller halted, cannot transfer\r\n");
        goto cleanup_error;
    }
    
    if (usbsts & (EHCI_STS_USBERRINT | EHCI_STS_HSE)) {
        terminal_printf(&main_terminal, "EHCI: Controller error, status=0x%08x\r\n", usbsts);
        // Limpiar bits de error
        EHCI_WRITE32(ehci, EHCI_OP_USBSTS, usbsts & (EHCI_STS_USBERRINT | EHCI_STS_HSE));
    }

    // Determinar max packet size
    uint32_t max_packet = device->descriptor.bMaxPacketSize0;
    if (max_packet == 0) {
        max_packet = 64;  // Default para high-speed
    }
    
    //terminal_printf(&main_terminal, "EHCI: Control transfer, addr=%u, length=%u, max_packet=%u\r\n",
    //               device->address, length, max_packet);
    
    // Allocate QH and qTDs
    ehci_qh_t* qh = ehci_alloc_qh(ehci);
    ehci_qtd_t* setup_qtd = ehci_alloc_qtd(ehci);
    ehci_qtd_t* status_qtd = ehci_alloc_qtd(ehci);
    ehci_qtd_t* data_qtd = NULL;
    
    if (!qh || !setup_qtd || !status_qtd) {
        terminal_puts(&main_terminal, "EHCI: Failed to allocate QH or qTDs\r\n");
        if (qh) ehci_free_qh(ehci, qh);
        if (setup_qtd) ehci_free_qtd(ehci, setup_qtd);
        if (status_qtd) ehci_free_qtd(ehci, status_qtd);
        return false;
    }
    
    if (length > 0) {
        data_qtd = ehci_alloc_qtd(ehci);
        if (!data_qtd) {
            terminal_puts(&main_terminal, "EHCI: Failed to allocate data qTD\r\n");
            ehci_free_qh(ehci, qh);
            ehci_free_qtd(ehci, setup_qtd);
            ehci_free_qtd(ehci, status_qtd);
            return false;
        }
    }
    
    // Get physical addresses
    uint32_t qh_phys = ehci->qh_pool_buffer->physical_address + 
                      ((uint8_t*)qh - (uint8_t*)ehci->qh_pool);
    uint32_t setup_qtd_phys = ehci->qtd_pool_buffer->physical_address + 
                             ((uint8_t*)setup_qtd - (uint8_t*)ehci->qtd_pool);
    uint32_t status_qtd_phys = ehci->qtd_pool_buffer->physical_address + 
                              ((uint8_t*)status_qtd - (uint8_t*)ehci->qtd_pool);
    uint32_t data_qtd_phys = data_qtd ? ehci->qtd_pool_buffer->physical_address + 
                            ((uint8_t*)data_qtd - (uint8_t*)ehci->qtd_pool) : 0;
    
    uint32_t setup_phys = mmu_virtual_to_physical((uint32_t)setup);
    uint32_t data_phys = data ? mmu_virtual_to_physical((uint32_t)data) : 0;
    
    // Verificar mapeos DMA
    if (!setup_phys) {
        terminal_puts(&main_terminal, "EHCI: Invalid setup packet physical address\r\n");
        goto cleanup_error;
    }
    
    if (data && !data_phys) {
        terminal_puts(&main_terminal, "EHCI: Invalid data buffer physical address\r\n");
        goto cleanup_error;
    }
    
    //terminal_printf(&main_terminal, "EHCI: Physical addresses - setup: 0x%08x, data: 0x%08x\r\n",
    //               setup_phys, data_phys);
    
    // ====================================================================
    // CONSTRUIR qTDs CORREGIDOS
    // ====================================================================
    
    // 1. SETUP qTD (siempre data toggle = 0)
    setup_qtd->next_qtd_ptr = data_qtd ? data_qtd_phys : status_qtd_phys;
    setup_qtd->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
    uint32_t toggle = 0;  // Start with DT=0 for setup
    setup_qtd->token = (8 << 16) |  // Size 8 (setup packet)
                       (EHCI_QTD_PID_SETUP << 8) |
                       (3 << 10) |  // CERR=3
                       EHCI_QTD_STATUS_ACTIVE |
                       EHCI_QTD_IOC |
                       (toggle << 31);  // DT=0
    
    
    // Configurar buffer pointers para SETUP (8 bytes)
    setup_qtd->buffer_ptr[0] = setup_phys;
    // Los siguientes buffer pointers deben ser 0 si no se usan
    for (int i = 1; i < 5; i++) {
        setup_qtd->buffer_ptr[i] = 0;
    }
    
    //terminal_printf(&main_terminal, "EHCI: SETUP qTD - token=0x%08x, buffer0=0x%08x\r\n",
    //               setup_qtd->token, setup_qtd->buffer_ptr[0]);
    //
    //terminal_printf(&main_terminal, "EHCI: SETUP qTD - token=0x%08x, buffer0=0x%08x\r\n",
    //               setup_qtd->token, setup_qtd->buffer_ptr[0]);
    
    // 2. DATA qTD (si es necesario)
    if (data_qtd && length > 0) {
        toggle = 1;
        data_qtd->next_qtd_ptr = status_qtd_phys;
        data_qtd->alt_next_qtd_ptr = EHCI_QH_TERMINATE;

        // Determinar dirección de transferencia
        uint8_t data_pid = (setup->bmRequestType & 0x80) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
        data_qtd->token = (length << 16) |
                                  (data_pid << 8) |
                                  (3 << 10) |
                                  EHCI_QTD_STATUS_ACTIVE |
                                  EHCI_QTD_IOC |
                                  (toggle << 31);  // DT=1

        // Configurar buffer pointers para DATA - MANERA CORRECTA
        data_qtd->buffer_ptr[0] = data_phys;
        
        // Para 18 bytes, solo necesitamos el primer buffer pointer
        // Los otros deben ser 0
        for (int i = 1; i < 5; i++) {
            data_qtd->buffer_ptr[i] = 0;
        }

        //terminal_printf(&main_terminal, "EHCI: DATA qTD - token=0x%08x, length=%u, pid=%s, buffer0=0x%08x\r\n",
        //               data_qtd->token, length, data_pid == EHCI_QTD_PID_IN ? "IN" : "OUT", data_qtd->buffer_ptr[0]);
    }
    
    // 3. STATUS qTD (siempre data toggle = 1)
    status_qtd->next_qtd_ptr = EHCI_QH_TERMINATE;
    status_qtd->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
    
    // Para STATUS phase:
    // - Si hubo DATA phase IN, STATUS es OUT
    // - Si hubo DATA phase OUT, STATUS es IN  
    // - Si no hubo DATA phase, STATUS es IN
    uint8_t status_pid = (setup->bmRequestType & 0x80) ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
    toggle = 1;
    status_qtd->token = (0 << 16) |  // No data
                             (status_pid << 8) |
                             (3 << 10) |
                             EHCI_QTD_STATUS_ACTIVE |
                             EHCI_QTD_IOC |
                             (toggle << 31);  // DT=1
    
    //terminal_printf(&main_terminal, "EHCI: STATUS qTD - token=0x%08x, pid=%s (data_phase=%s)\r\n",
    //               status_qtd->token, 
    //               status_pid == EHCI_QTD_PID_IN ? "IN" : "OUT",
    //               length > 0 ? "yes" : "no");
    
    // Buffer pointers para STATUS (0 bytes)
    for (int i = 0; i < 5; i++) {
        status_qtd->buffer_ptr[i] = 0;
    }
    
    //terminal_printf(&main_terminal, "EHCI: STATUS qTD - token=0x%08x, pid=%s\r\n",
    //                   status_qtd->token, status_pid == EHCI_QTD_PID_IN ? "IN" : "OUT");
        
                       //terminal_printf(&main_terminal, "EHCI: SETUP qTD token=0x%08x (Active:%d, PID:%d, Toggle:%d)\n",
                   //setup_qtd->token, 
                   //(setup_qtd->token >> 7) & 1,
                   //(setup_qtd->token >> 8) & 3,
                   //(setup_qtd->token >> 31) & 1);

    if (data_qtd) {
        //terminal_printf(&main_terminal, "EHCI: DATA qTD token=0x%08x (Active:%d, PID:%d, Toggle:%d, Bytes:%d)\n",
                       //data_qtd->token,
                       //(data_qtd->token >> 7) & 1,
                       //(data_qtd->token >> 8) & 3, 
                       //(data_qtd->token >> 31) & 1,
                       //(data_qtd->token >> 16) & 0x7FFF);
    }

    //terminal_printf(&main_terminal, "EHCI: STATUS qTD token=0x%08x (Active:%d, PID:%d, Toggle:%d)\n",
                   //status_qtd->token,
                   //(status_qtd->token >> 7) & 1,
                   //(status_qtd->token >> 8) & 3,
                   //(status_qtd->token >> 31) & 1);
    // ====================================================================
    // CONFIGURAR QUEUE HEAD (QH)
    // ====================================================================
    
    qh->qh_link_ptr = EHCI_QH_TERMINATE;
    
    // Endpoint characteristics:
    // - Bits 31-16: Max Packet Size
    // - Bit 15: H (head of reclamation list) = 0
    // - Bit 14: DTC (Data Toggle Control) = 1 (use qTD toggle)
    // - Bits 13-12: EPS (Endpoint Speed) = 2 (high speed)
    // - Bits 11-8: Endpoint Number = 0 (control endpoint)
    // - Bits 7-0: Device Address
    qh->ep_characteristics = ((max_packet << 16) |  // Max packet size
                             (0 << 15) |            // H = 0
                             (1 << 14) |            // DTC = 0 (use QH toggle) - ¡IMPORTANTE!
                             (2 << 12) |            // EPS = 2 (high speed)
                             (0 << 8) |             // Endpoint 0
                             (device->address << 0));
        
    // Endpoint capabilities:
    // - Bit 31-30: Mult = 1
    // - Bits 23-16: Hub Address = 0
    // - Bits 15-8: Port Number = 0
    qh->ep_capabilities = ((1 << 30) |  // Mult = 1
                          (0 << 23) |   // Hub addr = 0  
                          (0 << 16));   // Port number = 0
    
    qh->next_qtd_ptr = setup_qtd_phys;  // Primer qTD en la lista
    qh->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
    qh->token = 0;  // Limpiar bit halted
    
    //terminal_printf(&main_terminal, "EHCI: QH - ep_char=0x%08x, next_qtd=0x%08x\r\n",
                   //qh->ep_characteristics, qh->next_qtd_ptr);
    
    // ====================================================================
    // EJECUTAR TRANSFERENCIA
    // ====================================================================
    
    // Enlazar QH al async schedule
    uint32_t async_qh_phys = ehci->async_qh_buffer->physical_address;
    qh->qh_link_ptr = ehci->async_qh->qh_link_ptr;
    ehci->async_qh->qh_link_ptr = qh_phys | EHCI_QH_TYPE_QH;
    
    //terminal_printf(&main_terminal, "EHCI: Linked QH to async schedule (phys=0x%08x)\r\n", qh_phys);
    
    // Ring doorbell para notificar al controlador
    EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                EHCI_READ32(ehci, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
    
    terminal_puts(&main_terminal, "EHCI: Doorbell rung, waiting for completion...\r\n");
    
    // Esperar por completación (con timeout extendido)
    bool result = ehci_wait_for_qtd(status_qtd, EHCI_TIMEOUT_MS);
    
    if (!result) {
        terminal_puts(&main_terminal, "EHCI: Transfer failed or timeout\r\n");
        
        // Debug: ver estado de todos los qTDs
        //terminal_printf(&main_terminal, "EHCI: SETUP qTD token=0x%08x\r\n", setup_qtd->token);
        if (data_qtd) {
            //terminal_printf(&main_terminal, "EHCI: DATA qTD token=0x%08x\r\n", data_qtd->token);
        }
        //terminal_printf(&main_terminal, "EHCI: STATUS qTD token=0x%08x\r\n", status_qtd->token);
    } else {
        device->ep_toggles[0] = 0;   // EP0 OUT
        device->ep_toggles[16] = 0;  // EP0 IN
        terminal_puts(&main_terminal, "EHCI: Transfer completed successfully\r\n");
    }
    
    // ====================================================================
    // LIMPIAR
    // ====================================================================
    
    // Desenlazar QH del async schedule
    ehci->async_qh->qh_link_ptr = qh->qh_link_ptr;
    
    // Ring doorbell nuevamente para actualizar
    EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                EHCI_READ32(ehci, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
    
    // Esperar por IAA
    uint32_t timeout = 1000000;
    while (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_IAA) && timeout--) {
        __asm__ volatile("pause");
    }
    
    if (!timeout) {
        terminal_puts(&main_terminal, "EHCI: IAA timeout after unlinking\r\n");
    } else {
        EHCI_WRITE32(ehci, EHCI_OP_USBSTS, EHCI_STS_IAA);
    }
    
cleanup:
    // Liberar recursos
    ehci_free_qh(ehci, qh);
    ehci_free_qtd(ehci, setup_qtd);
    ehci_free_qtd(ehci, status_qtd);
    if (data_qtd) ehci_free_qtd(ehci, data_qtd);
    
    return result;

cleanup_error:
    // Cleanup en caso de error temprano
    if (qh) ehci_free_qh(ehci, qh);
    if (setup_qtd) ehci_free_qtd(ehci, setup_qtd);
    if (status_qtd) ehci_free_qtd(ehci, status_qtd);
    if (data_qtd) ehci_free_qtd(ehci, data_qtd);
    return false;
}

bool ehci_bulk_transfer(usb_device_t* device, uint8_t endpoint,
                        void* data, uint32_t length, bool is_in) {
    usb_controller_t* controller = &usb_controllers[device->controller_id];
    ehci_data_t* ehci = (ehci_data_t*)controller->regs;
    
    if (!ehci) return false;
    
    // CAMBIO: Permitir length == 0 para ciertos casos
    if (length == 0) {
        terminal_puts(&main_terminal, "EHCI: Zero-length transfer requested\n");
        return true;
    }
    
    uint32_t usbsts = EHCI_READ32(ehci, EHCI_OP_USBSTS);
    if (usbsts & EHCI_STS_HCHALTED) {
        terminal_puts(&main_terminal, "EHCI: Controller halted\n");
        return false;
    }
    
    if (usbsts & (EHCI_STS_USBERRINT | EHCI_STS_HSE)) {
        EHCI_WRITE32(ehci, EHCI_OP_USBSTS, usbsts);
    }
    
    uint8_t ep_num = endpoint & 0x0F;
    uint8_t toggle_idx = ep_num + (is_in ? 16 : 0);
    uint32_t max_packet = 512;
    uint32_t transferred = 0;

    // CAMBIO CRÍTICO: Para transferencias pequeñas (< max_packet), hacer una sola operación
    // Esto evita problemas con CSW y otros short packets
    bool is_short_transfer = (length <= max_packet);

    while (transferred < length) {
        uint32_t transfer_size = length - transferred;
        
        // Para short transfers, transferir todo de una vez
        if (!is_short_transfer && transfer_size > (16 * 1024)) {
            transfer_size = 16 * 1024;
        }
        
        ehci_qh_t* qh = ehci_alloc_qh(ehci);
        ehci_qtd_t* qtd = ehci_alloc_qtd(ehci);
        
        if (!qh || !qtd) {
            if (qh) ehci_free_qh(ehci, qh);
            if (qtd) ehci_free_qtd(ehci, qtd);
            return false;
        }
        
        uint32_t qh_phys = ehci->qh_pool_buffer->physical_address + 
                          ((uint8_t*)qh - (uint8_t*)ehci->qh_pool);
        uint32_t qtd_phys = ehci->qtd_pool_buffer->physical_address + 
                           ((uint8_t*)qtd - (uint8_t*)ehci->qtd_pool);
        uint32_t data_phys = mmu_virtual_to_physical((uint32_t)((uint8_t*)data + transferred));
        
        if (!data_phys) {
            terminal_printf(&main_terminal, "EHCI: Invalid buffer address\n");
            ehci_free_qh(ehci, qh);
            ehci_free_qtd(ehci, qtd);
            return false;
        }
        
        uint8_t toggle = device->ep_toggles[toggle_idx];
        uint8_t pid = is_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
        
        // Build qTD
        qtd->next_qtd_ptr = EHCI_QH_TERMINATE;
        qtd->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
        qtd->token = (EHCI_QTD_STATUS_ACTIVE |
                      EHCI_QTD_IOC |
                      (3 << 10) |
                      (transfer_size << 16) |
                      (pid << 8) |
                      (toggle << 31));
        
        qtd->buffer_ptr[0] = data_phys;
        for (int i = 1; i < 5 && transfer_size > (i * 4096); i++) {
            qtd->buffer_ptr[i] = (data_phys + (i * 4096)) & ~0xFFF;
        }
        
        // Configure QH
        qh->qh_link_ptr = EHCI_QH_TERMINATE;
        qh->ep_characteristics = ((max_packet << 16) |
                                 (0 << 15) |
                                 (1 << 14) |
                                 (2 << 12) |
                                 (ep_num << 8) |
                                 (device->address << 0));
        qh->ep_capabilities = (1 << 30);
        qh->next_qtd_ptr = qtd_phys;
        qh->alt_next_qtd_ptr = EHCI_QH_TERMINATE;
        qh->token = 0;
        
        // Link into async schedule
        qh->qh_link_ptr = ehci->async_qh->qh_link_ptr;
        ehci->async_qh->qh_link_ptr = qh_phys | EHCI_QH_TYPE_QH;
        
        EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                    EHCI_READ32(ehci, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
        
        if (!ehci_wait_for_qtd(qtd, 10000)) {
            terminal_printf(&main_terminal, "EHCI: Transfer timeout - token=0x%08x\n", qtd->token);
            
            ehci->async_qh->qh_link_ptr = qh->qh_link_ptr;
            EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                        EHCI_READ32(ehci, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
            
            uint32_t timeout = 10000000;
            while (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_IAA) && timeout--);
            if (timeout) EHCI_WRITE32(ehci, EHCI_OP_USBSTS, EHCI_STS_IAA);
            
            ehci_free_qh(ehci, qh);
            ehci_free_qtd(ehci, qtd);
            return false;
        }
        
        // CAMBIO CRÍTICO: Calcular bytes transferidos correctamente
        uint32_t residue = (qtd->token >> 16) & 0x7FFF;
        uint32_t bytes_transferred = transfer_size - residue;
        
        //terminal_printf(&main_terminal, "EHCI: Transferred %u bytes (requested %u, residue %u)\n",
        //               bytes_transferred, transfer_size, residue);
        
        // Update toggle correctamente
        if (bytes_transferred > 0) {
            uint32_t packets_sent = (bytes_transferred + max_packet - 1) / max_packet;
            device->ep_toggles[toggle_idx] = toggle ^ (packets_sent & 1);
        }
        
        // Unlink QH
        ehci->async_qh->qh_link_ptr = qh->qh_link_ptr;
        EHCI_WRITE32(ehci, EHCI_OP_USBCMD, 
                    EHCI_READ32(ehci, EHCI_OP_USBCMD) | EHCI_CMD_IAAD);
        
        uint32_t timeout = 10000000;
        while (!(EHCI_READ32(ehci, EHCI_OP_USBSTS) & EHCI_STS_IAA) && timeout--);
        if (timeout) EHCI_WRITE32(ehci, EHCI_OP_USBSTS, EHCI_STS_IAA);
        
        ehci_free_qh(ehci, qh);
        ehci_free_qtd(ehci, qtd);
        
        transferred += bytes_transferred;
        
        // CAMBIO: Solo terminar si realmente no hay más datos
        // Para short transfers, el residue debe ser 0 si todo se transfirió
        if (bytes_transferred == 0) {
            terminal_puts(&main_terminal, "EHCI: Zero bytes transferred, stopping\n");
            break;
        }
        
        // Si transferimos menos de lo solicitado, terminamos
        if (bytes_transferred < transfer_size) {
            terminal_printf(&main_terminal, "EHCI: Short packet (%u < %u)\n", 
                           bytes_transferred, transfer_size);
            break;
        }
    }
    
    return transferred > 0;
}