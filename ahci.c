#include "ahci.h"
#include "io.h"
#include "irq.h"
#include "isr.h" // Añadir para struct regs
#include "kernel.h"
#include "memory.h"
#include "mmu.h"
#include "string.h"
#include "terminal.h"
// Global AHCI controller
ahci_controller_t ahci_controller = {0};
// Timeout values
#define AHCI_TIMEOUT_MS 5000
#define AHCI_SPIN_TIMEOUT 1000000
// ========================================================================
// INICIALIZACIÓN
// ========================================================================
bool ahci_init(void) {
  terminal_puts(&main_terminal, "Initializing AHCI/SATA subsystem...\r\n");

  // Inicializar DMA si no está inicializado
  if (!dma_init()) {
    terminal_puts(&main_terminal,
                  "AHCI: Failed to initialize DMA subsystem\r\n");
    return false;
  }

  // Limpiar estructura del controlador
  memset(&ahci_controller, 0, sizeof(ahci_controller_t));

  // Detectar controlador AHCI
  if (!ahci_detect_controller()) {
    terminal_puts(&main_terminal, "AHCI: No AHCI controller detected\r\n");
    return false;
  }

  // Inicializar el controlador
  if (!ahci_initialize_controller()) {
    terminal_puts(&main_terminal, "AHCI: Failed to initialize controller\r\n");
    return false;
  }

  // NUEVO: Mostrar ports_implemented en binario para debug
  terminal_printf(&main_terminal,
                  "AHCI: Checking ports - implemented mask: 0x%08x\r\n",
                  ahci_controller.ports_implemented);

  // Inicializar puertos
  uint32_t ports_initialized = 0;
  for (uint8_t i = 0; i < 32; i++) { // AHCI soporta hasta 32 puertos
    if (ahci_controller.ports_implemented & (1 << i)) {
      terminal_printf(
          &main_terminal,
          "AHCI: Port %u is implemented, attempting initialization...\r\n", i);

      if (ahci_initialize_port(i)) {
        ports_initialized++;
      } else {
        terminal_printf(&main_terminal,
                        "AHCI: Port %u initialization failed or no device\r\n",
                        i);
      }
    }
  }

  ahci_controller.initialized = true;

  terminal_printf(&main_terminal,
                  "AHCI initialization complete: %u ports initialized\r\n",
                  ports_initialized);

  return true;
}

void ahci_cleanup(void) {
  if (!ahci_controller.initialized) {
    return;
  }
  // Limpiar buffers DMA
  for (uint8_t i = 0; i < ahci_controller.port_count; i++) {
    ahci_port_t *port = &ahci_controller.ports[i];
    if (port->cmd_list_buffer)
      dma_free_buffer(port->cmd_list_buffer);
    if (port->fis_buffer)
      dma_free_buffer(port->fis_buffer);
    for (int j = 0; j < AHCI_MAX_CMDS; j++) {
      if (port->cmd_table_buffers[j]) {
        dma_free_buffer(port->cmd_table_buffers[j]);
      }
    }

    // Deshabilitar AHCI
    if (ahci_controller.abar) {
      ahci_controller.abar->ghc &= ~AHCI_GHC_AE;
    }
    ahci_controller.initialized = false;
    terminal_puts(&main_terminal, "AHCI cleanup complete\r\n");
  }
}

bool ahci_detect_controller(void) {
  // Buscar controlador AHCI en PCI
  ahci_controller.pci_device =
      pci_find_device_by_class(AHCI_PCI_CLASS, AHCI_PCI_SUBCLASS);

  if (!ahci_controller.pci_device) {
    // También intentar buscar por vendor/device específicos
    terminal_puts(&main_terminal, "AHCI: No generic AHCI controller found, "
                                  "checking specific vendors...\r\n");

    // Intel AHCI controllers
    ahci_controller.pci_device = pci_find_device(0x8086, 0x2922); // ICH9M
    if (!ahci_controller.pci_device) {
      ahci_controller.pci_device = pci_find_device(0x8086, 0x3A22); // ICH10
    }

    // AMD AHCI controllers
    if (!ahci_controller.pci_device) {
      ahci_controller.pci_device = pci_find_device(0x1002, 0x4391); // AMD AHCI
    }

    if (!ahci_controller.pci_device) {
      return false;
    }
  }

  terminal_printf(
      &main_terminal, "AHCI: Found controller %04x:%04x at %02x:%02x.%x\r\n",
      ahci_controller.pci_device->vendor_id,
      ahci_controller.pci_device->device_id, ahci_controller.pci_device->bus,
      ahci_controller.pci_device->device, ahci_controller.pci_device->function);

  return true;
}

