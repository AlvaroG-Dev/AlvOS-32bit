#include "usb_mass_storage.h"
#include "usb_core.h"
#include "terminal.h"
#include "kernel.h"
#include "string.h"

// Global state
usb_msc_device_t usb_msc_devices[USB_MSC_MAX_DEVICES];
uint8_t usb_msc_device_count = 0;

// Helper to swap endianness
static uint32_t swap_uint32(uint32_t val) {
    return ((val >> 24) & 0xFF) | 
           ((val >> 8) & 0xFF00) | 
           ((val << 8) & 0xFF0000) | 
           ((val << 24) & 0xFF000000);
}

static uint16_t swap_uint16(uint16_t val) {
    return (val >> 8) | (val << 8);
}

// Driver structure
// En usb_mass_storage.c, cambia el driver registration:
static usb_driver_t msc_driver = {
    .name = "USB Mass Storage",
    .class_code = USB_CLASS_MASS_STORAGE,
    .subclass = 0xFF,        // Cualquier subclase
    .protocol = 0xFF,        // Cualquier protocolo
    .probe = usb_msc_probe,
    .init = usb_msc_init,
    .cleanup = usb_msc_cleanup
};

void usb_msc_register_driver(void) {
    usb_register_driver(&msc_driver);
    memset(usb_msc_devices, 0, sizeof(usb_msc_devices));
}

bool usb_msc_probe(usb_device_t* device) {
    // Aceptar cualquier subclase y protocolo de almacenamiento masivo
    if (device->class_code != USB_CLASS_MASS_STORAGE) {
        return false;
    }
    
    // Aceptar diferentes subclases comunes
    switch (device->subclass) {
        case USB_MSC_SUBCLASS_SCSI:    // SCSI Transparent (0x06)
            terminal_puts(&main_terminal, "USB MSC: SCSI Transparent device\n");
            break;
        case 0x02:                     // ATAPI (CD/DVD)
            terminal_puts(&main_terminal, "USB MSC: ATAPI (CD/DVD) device\n");
            break;
        case 0x04:                     // Floppy
            terminal_puts(&main_terminal, "USB MSC: Floppy device\n");
            break;
        case 0x05:                     // SFF-8070i
            terminal_puts(&main_terminal, "USB MSC: SFF-8070i device\n");
            break;
        case 0x01:                     // RBC (Flash drives)
            terminal_puts(&main_terminal, "USB MSC: RBC (Flash) device\n");
            break;
        default:
            terminal_printf(&main_terminal, "USB MSC: Unknown subclass 0x%02x, trying anyway\n", device->subclass);
            break;
    }
    
    // Aceptar diferentes protocolos
    switch (device->protocol) {
        case USB_MSC_PROTOCOL_BBB:     // Bulk-Only (0x50)
            terminal_puts(&main_terminal, "USB MSC: Bulk-Only protocol\n");
            break;
        case 0x01:                     // SCSI
            terminal_puts(&main_terminal, "USB MSC: SCSI protocol\n");
            break;
        case 0x02:                     // ATAPI
            terminal_puts(&main_terminal, "USB MSC: ATAPI protocol\n");
            break;
        case 0x62:                     // USB Attached SCSI (UAS)
            terminal_puts(&main_terminal, "USB MSC: UAS protocol (not fully supported)\n");
            break;
        case 0x00:                     // Control/Bulk/Interrupt
            terminal_puts(&main_terminal, "USB MSC: CBI protocol\n");
            break;
        default:
            terminal_printf(&main_terminal, "USB MSC: Unknown protocol 0x%02x, trying anyway\n", device->protocol);
            break;
    }
    
    terminal_printf(&main_terminal, "USB MSC: Compatible device - Class:0x%02x Subclass:0x%02x Protocol:0x%02x\n",
                   device->class_code, device->subclass, device->protocol);
    
    return true;
}

