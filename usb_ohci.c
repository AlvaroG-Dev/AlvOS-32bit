#include "usb_ohci.h"
#include "dma.h"
#include "kernel.h"
#include "mmu.h"
#include "string.h"
#include "terminal.h"

// Macros for MMIO access (32-bit only as OHCI is memory mapped)
static inline uint32_t ohci_read(ohci_data_t *ohci, uint32_t reg) {
  return *(volatile uint32_t *)(ohci->mmio_base + reg);
}

static inline void ohci_write(ohci_data_t *ohci, uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)(ohci->mmio_base + reg) = val;
}

// ========================================================================
// MEMORY MANAGEMENT
// ========================================================================

static ohci_ed_t *ohci_alloc_ed(ohci_data_t *ohci) {
  for (int i = 0; i < 64; i++) {
    if (!ohci->ed_used[i]) {
      ohci->ed_used[i] = true;
      ohci_ed_t *ed = &ohci->ed_pool[i];
      memset(ed, 0, sizeof(ohci_ed_t));
      return ed;
    }
  }
  return NULL;
}

static void ohci_free_ed(ohci_data_t *ohci, ohci_ed_t *ed) {
  int idx = ed - ohci->ed_pool;
  if (idx >= 0 && idx < 64)
    ohci->ed_used[idx] = false;
}

static ohci_td_t *ohci_alloc_td(ohci_data_t *ohci) {
  for (int i = 0; i < 128; i++) {
    if (!ohci->td_used[i]) {
      ohci->td_used[i] = true;
      ohci_td_t *td = &ohci->td_pool[i];
      memset(td, 0, sizeof(ohci_td_t));
      return td;
    }
  }
  return NULL;
}

static void ohci_free_td(ohci_data_t *ohci, ohci_td_t *td) {
  int idx = td - ohci->td_pool;
  if (idx >= 0 && idx < 128)
    ohci->td_used[idx] = false;
}

static uint32_t ohci_virt_to_phys(ohci_data_t *ohci, void *ptr) {
  // Check if in ED pool
  if ((uint32_t)ptr >= (uint32_t)ohci->ed_pool &&
      (uint32_t)ptr < ((uint32_t)ohci->ed_pool + (sizeof(ohci_ed_t) * 64))) {
    uint32_t offset = (uint32_t)ptr - (uint32_t)ohci->ed_pool;
    dma_buffer_t *buf = (dma_buffer_t *)ohci->ed_pool_buffer;
    return buf->physical_address + offset;
  }

  // Check if in TD pool
  if ((uint32_t)ptr >= (uint32_t)ohci->td_pool &&
      (uint32_t)ptr < ((uint32_t)ohci->td_pool + (sizeof(ohci_td_t) * 128))) {
    uint32_t offset = (uint32_t)ptr - (uint32_t)ohci->td_pool;
    dma_buffer_t *buf = (dma_buffer_t *)ohci->td_pool_buffer;
    return buf->physical_address + offset;
  }

  return mmu_virtual_to_physical((uint32_t)ptr);
}

// ========================================================================
// INITIALIZATION
// ========================================================================