bool ahci_initialize_controller(void) {
  pci_device_t *pci_dev = ahci_controller.pci_device;

  // Habilitar bus mastering y memory space
  pci_enable_bus_mastering(pci_dev);
  pci_enable_memory_space(pci_dev);

  // Obtener ABAR (AHCI Base Address Register) - BAR5
  if (!pci_dev->bars[5].is_valid) {
    terminal_puts(&main_terminal, "AHCI: BAR5 (ABAR) not valid\r\n");
    return false;
  }
  ahci_controller.abar_physical = pci_dev->bars[5].address;

  // Mapear ABAR a memoria virtual
  uint32_t abar_size = pci_dev->bars[5].size;
  if (abar_size < sizeof(hba_mem_t)) {
    terminal_printf(&main_terminal, "AHCI: ABAR size %u too small\r\n",
                    abar_size);
    return false;
  }

  uint32_t abar_virtual = 0;
  if (!mmu_ensure_physical_accessible(ahci_controller.abar_physical, abar_size,
                                      &abar_virtual)) {
    terminal_puts(&main_terminal,
                  "AHCI: Failed to map ABAR to virtual memory\r\n");
    return false;
  }

  ahci_controller.abar = (hba_mem_t *)abar_virtual;

  terminal_printf(&main_terminal,
                  "AHCI: ABAR mapped - phys=0x%08x, virt=0x%08x, size=%u\r\n",
                  ahci_controller.abar_physical, abar_virtual, abar_size);

  // Verificar que es realmente un controlador AHCI
  uint32_t version = ahci_controller.abar->vs;
  terminal_printf(&main_terminal, "AHCI: Version %u.%u%u\r\n",
                  (version >> 16) & 0xFFFF, (version >> 8) & 0xFF,
                  version & 0xFF);

  // Leer capacidades
  uint32_t cap = ahci_controller.abar->cap;
  ahci_controller.port_count = (cap & AHCI_CAP_NP_MASK) + 1;
  ahci_controller.command_slots =
      ((cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;
  ahci_controller.supports_64bit = (cap & AHCI_CAP_S64A) != 0;
  ahci_controller.supports_ncq = (cap & AHCI_CAP_SNCQ) != 0;
  ahci_controller.ports_implemented = ahci_controller.abar->pi;

  terminal_printf(
      &main_terminal,
      "AHCI: Capabilities - ports=%u, slots=%u, 64bit=%u, ncq=%u\r\n",
      ahci_controller.port_count, ahci_controller.command_slots,
      ahci_controller.supports_64bit, ahci_controller.supports_ncq);

  terminal_printf(&main_terminal, "AHCI: Ports implemented: 0x%08x\r\n",
                  ahci_controller.ports_implemented);

  // CRÍTICO: Mostrar qué puertos están implementados en binario
  terminal_puts(&main_terminal, "AHCI: Port bitmap: ");
  for (int i = 31; i >= 0; i--) {
    if (i == 31 || i == 23 || i == 15 || i == 7) {
      terminal_putchar(&main_terminal, ' ');
    }
    terminal_putchar(&main_terminal,
                     (ahci_controller.ports_implemented & (1 << i)) ? '1'
                                                                    : '0');
  }
  terminal_puts(&main_terminal, "\r\n");

  // CRÍTICO: Listar puertos específicos
  terminal_puts(&main_terminal, "AHCI: Implemented ports: ");
  bool first = true;
  for (int i = 0; i < 32; i++) {
    if (ahci_controller.ports_implemented & (1 << i)) {
      if (!first)
        terminal_puts(&main_terminal, ", ");
      terminal_printf(&main_terminal, "%u", i);
      first = false;
    }
  }
  terminal_puts(&main_terminal, "\r\n");

  // Realizar BIOS handoff si es necesario
  uint32_t bohc = ahci_controller.abar->bohc;
  if (bohc & 0x01) { // BIOS owns HBA
    terminal_puts(&main_terminal,
                  "AHCI: Requesting ownership from BIOS...\r\n");
    ahci_controller.abar->bohc |= 0x02; // OS ownership request

    // Esperar hasta 25 segundos para el handoff
    uint32_t timeout = 25000;
    while ((ahci_controller.abar->bohc & 0x01) && timeout--) {
      // Esperar 1ms
      for (volatile int i = 0; i < 10000; i++)
        ;
    }

    if (ahci_controller.abar->bohc & 0x01) {
      terminal_puts(&main_terminal, "AHCI: WARNING - BIOS handoff timeout\r\n");
    } else {
      terminal_puts(&main_terminal, "AHCI: BIOS handoff completed\r\n");
    }
  }

  // Habilitar AHCI
  if (!(ahci_controller.abar->ghc & AHCI_GHC_AE)) {
    ahci_controller.abar->ghc |= AHCI_GHC_AE;
    terminal_puts(&main_terminal, "AHCI: AHCI mode enabled\r\n");
  }

  // Habilitar interrupciones globales
  ahci_controller.abar->ghc |= AHCI_GHC_IE;

  return true;
}

bool ahci_initialize_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS) {
    terminal_printf(&main_terminal, "AHCI: Invalid port number %u\r\n",
                    port_num);
    return false;
  }

  ahci_port_t *port = &ahci_controller.ports[port_num];
  memset(port, 0, sizeof(ahci_port_t));
  port->port_num = port_num;
  port->port_regs =
      (hba_port_t *)((uint8_t *)ahci_controller.abar + AHCI_PORT_BASE +
                     (port_num * AHCI_PORT_SIZE));

  terminal_printf(&main_terminal, "AHCI: Initializing port %u...\r\n",
                  port_num);

  // Leer el estado del puerto inmediatamente
  uint32_t ssts = port->port_regs->ssts;
  uint32_t sig = port->port_regs->sig;
  uint32_t cmd = port->port_regs->cmd;

  terminal_printf(&main_terminal,
                  "AHCI: Port %u - SSTS=0x%08x, SIG=0x%08x, CMD=0x%08x\r\n",
                  port_num, ssts, sig, cmd);

  // Verificar si hay dispositivo presente
  uint8_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
  if (det == AHCI_PORT_DET_NONE || det == 0) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u - No device detected (DET=%u)\r\n", port_num,
                    det);
    port->present = false;
    return false;
  }

  port->present = true;
  port->signature = sig;

  // Determinar tipo de dispositivo por signature
  switch (sig) {
  case AHCI_SIG_ATA:
    port->device_type = 1; // SATA Drive
    terminal_printf(&main_terminal, "AHCI: Port %u - SATA Disk detected\r\n",
                    port_num);
    break;
  case AHCI_SIG_ATAPI:
    port->device_type = 2; // ATAPI Drive
    terminal_printf(&main_terminal, "AHCI: Port %u - ATAPI Drive detected\r\n",
                    port_num);
    break;
  case AHCI_SIG_SEMB:
    port->device_type = 3; // Enclosure Management Bridge
    terminal_printf(&main_terminal,
                    "AHCI: Port %u - Enclosure Bridge detected\r\n", port_num);
    break;
  case AHCI_SIG_PM:
    port->device_type = 4; // Port Multiplier
    terminal_printf(&main_terminal,
                    "AHCI: Port %u - Port Multiplier detected\r\n", port_num);
    break;
  default:
    terminal_printf(&main_terminal,
                    "AHCI: Port %u - Unknown device (SIG=0x%08x)\r\n", port_num,
                    sig);
    port->device_type = 0;
    break;
  }

  // Inicializar estructuras DMA para el puerto
  if (port->device_type == 1 || port->device_type == 2) {
    // Allocate command list (1KB aligned)
    port->cmd_list_buffer = dma_alloc_buffer(AHCI_CMD_SLOT_SIZE * 32, 1024);
    if (!port->cmd_list_buffer) {
      terminal_printf(&main_terminal,
                      "AHCI: Failed to allocate command list for port %u\r\n",
                      port_num);
      return false;
    }
    port->cmd_list = (hba_cmd_header_t *)port->cmd_list_buffer->virtual_address;
    memset(port->cmd_list, 0, AHCI_CMD_SLOT_SIZE * 32);

    // Allocate FIS area (256 bytes aligned)
    port->fis_buffer = dma_alloc_buffer(AHCI_RX_FIS_SIZE, 256);
    if (!port->fis_buffer) {
      terminal_printf(&main_terminal,
                      "AHCI: Failed to allocate FIS buffer for port %u\r\n",
                      port_num);
      dma_free_buffer(port->cmd_list_buffer);
      return false;
    }
    port->fis_base = (uint8_t *)port->fis_buffer->virtual_address;
    memset(port->fis_base, 0, AHCI_RX_FIS_SIZE);

    // Initialize command tables
    for (int i = 0; i < AHCI_MAX_CMDS; i++) {
      port->cmd_table_buffers[i] = dma_alloc_buffer(sizeof(hba_cmd_tbl_t), 128);
      if (!port->cmd_table_buffers[i]) {
        terminal_printf(
            &main_terminal,
            "AHCI: Failed to allocate command table %d for port %u\r\n", i,
            port_num);
        // Cleanup already allocated buffers
        for (int j = 0; j < i; j++) {
          dma_free_buffer(port->cmd_table_buffers[j]);
        }
        dma_free_buffer(port->fis_buffer);
        dma_free_buffer(port->cmd_list_buffer);
        return false;
      }
      port->cmd_tables[i] =
          (hba_cmd_tbl_t *)port->cmd_table_buffers[i]->virtual_address;
      memset(port->cmd_tables[i], 0, sizeof(hba_cmd_tbl_t));

      // Link command table to command header
      port->cmd_list[i].ctba =
          port->cmd_table_buffers[i]->physical_address & 0xFFFFFFFF;
      if (ahci_controller.supports_64bit) {
        port->cmd_list[i].ctbau =
            (port->cmd_table_buffers[i]->physical_address >> 32) & 0xFFFFFFFF;
      }
    }

    // Set command list and FIS base addresses
    port->port_regs->clb = port->cmd_list_buffer->physical_address & 0xFFFFFFFF;
    port->port_regs->clbu =
        ahci_controller.supports_64bit
            ? (port->cmd_list_buffer->physical_address >> 32) & 0xFFFFFFFF
            : 0;
    port->port_regs->fb = port->fis_buffer->physical_address & 0xFFFFFFFF;
    port->port_regs->fbu =
        ahci_controller.supports_64bit
            ? (port->fis_buffer->physical_address >> 32) & 0xFFFFFFFF
            : 0;

    // Clear interrupts
    port->port_regs->is = ~0U;

    // Enable interrupts
    port->port_regs->ie = AHCI_PORT_IE_MASK;

    // Start the port
    if (!ahci_start_port(port_num)) {
      terminal_printf(&main_terminal, "AHCI: Failed to start port %u\r\n",
                      port_num);
      return false;
    }

    port->initialized = true;
    terminal_printf(&main_terminal,
                    "AHCI: Port %u initialized successfully\r\n", port_num);
    return true;
  }

  return false;
}