bool usb_msc_init(usb_device_t* device) {
    terminal_puts(&main_terminal, "Initializing USB Mass Storage device...\r\n");
    
    if (usb_msc_device_count >= USB_MSC_MAX_DEVICES) {
        terminal_puts(&main_terminal, "USB MSC: Maximum device count reached\r\n");
        return false;
    }
    
    usb_msc_device_t* msc = &usb_msc_devices[usb_msc_device_count];
    memset(msc, 0, sizeof(usb_msc_device_t));
    
    msc->usb_device = device;
    msc->tag_counter = 1;
    
    // Get configuration descriptor
    uint8_t buffer[256];
    if (!usb_get_config_descriptor(device, 0, buffer, 255)) {
        terminal_puts(&main_terminal, "USB MSC: Failed to get config descriptor\r\n");
        return false;
    }
    
    usb_config_descriptor_t* config = (usb_config_descriptor_t*)buffer;
    
    // Parse interfaces and endpoints
    uint8_t* ptr = buffer + config->bLength;
    uint16_t remaining = config->wTotalLength - config->bLength;
    
    bool found_interface = false;
    uint8_t interface_num = 0;
    
    while (remaining > 0) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];
        
        if (type == USB_DESC_INTERFACE) {
            usb_interface_descriptor_t* intf = (usb_interface_descriptor_t*)ptr;
            
            if (intf->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                (intf->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI || 
                 intf->bInterfaceSubClass == 0x01 || intf->bInterfaceSubClass == 0x02 || 
                 intf->bInterfaceSubClass == 0x04 || intf->bInterfaceSubClass == 0x05) &&
                (intf->bInterfaceProtocol == USB_MSC_PROTOCOL_BBB || 
                 intf->bInterfaceProtocol == 0x00 || intf->bInterfaceProtocol == 0x01 || 
                 intf->bInterfaceProtocol == 0x02 || intf->bInterfaceProtocol == 0x62)) {
                
                interface_num = intf->bInterfaceNumber;
                found_interface = true;
                
                terminal_printf(&main_terminal, "USB MSC: Found interface %u\r\n", interface_num);
            }
        } else if (type == USB_DESC_ENDPOINT && found_interface) {
            usb_endpoint_descriptor_t* ep = (usb_endpoint_descriptor_t*)ptr;
            
            if (ep->bmAttributes == 0x02) {  // Bulk
                if (ep->bEndpointAddress & 0x80) {
                    msc->ep_in = ep->bEndpointAddress & 0x0F;
                } else {
                    msc->ep_out = ep->bEndpointAddress & 0x0F;
                }
            }
            
            if (msc->ep_in && msc->ep_out) {
                break;
            }
        }
        
        ptr += len;
        remaining -= len;
    }
    
    if (!found_interface || !msc->ep_in || !msc->ep_out) {
        terminal_puts(&main_terminal, "USB MSC: Failed to find endpoints\r\n");
        return false;
    }
    
    terminal_printf(&main_terminal, "USB MSC: Endpoints - IN=%u, OUT=%u\r\n", 
                   msc->ep_in, msc->ep_out);
    
    // Set configuration if not already set
    if (device->config_value == 0) {
        if (!usb_set_configuration(device, config->bConfigurationValue)) {
            terminal_puts(&main_terminal, "USB MSC: Failed to set configuration\r\n");
            return false;
        }
        device->config_value = config->bConfigurationValue;
    }
    
    // CRÍTICO: Después de SET_CONFIGURATION, resetear TODOS los toggles
    memset(device->ep_toggles, 0, sizeof(device->ep_toggles));
    terminal_puts(&main_terminal, "USB MSC: All endpoint toggles reset to 0\r\n");
    
    // Get max LUN
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA1,  // IN, class, interface
        .bRequest = 0xFE,       // GET_MAX_LUN
        .wValue = 0,
        .wIndex = interface_num,
        .wLength = 1
    };
    
    uint8_t max_lun = 0;
    if (usb_control_transfer(device, &setup, &max_lun, 1)) {
        msc->max_lun = max_lun;
    } else {
        msc->max_lun = 0;      // Assume single LUN
    }
    
    terminal_printf(&main_terminal, "USB MSC: Max LUN = %u\r\n", msc->max_lun);
    
    // Bulk-Only Mass Storage Reset
    usb_setup_packet_t reset_setup = {
        .bmRequestType = 0x21,  // OUT, class, interface
        .bRequest = 0xFF,       // MASS_STORAGE_RESET
        .wValue = 0,
        .wIndex = interface_num,
        .wLength = 0
    };
    
    if (!usb_control_transfer(device, &reset_setup, NULL, 0)) {
        terminal_puts(&main_terminal, "USB MSC: Bulk reset failed\r\n");
    } else {
        terminal_puts(&main_terminal, "USB MSC: Bulk reset successful\r\n");
    }
    
    // Wait después del reset
    for (volatile int i = 0; i < 1000000; i++);
    
    // Clear HALT en ambos endpoints y resetear sus toggles
    terminal_puts(&main_terminal, "USB MSC: Clearing endpoint halts...\r\n");
    usb_clear_endpoint_halt(device, msc->ep_in | 0x80);
    usb_clear_endpoint_halt(device, msc->ep_out);
    
    // CRÍTICO: Clear HALT resetea el toggle a 0
    uint8_t ep_in_idx = msc->ep_in + 16;  // IN direction
    uint8_t ep_out_idx = msc->ep_out;      // OUT direction
    device->ep_toggles[ep_in_idx] = 0;
    device->ep_toggles[ep_out_idx] = 0;
    
    terminal_printf(&main_terminal, "USB MSC: Toggle reset - IN[%u]=0, OUT[%u]=0\r\n",
                   ep_in_idx, ep_out_idx);
    
    // Wait después de clear halt
    for (volatile int i = 0; i < 1000000; i++);
    
    // Initialize device
    if (!usb_msc_inquiry(msc)) {
        terminal_puts(&main_terminal, "USB MSC: Inquiry failed\r\n");
        return false;
    }
    
    // Test unit ready (may need multiple attempts)
    bool ready = false;
    for (int i = 0; i < 10; i++) {
        if (usb_msc_test_unit_ready(msc)) {
            ready = true;
            break;
        }
        // Wait 200ms
        for (volatile int d = 0; d < 2000000; d++);
    }
    
    if (!ready) {
        terminal_puts(&main_terminal, "USB MSC: Device not ready\r\n");
        return false;
    }
    
    if (!usb_msc_read_capacity(msc)) {
        terminal_puts(&main_terminal, "USB MSC: Read capacity failed\r\n");
        return false;
    }
    
    terminal_printf(&main_terminal, "USB MSC: Capacity %u blocks x %u bytes\r\n",
                   msc->block_count, msc->block_size);
    
    msc->initialized = true;
    usb_msc_device_count++;
    
    return true;
}