bool ohci_init(usb_controller_t *controller) {
  terminal_puts(&main_terminal, "Initializing OHCI controller...\r\n");

  pci_device_t *pci_dev = controller->pci_dev;
  pci_enable_bus_mastering(pci_dev);

  // Get MMIO base from BAR0
  if (!pci_dev->bars[0].is_valid ||
      pci_dev->bars[0].type != PCI_BAR_TYPE_MEMORY) {
    terminal_puts(&main_terminal, "OHCI: BAR0 not valid or not MEM type\r\n");
    return false;
  }

  // Map MMIO
  uint32_t mmio_phys = (uint32_t)pci_dev->bars[0].address;
  if (!mmu_map_page(mmio_phys, mmio_phys,
                    PAGE_PRESENT | PAGE_RW | PAGE_CACHE_DISABLE)) {
    // If mapping fails, try direct access (identity map might already exist)
    terminal_puts(&main_terminal,
                  "OHCI: MMIO Helper map failed (maybe already mapped)\r\n");
  }

  ohci_data_t *ohci = (ohci_data_t *)kernel_malloc(sizeof(ohci_data_t));
  if (!ohci)
    return false;
  memset(ohci, 0, sizeof(ohci_data_t));
  ohci->mmio_base = mmio_phys;
  controller->regs = ohci;

  terminal_printf(&main_terminal, "OHCI: MMIO Base = 0x%08x\r\n",
                  ohci->mmio_base);

  // Take ownership (SMI vs OS) - Simplified checks
  // We should check OpRegisters...

  // Save state?
  // Reset
  ohci_write(ohci, OHCI_REG_CONTROL, 0); // Turn off legacy
  // Wait
  for (volatile int i = 0; i < 10000; i++)
    ;

  ohci_write(ohci, OHCI_REG_CMDSTATUS, 1); // Host Controller Reset (HCR)
  // Wait for reset
  int timeout = 100000;
  while ((ohci_read(ohci, OHCI_REG_CMDSTATUS) & 1) && timeout--)
    __asm__("pause");
  if (timeout <= 0) {
    terminal_puts(&main_terminal, "OHCI: Reset timeout\r\n");
    return false;
  }

  // Init HCCA
  dma_buffer_t *hcca_buf = dma_alloc_buffer(sizeof(ohci_hcca_t), 256);
  if (!hcca_buf)
    return false;
  ohci->hcca_buffer = hcca_buf;
  ohci->hcca = (ohci_hcca_t *)hcca_buf->virtual_address;
  memset(ohci->hcca, 0, sizeof(ohci_hcca_t));

  ohci_write(ohci, OHCI_REG_HCCA, hcca_buf->physical_address);

  // Alloc ED pool
  dma_buffer_t *ed_buf = dma_alloc_buffer(sizeof(ohci_ed_t) * 64, 16);
  if (!ed_buf)
    return false;
  ohci->ed_pool_buffer = ed_buf;
  ohci->ed_pool = (ohci_ed_t *)ed_buf->virtual_address;
  memset(ohci->ed_pool, 0, sizeof(ohci_ed_t) * 64);

  // Alloc TD pool
  dma_buffer_t *td_buf = dma_alloc_buffer(sizeof(ohci_td_t) * 128, 16);
  if (!td_buf)
    return false;
  ohci->td_pool_buffer = td_buf;
  ohci->td_pool = (ohci_td_t *)td_buf->virtual_address;
  memset(ohci->td_pool, 0, sizeof(ohci_td_t) * 128);

  // Set Control Head
  ohci_ed_t *ctrl_ed = ohci_alloc_ed(ohci);
  ohci->control_head_ed = ctrl_ed;
  ohci_write(ohci, OHCI_REG_CONTROLHEAD, ohci_virt_to_phys(ohci, ctrl_ed));

  // Set Bulk Head
  ohci_ed_t *bulk_ed = ohci_alloc_ed(ohci);
  ohci->bulk_head_ed = bulk_ed;
  ohci_write(ohci, OHCI_REG_BULKHEAD, ohci_virt_to_phys(ohci, bulk_ed));

  // Set HcPeriodicStart to 0x2A2F (~90% of frame)
  ohci_write(ohci, OHCI_REG_PERIODSTART, 0x2A2F);

  // Enable Control, Bulk, Lists and Operational state
  uint32_t control = OHCI_CTRL_CLE | OHCI_CTRL_BLE |
                     OHCI_CTRL_HCFS_OPERATIONAL | OHCI_CTRL_PLE |
                     3; // CBSR=3 (4:1)
  ohci_write(ohci, OHCI_REG_CONTROL, control);

  controller->initialized = true;
  terminal_puts(&main_terminal, "OHCI: Controller operational\r\n");

  ohci_detect_ports(controller);

  return true;
}

void ohci_cleanup(usb_controller_t *controller) {
  if (!controller->regs)
    return;
  ohci_data_t *ohci = (ohci_data_t *)controller->regs;

  // Reset controller
  ohci_write(ohci, OHCI_REG_CONTROL, OHCI_CTRL_HCFS_RESET);

  // Free buffers
  if (ohci->hcca_buffer)
    dma_free_buffer((dma_buffer_t *)ohci->hcca_buffer);
  if (ohci->ed_pool_buffer)
    dma_free_buffer((dma_buffer_t *)ohci->ed_pool_buffer);
  if (ohci->td_pool_buffer)
    dma_free_buffer((dma_buffer_t *)ohci->td_pool_buffer);

  kernel_free(ohci);
  controller->regs = NULL;
}