bool ahci_start_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS)
    return false;
  ahci_port_t *port = &ahci_controller.ports[port_num];
  hba_port_t *regs = port->port_regs;
  // Asegurarse de que el puerto esté detenido
  if (regs->cmd & (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE)) {
    if (!ahci_stop_port(port_num)) {
      return false;
    }
  }
  // Limpiar interrupciones pendientes
  regs->is = ~0U;
  // Habilitar FIS Receive (FRE)
  regs->cmd |= AHCI_PORT_CMD_FRE;
  // Esperar a que FR se active
  uint32_t timeout = 10000;
  while (!(regs->cmd & AHCI_PORT_CMD_FR) && timeout--) {
    __asm__ volatile("pause");
  }
  if (!(regs->cmd & AHCI_PORT_CMD_FR)) {
    terminal_printf(&main_terminal, "AHCI: Port %u FIS receive not running\r\n",
                    port_num);
    return false;
  }
  // Forzar ICC a Active (importante después de COMRESET)
  regs->cmd = (regs->cmd & ~AHCI_PORT_CMD_ICC_MASK) |
              (AHCI_PORT_CMD_ICC_ACTIVE << AHCI_PORT_CMD_ICC_SHIFT);
  // Habilitar Start (ST)
  regs->cmd |= AHCI_PORT_CMD_ST;
  // Esperar a que CR se active
  timeout = 10000;
  while (!(regs->cmd & AHCI_PORT_CMD_CR) && timeout--) {
    __asm__ volatile("pause");
  }
  if (!(regs->cmd & AHCI_PORT_CMD_CR)) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u command list not running\r\n", port_num);
    return false;
  }
  terminal_printf(&main_terminal, "AHCI: Port %u started successfully\r\n",
                  port_num);
  return true;
}
bool ahci_stop_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS)
    return false;
  ahci_port_t *port = &ahci_controller.ports[port_num];
  hba_port_t *regs = port->port_regs;
  // Desactivar ST (Start)
  regs->cmd &= ~AHCI_PORT_CMD_ST;
  // Esperar a que CR (Command List Running) se desactive
  uint32_t timeout = 5000;
  while ((regs->cmd & AHCI_PORT_CMD_CR) && timeout--) {
    __asm__ volatile("pause");
  }
  // Desactivar FRE (FIS Receive Enable)
  regs->cmd &= ~AHCI_PORT_CMD_FRE;
  // Esperar a que FR (FIS Receive Running) se desactive
  timeout = 5000;
  while ((regs->cmd & AHCI_PORT_CMD_FR) && timeout--) {
    __asm__ volatile("pause");
  }
  if (regs->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) {
    terminal_printf(&main_terminal,
                    "AHCI: Failed to stop port %u (CMD=0x%08x)\r\n", port_num,
                    regs->cmd);
    return false;
  }
  terminal_printf(&main_terminal, "AHCI: Port %u stopped\r\n", port_num);
  return true;
}
// ========================================================================
// FUNCIONES DE COMANDO
// ========================================================================
int ahci_find_cmdslot(ahci_port_t *port) {
  uint32_t slots = (port->port_regs->sact | port->port_regs->ci);
  for (int i = 0; i < AHCI_MAX_CMDS; i++) {
    if ((slots & (1 << i)) == 0 && !port->command_slots[i]) {
      return i;
    }
  }
  return -1; // No free slot
}
bool ahci_spin_up_device(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS)
    return false;
  ahci_port_t *port = &ahci_controller.ports[port_num];
  if (!port->present)
    return false;
  terminal_printf(&main_terminal,
                  "AHCI: Attempting to spin up device on port %u...\r\n",
                  port_num);
  // Método 1: Usar SUD (Spin Up Device)
  port->port_regs->cmd |= AHCI_PORT_CMD_SUD;
  // Esperar
  for (volatile int i = 0; i < 2000000; i++)
    ; // 20ms
  // Verificar estado
  uint32_t ssts = port->port_regs->ssts;
  uint8_t ipm = (ssts >> 8) & 0x0F;
  if (ipm == 1) {
    terminal_printf(&main_terminal,
                    "AHCI: Device spun up successfully (IPM=%u)\r\n", ipm);
    return true;
  }
  // Método 2: Enviar comando IDLE IMMEDIATE si el primero falla
  terminal_puts(&main_terminal,
                "AHCI: SUD failed, trying IDLE IMMEDIATE command...\r\n");
  int slot = ahci_find_cmdslot(port);
  if (slot == -1) {
    terminal_puts(&main_terminal,
                  "AHCI: No free command slot for IDLE IMMEDIATE\r\n");
    return false;
  }
  fis_reg_h2d_t fis = {0};
  fis.fis_type = FIS_TYPE_REG_H2D;
  fis.c = 1;
  fis.command = 0xE3;  // IDLE IMMEDIATE
  fis.featurel = 0x44; // Disable power management
  if (ahci_send_command(port_num, slot, (uint8_t *)&fis, NULL, 0, false)) {
    terminal_puts(&main_terminal, "AHCI: IDLE IMMEDIATE command sent\r\n");
    return true;
  }
  terminal_puts(&main_terminal, "AHCI: Failed to spin up device\r\n");
  return false;
}
bool ahci_send_command(uint8_t port_num, uint8_t slot, uint8_t *fis,
                       void *buffer, uint32_t buffer_size, bool write) {
  if (port_num >= ahci_controller.port_count ||
      !ahci_controller.ports[port_num].initialized) {
    return false;
  }
  ahci_port_t *port = &ahci_controller.ports[port_num];
  hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
  hba_cmd_tbl_t *cmd_table = port->cmd_tables[slot];
  // Configurar command header
  cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t); // FIS length
  cmd_header->w = write ? 1 : 0;
  cmd_header->prdtl = buffer ? 1 : 0; // Number of PRDT entries
  // Copiar FIS al command table
  memcpy(cmd_table->cfis, fis, sizeof(fis_reg_h2d_t));
  // Configurar PRDT si hay buffer
  if (buffer && buffer_size > 0) {
    uint32_t phys_addr = mmu_virtual_to_physical((uint32_t)buffer);
    if (phys_addr == 0) {
      terminal_printf(&main_terminal,
                      "AHCI: Failed to get physical address for buffer\r\n");
      return false;
    }
    cmd_table->prdt_entry[0].dba = phys_addr;
    cmd_table->prdt_entry[0].dbau = 0;
    cmd_table->prdt_entry[0].dbc = buffer_size - 1; // Size - 1
    cmd_table->prdt_entry[0].i = 1;                 // Interrupt on completion
  }
  // Marcar slot como usado
  port->command_slots[slot] = true;
  // Emitir comando
  port->port_regs->ci = 1 << slot;
  // Esperar completación
  uint32_t timeout = AHCI_TIMEOUT_MS * 1000; // Convert to microseconds
  while ((port->port_regs->ci & (1 << slot)) && timeout--) {
    // Check for errors
    if (port->port_regs->is & AHCI_PORT_IS_TFES) {
      terminal_printf(&main_terminal, "AHCI: Task file error on port %u\r\n",
                      port_num);
      port->command_slots[slot] = false;
      return false;
    }
    // Small delay
    for (volatile int i = 0; i < 10; i++)
      ;
  }
  port->command_slots[slot] = false;
  if (port->port_regs->ci & (1 << slot)) {
    terminal_printf(&main_terminal,
                    "AHCI: Command timeout on port %u slot %u\r\n", port_num,
                    slot);
    return false;
  }
  // Clear interrupts
  port->port_regs->is = 0xFFFFFFFF;
  return true;
}
bool ahci_identify_device(uint8_t port_num, void *buffer) {
  if (!buffer)
    return false;
  ahci_port_t *port = &ahci_controller.ports[port_num];
  int slot = ahci_find_cmdslot(port);
  if (slot == -1)
    return false;
  // Preparar FIS
  fis_reg_h2d_t fis = {0};
  fis.fis_type = FIS_TYPE_REG_H2D;
  fis.c = 1; // Command bit
  fis.command =
      (port->device_type == 2) ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;
  return ahci_send_command(port_num, slot, (uint8_t *)&fis, buffer, 512, false);
}
bool ahci_read_sectors(uint8_t port_num, uint64_t lba, uint32_t count,
                       void *buffer) {
  if (!buffer || count == 0)
    return false;
  ahci_port_t *port = &ahci_controller.ports[port_num];
  if (port->device_type != 1)
    return false; // Solo SATA
  int slot = ahci_find_cmdslot(port);
  if (slot == -1)
    return false;
  // Preparar FIS
  fis_reg_h2d_t fis = {0};
  fis.fis_type = FIS_TYPE_REG_H2D;
  fis.c = 1; // Command bit
  if (lba > 0xFFFFFFF) {
    // LBA48
    fis.command = ATA_CMD_READ_DMA_EXT;
    fis.lba0 = lba & 0xFF;
    fis.lba1 = (lba >> 8) & 0xFF;
    fis.lba2 = (lba >> 16) & 0xFF;
    fis.lba3 = (lba >> 24) & 0xFF;
    fis.lba4 = (lba >> 32) & 0xFF;
    fis.lba5 = (lba >> 40) & 0xFF;
    fis.device = 1 << 6; // LBA mode
    fis.countl = count & 0xFF;
    fis.counth = (count >> 8) & 0xFF;
  } else {
    // LBA28
    fis.command = ATA_CMD_READ_DMA;
    fis.lba0 = lba & 0xFF;
    fis.lba1 = (lba >> 8) & 0xFF;
    fis.lba2 = (lba >> 16) & 0xFF;
    fis.device = (1 << 6) | ((lba >> 24) & 0x0F); // LBA mode + upper 4 bits
    fis.countl = count & 0xFF;
  }
  return ahci_send_command(port_num, slot, (uint8_t *)&fis, buffer, count * 512,
                           false);
}