void usb_msc_cleanup(usb_device_t* device) {
    if (!device->driver_data) return;
    
    usb_msc_device_t* msc = (usb_msc_device_t*)device->driver_data;
    msc->initialized = false;
    
    if (usb_msc_device_count > 0) {
        usb_msc_device_count--;
    }
}

static bool usb_msc_execute_command(usb_msc_device_t* msc, uint8_t* cbd, uint8_t cbd_len,
                                    void* data, uint32_t data_len, bool data_in) {
    // CRÍTICO: Alinear y usar memoria estática para evitar problemas de mapeo
    static usb_msc_cbw_t cbw __attribute__((aligned(64)));
    static uint8_t csw_buffer[64] __attribute__((aligned(64)));
    
    memset(&cbw, 0, sizeof(cbw));
    
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = ++msc->tag_counter;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = data_in ? CBW_FLAG_DATA_IN : CBW_FLAG_DATA_OUT;
    cbw.bCBWLUN = msc->max_lun;
    cbw.bCBWCBLength = cbd_len;
    memcpy(cbw.CBWCB, cbd, cbd_len);
    
    //terminal_printf(&main_terminal, "USB MSC: Command - Tag=%u, DataLen=%u, Dir=%s, CDB[0]=0x%02x\n",
    //               cbw.dCBWTag, data_len, data_in ? "IN" : "OUT", cbd[0]);
    
    // Enviar CBW (31 bytes exactos)
    if (!usb_bulk_transfer(msc->usb_device, msc->ep_out, &cbw, 31, false)) {
        terminal_puts(&main_terminal, "USB MSC: Failed to send CBW\n");
        return false;
    }
    
    //terminal_puts(&main_terminal, "USB MSC: CBW sent successfully\n");
    
    // Fase de datos
    if (data_len > 0) {
        //terminal_printf(&main_terminal, "USB MSC: Data phase - %u bytes, dir=%s\n", 
        //               data_len, data_in ? "IN" : "OUT");
        
        if (!usb_bulk_transfer(msc->usb_device, data_in ? msc->ep_in : msc->ep_out,
                              data, data_len, data_in)) {
            terminal_puts(&main_terminal, "USB MSC: Data phase failed\n");
            usb_clear_endpoint_halt(msc->usb_device, 
                                   data_in ? (msc->ep_in | 0x80) : msc->ep_out);
        } else {
            //terminal_puts(&main_terminal, "USB MSC: Data phase completed successfully\n");
        }
    }
    
    // Recibir CSW (13 bytes) con handling especial
    memset(csw_buffer, 0, sizeof(csw_buffer));
    
    //terminal_puts(&main_terminal, "USB MSC: Receiving CSW...\n");
    
    // CRÍTICO: Recibir exactamente 13 bytes en el buffer
    // Usar la función especializada que no termina en short packets prematuros
    uint32_t csw_received = 0;
    uint8_t* csw_ptr = csw_buffer;
    uint32_t csw_size = 13;
    
    // Recibir CSW en una sola transferencia
    if (!usb_bulk_transfer(msc->usb_device, msc->ep_in, csw_buffer, csw_size, true)) {
        terminal_puts(&main_terminal, "USB MSC: Failed to receive CSW\n");
        return false;
    }
    
    usb_msc_csw_t* csw = (usb_msc_csw_t*)csw_buffer;
    
    // Debug: dump bytes recibidos
    //terminal_puts(&main_terminal, "USB MSC: CSW bytes = ");
    //for (int i = 0; i < 13; i++) {
    //    terminal_printf(&main_terminal, "%02x ", csw_buffer[i]);
    //}
    //terminal_puts(&main_terminal, "\n");
    
    //terminal_printf(&main_terminal, "USB MSC: CSW - Signature=0x%08x, Tag=%u, Residue=%u, Status=%u\n",
    //               csw->dCSWSignature, csw->dCSWTag, csw->dCSWDataResidue, csw->bCSWStatus);
    
    if (csw->dCSWSignature != CSW_SIGNATURE) {
        terminal_printf(&main_terminal, "USB MSC: Invalid CSW signature: 0x%08x (expected 0x%08x)\n", 
                       csw->dCSWSignature, CSW_SIGNATURE);
        
        terminal_puts(&main_terminal, "USB MSC: Attempting bulk-only mass storage reset\n");
        usb_setup_packet_t reset_setup = {
            .bmRequestType = 0x21,
            .bRequest = 0xFF,
            .wValue = 0,
            .wIndex = 0,
            .wLength = 0
        };
        usb_control_transfer(msc->usb_device, &reset_setup, NULL, 0);
        
        usb_clear_endpoint_halt(msc->usb_device, msc->ep_in | 0x80);
        usb_clear_endpoint_halt(msc->usb_device, msc->ep_out);
        
        return false;
    }
    
    if (csw->dCSWTag != cbw.dCBWTag) {
        terminal_printf(&main_terminal, "USB MSC: CSW tag mismatch: expected %u, got %u\n",
                       cbw.dCBWTag, csw->dCSWTag);
        return false;
    }
    
    if (csw->bCSWStatus != CSW_STATUS_GOOD) {
        terminal_printf(&main_terminal, "USB MSC: Command failed, status=%u, residue=%u\n",
                       csw->bCSWStatus, csw->dCSWDataResidue);
        return false;
    }
    
    //terminal_puts(&main_terminal, "USB MSC: Command completed successfully\n");
    return true;
}