bool ohci_detect_ports(usb_controller_t *controller) {
  ohci_data_t *ohci = (ohci_data_t *)controller->regs;

  // Get number of ports from RhDescriptorA
  uint32_t desc_a = ohci_read(ohci, OHCI_REG_RHDESCRIPTORA);
  uint32_t num_ports = desc_a & 0xFF;

  terminal_printf(&main_terminal, "OHCI: Root Hub has %u ports\r\n", num_ports);

  for (uint32_t i = 0; i < num_ports; i++) {
    uint32_t status = ohci_read(ohci, OHCI_REG_RHPORTSTATUS + (i * 4));

    // Bit 0 = Connected
    if (status & 1) {
      terminal_printf(&main_terminal, "OHCI: Device detected on port %u\r\n",
                      i);

      // Bit 1 = Enable (PES)
      if (!(status & 2)) {
        // Reset port (Set PRC - Bit 4)
        ohci_write(ohci, OHCI_REG_RHPORTSTATUS + (i * 4), 0x10);

        // Wait 50ms
        for (volatile int k = 0; k < 5000000; k++)
          ;

        // Wait for reset change (PRSC - Bit 20)
        int timeout = 1000000;
        while (!(ohci_read(ohci, OHCI_REG_RHPORTSTATUS + (i * 4)) & 0x100000) &&
               timeout--)
          __asm__("pause");

        // Clear PRSC
        ohci_write(ohci, OHCI_REG_RHPORTSTATUS + (i * 4), 0x100000);
      }

      // Check if enabled
      if (ohci_read(ohci, OHCI_REG_RHPORTSTATUS + (i * 4)) & 2) {
        // Enumerate
        usb_enumerate_device(controller, i + 1); // Ports are 1-based usually
      }
    }
  }
  return true;
}

// ========================================================================
// TRANSFER LOGIC (Simplified polling)
// ========================================================================