bool ahci_write_sectors(uint8_t port_num, uint64_t lba, uint32_t count,
                        const void *buffer) {
  if (!buffer || count == 0) {
    terminal_printf(&main_terminal, "AHCI: Invalid write parameters\n");
    return false;
  }

  ahci_port_t *port = &ahci_controller.ports[port_num];
  if (!port->initialized || port->device_type != 1) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u not initialized or not SATA\n", port_num);
    return false;
  }

  // terminal_printf(
  //     &main_terminal,
  //     "AHCI: Write sectors - port=%u, lba=%llu, count=%u, buffer=%p\n",
  //     port_num, lba, count, buffer);

  // Verificar que hay slots disponibles
  int slot = ahci_find_cmdslot(port);
  if (slot == -1) {
    terminal_printf(&main_terminal, "AHCI: No free command slots on port %u\n",
                    port_num);
    return false;
  }

  // terminal_printf(&main_terminal, "AHCI: Using command slot %d\n", slot);

  // Preparar FIS
  fis_reg_h2d_t fis = {0};
  fis.fis_type = FIS_TYPE_REG_H2D;
  fis.c = 1; // Command bit

  if (lba > 0xFFFFFFF) {
    // LBA48
    fis.command = ATA_CMD_WRITE_DMA_EXT;
    fis.lba0 = lba & 0xFF;
    fis.lba1 = (lba >> 8) & 0xFF;
    fis.lba2 = (lba >> 16) & 0xFF;
    fis.lba3 = (lba >> 24) & 0xFF;
    fis.lba4 = (lba >> 32) & 0xFF;
    fis.lba5 = (lba >> 40) & 0xFF;
    fis.device = 1 << 6; // LBA mode
    fis.countl = count & 0xFF;
    fis.counth = (count >> 8) & 0xFF;
    // terminal_printf(&main_terminal, "AHCI: Using LBA48 command\n");
  } else {
    // LBA28
    fis.command = ATA_CMD_WRITE_DMA;
    fis.lba0 = lba & 0xFF;
    fis.lba1 = (lba >> 8) & 0xFF;
    fis.lba2 = (lba >> 16) & 0xFF;
    fis.device = (1 << 6) | ((lba >> 24) & 0x0F); // LBA mode + upper 4 bits
    fis.countl = count & 0xFF;
    // terminal_printf(&main_terminal, "AHCI: Using LBA28 command\n");
  }

  // terminal_printf(&main_terminal,
  //              "AHCI: FIS prepared - command=0x%02X, device=0x%02X\n",
  //              fis.command, fis.device);

  // Enviar comando
  bool result = ahci_send_command(port_num, slot, (uint8_t *)&fis,
                                  (void *)buffer, count * 512, true);

  if (!result) {
    terminal_printf(&main_terminal, "AHCI: send_command failed\n");

    // Mostrar estado del puerto
    terminal_printf(&main_terminal, "AHCI: Port %u status after failure:\n",
                    port_num);
    terminal_printf(&main_terminal, "  CMD: 0x%08x\n", port->port_regs->cmd);
    terminal_printf(&main_terminal, "  TFD: 0x%08x\n", port->port_regs->tfd);
    terminal_printf(&main_terminal, "  IS: 0x%08x\n", port->port_regs->is);
    terminal_printf(&main_terminal, "  CI: 0x%08x\n", port->port_regs->ci);
    terminal_printf(&main_terminal, "  SERR: 0x%08x\n", port->port_regs->serr);
  } else {
    // terminal_printf(&main_terminal, "AHCI: Write completed successfully\n");
  }

  return result;
}