bool usb_msc_test_unit_ready(usb_msc_device_t* msc) {
    uint8_t cmd[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return usb_msc_execute_command(msc, cmd, 6, NULL, 0, false);
}

bool usb_msc_inquiry(usb_msc_device_t* msc) {
    uint8_t cmd[6] = {
        SCSI_CMD_INQUIRY,
        0, 0, 0, 36, 0
    };
    
    scsi_inquiry_response_t response;
    if (!usb_msc_execute_command(msc, cmd, 6, &response, sizeof(response), true)) {
        return false;
    }
    
    char vendor[9] = {0};
    char product[17] = {0};
    memcpy(vendor, response.vendor_id, 8);
    memcpy(product, response.product_id, 16);
    
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    
    terminal_printf(&main_terminal, "USB MSC: %s %s\r\n", vendor, product);
    
    return true;
}

bool usb_msc_read_capacity(usb_msc_device_t* msc) {
    uint8_t cmd[10] = {
        SCSI_CMD_READ_CAPACITY_10,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    
    scsi_read_capacity_response_t response;
    if (!usb_msc_execute_command(msc, cmd, 10, &response, sizeof(response), true)) {
        return false;
    }
    
    msc->block_count = swap_uint32(response.last_lba) + 1;
    msc->block_size = swap_uint32(response.block_size);
    
    return true;
}

bool usb_msc_read_blocks(usb_msc_device_t* msc, uint32_t lba, uint16_t count, void* buffer) {
    if (!msc->initialized) return false;
    
    uint8_t cmd[10] = {
        SCSI_CMD_READ_10,
        0,
        (lba >> 24) & 0xFF,
        (lba >> 16) & 0xFF,
        (lba >> 8) & 0xFF,
        lba & 0xFF,
        0,
        (count >> 8) & 0xFF,
        count & 0xFF,
        0
    };
    
    uint32_t data_len = count * msc->block_size;
    return usb_msc_execute_command(msc, cmd, 10, buffer, data_len, true);
}

bool usb_msc_write_blocks(usb_msc_device_t* msc, uint32_t lba, uint16_t count, const void* buffer) {
    if (!msc->initialized) return false;
    
    uint8_t cmd[10] = {
        SCSI_CMD_WRITE_10,
        0,
        (lba >> 24) & 0xFF,
        (lba >> 16) & 0xFF,
        (lba >> 8) & 0xFF,
        lba & 0xFF,
        0,
        (count >> 8) & 0xFF,
        count & 0xFF,
        0
    };
    
    uint32_t data_len = count * msc->block_size;
    return usb_msc_execute_command(msc, cmd, 10, (void*)buffer, data_len, false);
}

void usb_msc_list_devices(void) {
    terminal_puts(&main_terminal, "\r\n=== USB Mass Storage Devices ===\r\n");
    
    for (uint8_t i = 0; i < usb_msc_device_count; i++) {
        usb_msc_device_t* msc = &usb_msc_devices[i];
        
        if (!msc->initialized) continue;
        
        uint64_t size_mb = ((uint64_t)msc->block_count * msc->block_size) / (1024 * 1024);
        
        terminal_printf(&main_terminal, "Device %u: %u MB (%u blocks x %u bytes)\r\n",
                       i, (uint32_t)size_mb, msc->block_count, msc->block_size);
    }
    
    if (usb_msc_device_count == 0) {
        terminal_puts(&main_terminal, "No USB mass storage devices found\r\n");
    }
    
    terminal_puts(&main_terminal, "\r\n");
}

usb_msc_device_t* usb_msc_get_device(uint8_t index) {
    if (index >= usb_msc_device_count) return NULL;
    return &usb_msc_devices[index];
}

uint8_t usb_msc_get_device_count(void) {
    return usb_msc_device_count;
}