bool ohci_control_transfer(usb_device_t *device, usb_setup_packet_t *setup,
                           void *data, uint16_t length) {
  usb_controller_t *controller = &usb_controllers[device->controller_id];
  ohci_data_t *ohci = (ohci_data_t *)controller->regs;

  ohci_ed_t *ed = ohci_alloc_ed(ohci);
  ohci_td_t *setup_td = ohci_alloc_td(ohci);
  ohci_td_t *data_td = (length > 0) ? ohci_alloc_td(ohci) : NULL;
  ohci_td_t *status_td = ohci_alloc_td(ohci);
  ohci_td_t *dummy_td = ohci_alloc_td(ohci);

  if (!ed || !setup_td || !status_td || !dummy_td || (length > 0 && !data_td)) {
    // Cleanup... simplified for brevity, just return fail
    return false;
  }

  // Set up ED
  uint32_t speed = (device->speed == USB_SPEED_LOW) ? 1 : 0;
  uint32_t mps = device->descriptor.bMaxPacketSize0;
  if (mps == 0)
    mps = 8;

  ed->info = (device->address) | (0 << 7) | (0 << 11) | (speed << 13) |
             (0 << 14) | (mps << 16);
  ed->head_p = ohci_virt_to_phys(ohci, setup_td);
  ed->tail_p = ohci_virt_to_phys(ohci, dummy_td);
  ed->next_ed = 0;

  // Setup TD
  uint32_t setup_phys = ohci_virt_to_phys(ohci, setup);
  setup_td->info =
      (0x2 << 19) | (2 << 21) | (3 << 24) | (0 << 26); // SETUP, DATA0
  setup_td->cbp = setup_phys;
  setup_td->be = setup_phys + sizeof(usb_setup_packet_t) - 1;
  setup_td->next_td = (length > 0) ? ohci_virt_to_phys(ohci, data_td)
                                   : ohci_virt_to_phys(ohci, status_td);

  // Data TD
  if (length > 0) {
    uint32_t data_phys = ohci_virt_to_phys(ohci, data);
    uint32_t pid = (setup->bmRequestType & 0x80) ? 0x2 : 0x1; // IN : OUT
    data_td->info =
        (pid << 19) | (3 << 21) | (3 << 24) | (0 << 26); // PID, Data1
    data_td->cbp = data_phys;
    data_td->be = data_phys + length - 1;
    data_td->next_td = ohci_virt_to_phys(ohci, status_td);
  }

  // Status TD
  uint32_t status_pid =
      (length > 0 && (setup->bmRequestType & 0x80)) ? 0x1 : 0x2; // OUT : IN
  status_td->info =
      (status_pid << 19) | (3 << 21) | (3 << 24) | (0 << 26); // Data1
  status_td->cbp = 0;
  status_td->be = 0;
  status_td->next_td = ohci_virt_to_phys(ohci, dummy_td);

  // Link ED to Control List
  ohci_write(ohci, OHCI_REG_CONTROLHEAD, ohci_virt_to_phys(ohci, ed));
  ohci_write(ohci, OHCI_REG_CMDSTATUS, 0x02); // CLF - Control List Filled

  // Poll for completion
  // Wait until head_p == tail_p (dummy_td)
  // Or check if done head update

  uint32_t timeout = 2000000;
  while ((ed->head_p & ~3) != (ed->tail_p & ~3) && timeout--)
    __asm__("pause");

  // Unlink ED (Set Head to 0)
  ohci_write(ohci, OHCI_REG_CONTROLHEAD, 0);

  bool result = true;
  if (timeout <= 0 || (ed->head_p & 1)) { // Halted
    result = false;
  }

  ohci_free_td(ohci, setup_td);
  if (data_td)
    ohci_free_td(ohci, data_td);
  ohci_free_td(ohci, status_td);
  ohci_free_td(ohci, dummy_td);
  ohci_free_ed(ohci, ed);

  return result;
}

bool ohci_bulk_transfer(usb_device_t *device, uint8_t endpoint, void *data,
                        uint32_t length, bool is_in) {
  usb_controller_t *controller = &usb_controllers[device->controller_id];
  ohci_data_t *ohci = (ohci_data_t *)controller->regs;

  ohci_ed_t *ed = ohci_alloc_ed(ohci);
  ohci_td_t *td = ohci_alloc_td(ohci);
  ohci_td_t *dummy_td = ohci_alloc_td(ohci);

  if (!ed || !td || !dummy_td)
    return false;

  // Set up ED
  uint32_t ep_num = endpoint & 0xF;
  uint32_t mps = 64; // Should get from endpoint desc

  ed->info = (device->address) | (ep_num << 7) | (0 << 11) | (0 << 13) |
             (0 << 14) | (mps << 16);
  ed->head_p = ohci_virt_to_phys(ohci, td);
  ed->tail_p = ohci_virt_to_phys(ohci, dummy_td);
  ed->next_ed = 0;

  // Setup TD
  uint32_t data_phys = ohci_virt_to_phys(ohci, data);
  uint32_t pid = is_in ? 0x2 : 0x1;

  td->info =
      (pid << 19) | (0 << 21) | (3 << 24) | (0 << 26); // PID, Toggle from ED
  td->cbp = data_phys;
  td->be = data_phys + length - 1;
  td->next_td = ohci_virt_to_phys(ohci, dummy_td);

  // Link ED to Bulk List
  ohci_write(ohci, OHCI_REG_BULKHEAD, ohci_virt_to_phys(ohci, ed));
  ohci_write(ohci, OHCI_REG_CMDSTATUS, 0x04); // BLF

  uint32_t timeout = 10000000;
  while ((ed->head_p & ~3) != (ed->tail_p & ~3) && timeout--)
    __asm__("pause");

  ohci_write(ohci, OHCI_REG_BULKHEAD, 0);

  bool result = !(ed->head_p & 1) && (timeout > 0);

  ohci_free_td(ohci, td);
  ohci_free_td(ohci, dummy_td);
  ohci_free_ed(ohci, ed);

  return result;
}