// ========================================================================
// FUNCIONES DE UTILIDAD
// ========================================================================
void ahci_list_devices(void) {
  terminal_puts(&main_terminal, "\r\n=== AHCI/SATA Devices ===\r\n");
  if (!ahci_controller.initialized) {
    terminal_puts(&main_terminal, "AHCI not initialized\r\n");
    return;
  }
  terminal_printf(&main_terminal, "Controller: %04x:%04x\r\n",
                  ahci_controller.pci_device->vendor_id,
                  ahci_controller.pci_device->device_id);
  uint32_t devices_found = 0;
  for (uint8_t i = 0; i < ahci_controller.port_count; i++) {
    if (ahci_controller.ports[i].present) {
      ahci_port_t *port = &ahci_controller.ports[i];
      terminal_printf(&main_terminal, "Port %u: %s\r\n", i,
                      ahci_get_device_type_name(port->signature));
      if (port->initialized && port->device_type == 1) {
        // Intentar identificar dispositivo SATA
        uint16_t *identify_buffer = (uint16_t *)kernel_malloc(512);
        if (identify_buffer && ahci_identify_device(i, identify_buffer)) {
          // Extraer información básica
          uint32_t sectors_28 =
              (identify_buffer[61] << 16) | identify_buffer[60];
          uint64_t sectors_48 = 0;
          if (identify_buffer[83] & (1 << 10)) { // LBA48 support
            sectors_48 = ((uint64_t)identify_buffer[103] << 48) |
                         ((uint64_t)identify_buffer[102] << 32) |
                         ((uint64_t)identify_buffer[101] << 16) |
                         identify_buffer[100];
          }
          uint64_t total_sectors = sectors_48 ? sectors_48 : sectors_28;
          uint64_t size_mb = (total_sectors * 512) / (1024 * 1024);
          terminal_printf(&main_terminal,
                          "  Capacity: %llu sectors (%llu MB)\r\n",
                          total_sectors, size_mb);
          // Modelo (palabras 27-46, swapped)
          char model[41] = {0};
          for (int j = 0; j < 20; j++) {
            uint16_t word = identify_buffer[27 + j];
            model[j * 2] = (word >> 8) & 0xFF;
            model[j * 2 + 1] = word & 0xFF;
          }
          // Remover espacios al final
          for (int j = 39; j >= 0 && (model[j] == ' ' || model[j] == 0); j--) {
            model[j] = 0;
          }
          terminal_printf(&main_terminal, "  Model: %s\r\n", model);
        }
        if (identify_buffer)
          kernel_free(identify_buffer);
      }
      devices_found++;
    }
  }
  if (devices_found == 0) {
    terminal_puts(&main_terminal, "No devices detected\r\n");
  }
  terminal_puts(&main_terminal, "\r\n");
}
void ahci_print_port_status(uint8_t port_num) {
  if (port_num >= ahci_controller.port_count) {
    terminal_printf(&main_terminal, "Invalid port number %u\r\n", port_num);
    return;
  }
  ahci_port_t *port = &ahci_controller.ports[port_num];
  terminal_printf(&main_terminal, "\r\n=== AHCI Port %u Status ===\r\n",
                  port_num);
  if (!port->present) {
    terminal_puts(&main_terminal, "No device present\r\n");
    return;
  }
  terminal_printf(&main_terminal, "Device Type: %s\r\n",
                  ahci_get_device_type_name(port->signature));
  terminal_printf(&main_terminal, "Initialized: %s\r\n",
                  port->initialized ? "Yes" : "No");
  uint32_t ssts = port->port_regs->ssts;
  uint8_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
  uint8_t spd = (ssts >> AHCI_PORT_SSTS_SPD_SHIFT) & AHCI_PORT_SSTS_SPD_MASK;
  uint8_t ipm = (ssts >> AHCI_PORT_SSTS_IPM_SHIFT) & AHCI_PORT_SSTS_IPM_MASK;
  terminal_printf(&main_terminal, "SATA Status: det=%u, spd=%u, ipm=%u\r\n",
                  det, spd, ipm);
  terminal_printf(&main_terminal, "Command: 0x%08x\r\n", port->port_regs->cmd);
  terminal_printf(&main_terminal, "Status: 0x%08x\r\n", port->port_regs->is);
  terminal_printf(&main_terminal, "Error: 0x%08x\r\n", port->port_regs->serr);
  terminal_puts(&main_terminal, "\r\n");
}
const char *ahci_get_device_type_name(uint32_t signature) {
  switch (signature) {
  case AHCI_SIG_ATA:
    return "SATA Drive";
  case AHCI_SIG_ATAPI:
    return "ATAPI Drive";
  case AHCI_SIG_SEMB:
    return "Enclosure Management Bridge";
  case AHCI_SIG_PM:
    return "Port Multiplier";
  default:
    return "Unknown Device";
  }
}
// ========================================================================
// IRQ HANDLER
// ========================================================================
void ahci_irq_handler(struct regs *r) {
  if (!ahci_controller.initialized) {
    return;
  }
  uint32_t global_is = ahci_controller.abar->is;
  for (uint8_t port = 0; port < ahci_controller.port_count; port++) {
    if (!(global_is & (1 << port)))
      continue;
    if (!ahci_controller.ports[port].initialized)
      continue;
    ahci_port_t *ahci_port = &ahci_controller.ports[port];
    uint32_t port_is = ahci_port->port_regs->is;
    if (port_is & AHCI_PORT_IS_TFES) {
      terminal_printf(&main_terminal, "AHCI: Task file error on port %u\r\n",
                      port);
    }
    if (port_is & AHCI_PORT_IS_DHRS) {
      // Device to Host Register FIS received
    }
    if (port_is & AHCI_PORT_IS_PCS) {
      terminal_printf(&main_terminal,
                      "AHCI: Port connect change on port %u\r\n", port);
    }
    // Clear port interrupts
    ahci_port->port_regs->is = port_is;
  }
  // Clear global interrupts
  ahci_controller.abar->is = global_is;
  pic_send_eoi(r->int_no - 32);
}