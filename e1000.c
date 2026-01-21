#include "e1000.h"
#include "driver_system.h"
#include "io.h"
#include "kernel.h"
#include "memory.h"
#include "mmu.h"
#include "pci.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"

// Dispositivo E1000 global
e1000_device_t e1000_device = {0};

// ===================================================
// FUNCIONES DE ACCESO A REGISTROS
// ===================================================

// Reemplaza las funciones de lectura/escritura:
static inline uint32_t e1000_read_reg(uint32_t reg) {
  if (e1000_device.mem_virt) {
    uint32_t value = *(volatile uint32_t *)(e1000_device.mem_virt + reg);
    // El E1000 usa little-endian en memoria, igual que x86
    return value;
  } else {
    return inl(e1000_device.io_base + reg);
  }
}

static inline void e1000_write_reg(uint32_t reg, uint32_t value) {
  if (e1000_device.mem_virt) {
    *(volatile uint32_t *)(e1000_device.mem_virt + reg) = value;
  } else {
    outl(e1000_device.io_base + reg, value);
  }
}

// ===================================================
// FUNCIONES DE INICIALIZACIÓN
// ===================================================

static bool e1000_detect_device(void) {
  serial_printf(COM1_BASE,
                "[E1000] Searching for Intel E1000/E1000e NIC...\r\n");

  // Lista de IDs de dispositivos soportados
  const uint16_t supported_devices[] = {
      0x100E, 0x100F, 0x1004, 0x1000, 0x1001, 0x1008, 0x100C,
      0x1015, 0x1017, 0x1016, 0x101E, 0x153B, 0x153A, 0x1559,
      0x155A, 0x15B8, 0x15B7, 0x10D3, 0x10F6, 0x1502, 0x1503,
      0x10EA, 0x10EB, 0x10EF, 0x10F0, 0x294C, 0x10BD, 0};

  pci_device_t *device = NULL;

  for (int i = 0; supported_devices[i] != 0; i++) {
    device = pci_find_device(0x8086, supported_devices[i]);
    if (device) {
      serial_printf(COM1_BASE, "[E1000] Found supported device ID: 0x%04x\r\n",
                    supported_devices[i]);
      break;
    }
  }

  if (!device) {
    serial_printf(COM1_BASE,
                  "[E1000] No supported Intel E1000/E1000e device found\r\n");
    return false;
  }

  serial_printf(COM1_BASE, "[E1000] Found device at %02x:%02x.%x\r\n",
                device->bus, device->device, device->function);

  // Obtener BAR0 (normalmente memoria MMIO)
  for (int i = 0; i < 6; i++) {
    if (device->bars[i].is_valid &&
        device->bars[i].type == PCI_BAR_TYPE_MEMORY) {
      e1000_device.mem_base = device->bars[i].address;
      serial_printf(COM1_BASE, "[E1000] MMIO BAR%d: 0x%08x%08x (size: %u)\r\n",
                    i, (uint32_t)(e1000_device.mem_base >> 32),
                    (uint32_t)(e1000_device.mem_base & 0xFFFFFFFF),
                    device->bars[i].size);

      if (e1000_device.mem_base > 0xFFFFFFFF) {
        terminal_puts(&main_terminal,
                      "[E1000] ERROR: BAR is above 4GB, not addressable!\r\n");
        return false;
      }
      break;
    }
  }

  // Si no hay MMIO, usar I/O
  if (!e1000_device.mem_base) {
    for (int i = 0; i < 6; i++) {
      if (device->bars[i].is_valid && device->bars[i].type == PCI_BAR_TYPE_IO) {
        e1000_device.io_base = device->bars[i].address;
        serial_printf(COM1_BASE, "[E1000] I/O BAR%d: 0x%08x\r\n", i,
                      e1000_device.io_base);
        break;
      }
    }
  }

  if (!e1000_device.mem_base && !e1000_device.io_base) {
    terminal_puts(&main_terminal, "[E1000] ERROR: No valid BARs found!\r\n");
    return false;
  }

  e1000_device.irq_line = device->interrupt_line;

  // Habilitar Bus Mastering y Memory Space explícitamente (CRÍTICO)
  pci_enable_bus_mastering(device);
  pci_enable_memory_space(device);
  serial_printf(COM1_BASE,
                "[E1000] PCI Bus Mastering and Memory Space ENABLED\r\n");

  return true;
}

static bool e1000_map_memory(void) {
  if (!e1000_device.mem_base) {
    return false;
  }

  serial_printf(COM1_BASE, "[E1000] Mapping MMIO at phys=0x%08x\r\n",
                e1000_device.mem_base);

  // IMPORTANTE: Los dispositivos PCI pueden necesitar espacio no cacheable
  // Usar 0xF0000000 como base virtual para dispositivos PCI
  uint32_t virt_addr = 0xF0000000;

  // Verificar que no esté ya mapeado
  if (mmu_is_mapped(virt_addr)) {
    // Ya está mapeado, usar esa dirección
    e1000_device.mem_virt = (uint8_t *)virt_addr;
    serial_printf(COM1_BASE, "[E1000] Already mapped at virt=0x%08x\r\n",
                  virt_addr);
    return true;
  }

  // Intentar mapear página por página
  // Mapear solo 4KB primero para probar
  if (!mmu_map_page(virt_addr, e1000_device.mem_base,
                    PAGE_PRESENT | PAGE_RW | PAGE_CACHE_DISABLE)) {
    serial_printf(COM1_BASE, "[E1000] Failed to map first page\r\n");

    // Intentar con una dirección virtual diferente
    virt_addr = 0xF0100000;
    if (!mmu_map_page(virt_addr, e1000_device.mem_base,
                      PAGE_PRESENT | PAGE_RW | PAGE_CACHE_DISABLE)) {
      terminal_puts(&main_terminal, "[E1000] Failed with alt address\r\n");
      return false;
    }
  }

  // Mapear páginas adicionales si es necesario
  // El E1000 necesita ~256KB de espacio MMIO
  uint32_t pages_needed = 64; // 256KB / 4KB
  for (uint32_t i = 1; i < pages_needed; i++) {
    uint32_t page_virt = virt_addr + (i * PAGE_SIZE);
    uint32_t page_phys = e1000_device.mem_base + (i * PAGE_SIZE);

    if (!mmu_map_page(page_virt, page_phys,
                      PAGE_PRESENT | PAGE_RW | PAGE_CACHE_DISABLE)) {
      serial_printf(COM1_BASE, "[E1000] Warning: Failed to map page %u\r\n", i);
      // Continuar con menos páginas
      break;
    }
  }

  e1000_device.mem_virt = (uint8_t *)virt_addr;

  // Flushear TLB
  __asm__ volatile("movl %%cr3, %%eax; movl %%eax, %%cr3" ::: "eax");

  serial_printf(COM1_BASE,
                "[E1000] MMIO mapped: phys=0x%08x -> virt=0x%08x\r\n",
                e1000_device.mem_base, virt_addr);

  return true;
}

static bool e1000_alloc_buffers(void) {
  // Allocar descriptores de transmisión
  e1000_device.tx_descs = (e1000_tx_desc_t *)kernel_malloc(
      sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

  if (!e1000_device.tx_descs) {
    terminal_puts(&main_terminal,
                  "[E1000] Failed to allocate TX descriptors\r\n");
    return false;
  }

  memset(e1000_device.tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

  // Allocar descriptores de recepción
  e1000_device.rx_descs = (e1000_rx_desc_t *)kernel_malloc(
      sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);

  if (!e1000_device.rx_descs) {
    terminal_puts(&main_terminal,
                  "[E1000] Failed to allocate RX descriptors\r\n");
    kernel_free(e1000_device.tx_descs);
    return false;
  }

  memset(e1000_device.rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);

  // Allocar buffers de transmisión
  for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
    e1000_device.tx_buffers[i] = (uint8_t *)kernel_malloc(E1000_MAX_PKT_SIZE);
    if (!e1000_device.tx_buffers[i]) {
      terminal_puts(&main_terminal,
                    "[E1000] Failed to allocate TX buffers\r\n");
      // Liberar todo
      for (int j = 0; j < i; j++) {
        kernel_free(e1000_device.tx_buffers[j]);
      }
      kernel_free(e1000_device.tx_descs);
      kernel_free(e1000_device.rx_descs);
      return false;
    }
  }

  // Allocar buffers de recepción
  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    e1000_device.rx_buffers[i] = (uint8_t *)kernel_malloc(E1000_MAX_PKT_SIZE);
    if (!e1000_device.rx_buffers[i]) {
      terminal_puts(&main_terminal,
                    "[E1000] Failed to allocate RX buffers\r\n");
      // Liberar todo
      for (int j = 0; j < E1000_NUM_TX_DESC; j++) {
        kernel_free(e1000_device.tx_buffers[j]);
      }
      for (int j = 0; j < i; j++) {
        kernel_free(e1000_device.rx_buffers[j]);
      }
      kernel_free(e1000_device.tx_descs);
      kernel_free(e1000_device.rx_descs);
      return false;
    }
  }

  serial_printf(COM1_BASE, "[E1000] Allocated %d TX and %d RX descriptors\r\n",
                E1000_NUM_TX_DESC, E1000_NUM_RX_DESC);

  return true;
}

static void e1000_read_mac(void) {
  // Leer MAC address de los registros RAL/RAH
  uint32_t mac_low = e1000_read_reg(E1000_REG_RAL);
  uint32_t mac_high = e1000_read_reg(E1000_REG_RAH);

  e1000_device.mac_addr[0] = (mac_low >> 0) & 0xFF;
  e1000_device.mac_addr[1] = (mac_low >> 8) & 0xFF;
  e1000_device.mac_addr[2] = (mac_low >> 16) & 0xFF;
  e1000_device.mac_addr[3] = (mac_low >> 24) & 0xFF;
  e1000_device.mac_addr[4] = (mac_high >> 0) & 0xFF;
  e1000_device.mac_addr[5] = (mac_high >> 8) & 0xFF;

  serial_printf(COM1_BASE,
                "[E1000] MAC address: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                e1000_device.mac_addr[0], e1000_device.mac_addr[1],
                e1000_device.mac_addr[2], e1000_device.mac_addr[3],
                e1000_device.mac_addr[4], e1000_device.mac_addr[5]);
}

static void e1000_reset(void) {
  serial_printf(COM1_BASE, "[E1000] Resetting device...\r\n");

  // 0. Verificar si podemos leer del dispositivo
  uint32_t status = e1000_read_reg(E1000_REG_STATUS);
  if (status == 0xFFFFFFFF) {
    terminal_puts(&main_terminal, "[E1000] ERROR: Device reads 0xFFFFFFFF. "
                                  "MMIO mapping might be broken!\r\n");
    return;
  }

  // 1. MASTER DISABLE (Recomendado por Intel para evitar corrupción de DMA
  // durante reset)
  serial_printf(COM1_BASE, "[E1000] Disabling master...\r\n");
  uint32_t ctrl = e1000_read_reg(E1000_REG_CTRL);

  // Algunos dispositivos modernos necesitan preservar bits o realizar esta
  // secuencia
  e1000_write_reg(E1000_REG_CTRL,
                  ctrl | (1 << 31)); // GIO Master Disable (bit 31)

  // Esperar a que el Master Disable se complete (Status bit 19 se limpia)
  int master_timeout = 1000;
  while (master_timeout-- > 0) {
    if (!(e1000_read_reg(E1000_REG_STATUS) & (1 << 19)))
      break;
    for (int i = 0; i < 100; i++)
      io_wait();
  }

  if (master_timeout <= 0) {
    terminal_puts(&main_terminal,
                  "[E1000] WARNING: Master Disable timeout\r\n");
  }

  // 2. Iniciar software reset
  serial_printf(COM1_BASE, "[E1000] Issuing software reset...\r\n");
  ctrl = e1000_read_reg(E1000_REG_CTRL);
  e1000_write_reg(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);

  // 3. Esperar a que el hardware limpie el bit RST
  // Importante: Un delay mínimo de 1-2us es recomendado antes de empezar a leer
  for (int i = 0; i < 1000; i++)
    io_wait();

  int timeout = 10000; // 10k iteraciones con delay son más que suficientes
  while (timeout > 0) {
    uint32_t current_ctrl = e1000_read_reg(E1000_REG_CTRL);

    // Si la lectura devuelve 0xFFFFFFFF, algo fue muy mal
    if (current_ctrl == 0xFFFFFFFF) {
      terminal_puts(&main_terminal,
                    "[E1000] ERROR: Bus hang detected during reset!\r\n");
      break;
    }

    if (!(current_ctrl & E1000_CTRL_RST))
      break;

    for (int i = 0; i < 50; i++)
      io_wait(); // Pequeño delay entre lecturas
    timeout--;
  }

  if (timeout <= 0) {
    terminal_puts(&main_terminal,
                  "[E1000] ERROR: Reset timed out (RST bit stuck)!\r\n");
  } else {
    serial_printf(COM1_BASE, "[E1000] Reset complete\r\n");
  }

  // 4. Esperar un poco más para que la EEPROM se recargue
  for (int i = 0; i < 5000; i++)
    io_wait();
}

static void e1000_init_rx(void) {
  serial_printf(COM1_BASE, "[E1000] Initializing receive...\r\n");

  // Configurar receive descriptor ring
  uint32_t rx_desc_phys = (uint32_t)e1000_device.rx_descs;
  e1000_write_reg(E1000_REG_RDBAL, rx_desc_phys & 0xFFFFFFFF);
  e1000_write_reg(E1000_REG_RDBAH, 0); // 32-bit address, high part is 0
  e1000_write_reg(E1000_REG_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);

  // Configurar buffers de recepción
  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    // Asegurar que usamos dirección física real
    uint32_t buffer_phys =
        mmu_virtual_to_physical((uint32_t)e1000_device.rx_buffers[i]);
    e1000_device.rx_descs[i].buffer_addr = (uint64_t)buffer_phys;
    e1000_device.rx_descs[i].status = 0;
  }

  // Inicializar índices
  e1000_write_reg(E1000_REG_RDH, 0);
  e1000_write_reg(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
  e1000_device.rx_curr = 0;

  // Configurar receive control
  uint32_t rctl = e1000_read_reg(E1000_REG_RCTL);
  rctl |= E1000_RCTL_EN;          // Habilitar recepción
  rctl |= E1000_RCTL_BAM;         // Aceptar broadcast
  rctl |= E1000_RCTL_UPE;         // Unicast Promiscuous Mode (Fix for some VMs)
  rctl |= E1000_RCTL_SECRC;       // Strip Ethernet CRC
  rctl |= E1000_RCTL_LPE;         // Permitir paquetes largos
  rctl &= ~E1000_RCTL_BSIZE_2048; // Buffer size
  rctl |= E1000_RCTL_BSIZE_2048;  // 2048 bytes

  e1000_write_reg(E1000_REG_RCTL, rctl);

  serial_printf(COM1_BASE, "[E1000] Receive initialized\r\n");
}

static void e1000_init_tx(void) {
  serial_printf(COM1_BASE, "[E1000] Initializing transmit...\r\n");

  // 1. Resetear transmisión primero
  uint32_t tctl = e1000_read_reg(E1000_REG_TCTL);
  tctl &= ~E1000_TCTL_EN; // Deshabilitar transmisión temporalmente
  e1000_write_reg(E1000_REG_TCTL, tctl);

  // Esperar a que se deshabilite
  for (int i = 0; i < 1000; i++) {
    if (!(e1000_read_reg(E1000_REG_TCTL) & E1000_TCTL_EN)) {
      break;
    }
  }

  // 2. Configurar transmit descriptor ring con dirección física CORRECTA
  // Necesitamos la dirección física de los descriptores, no virtual
  uint32_t tx_desc_phys = (uint32_t)e1000_device.tx_descs;

  // IMPORTANTE: Si usas paginación, necesitas convertir dirección virtual a
  // física Para ahora, asumimos que kernel_malloc devuelve dirección física (en
  // sistemas sin paginación o con identidad mapping)

  e1000_write_reg(E1000_REG_TDBAL, tx_desc_phys & 0xFFFFFFFF);
  e1000_write_reg(E1000_REG_TDBAH, 0); // Para sistemas 32-bit, high bits son 0
  e1000_write_reg(E1000_REG_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

  // 3. Inicializar descriptores CORRECTAMENTE
  for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
    e1000_tx_desc_t *desc = &e1000_device.tx_descs[i];

    // Resetear descriptor
    memset(desc, 0, sizeof(e1000_tx_desc_t));

    // Configurar dirección física del buffer
    // IMPORTANTE: kernel_malloc devuelve dirección virtual
    // Necesitamos convertirla a física
    uint32_t buffer_virt = (uint32_t)e1000_device.tx_buffers[i];
    uint32_t buffer_phys = mmu_virtual_to_physical(buffer_virt);

    desc->buffer_addr = (uint64_t)buffer_phys;
    desc->length = 0;
    desc->cmd = 0;
    desc->status = E1000_TXD_STAT_DD; // Marcar como "done" inicialmente
    desc->special = 0;
    desc->cso = 0;
    desc->css = 0;
  }

  // 4. Inicializar índices
  e1000_write_reg(E1000_REG_TDH, 0);
  e1000_write_reg(E1000_REG_TDT, 0);
  e1000_device.tx_curr = 0;

  // 5. Configurar transmit control
  tctl = e1000_read_reg(E1000_REG_TCTL);
  tctl = E1000_TCTL_EN |                  // Habilitar transmisión
         E1000_TCTL_PSP |                 // Pad short packets
         (0x10 << E1000_TCTL_CT_SHIFT) |  // Collision threshold = 16
         (0x40 << E1000_TCTL_COLD_SHIFT); // Collision distance = 64
  e1000_write_reg(E1000_REG_TCTL, tctl);

  // 6. Configurar Inter Packet Gap
  e1000_write_reg(E1000_REG_TIPG, 0x0060200A);

  // 7. Verificar que se inicializó correctamente
  uint32_t tdh = e1000_read_reg(E1000_REG_TDH);
  uint32_t tdt = e1000_read_reg(E1000_REG_TDT);

  serial_printf(COM1_BASE, "[E1000] TX initialized: TDH=%u, TDT=%u\r\n", tdh,
                tdt);
  serial_printf(COM1_BASE, "[E1000] TX descriptors at phys=0x%08x\r\n",
                tx_desc_phys);
}

static void e1000_enable_interrupts(void) {
  // Limpiar interrupciones pendientes
  e1000_write_reg(E1000_REG_ICR, 0xFFFFFFFF);

  // Habilitar interrupciones específicas
  /*
  uint32_t ims = E1000_ICR_TXDW | // Transmit descriptor written back
                 E1000_ICR_RXT0 | // Receive timer
                 E1000_ICR_RXO |  // Receive overrun
                 E1000_ICR_LSC;   // Link status change

  e1000_write_reg(E1000_REG_IMS, ims);
  */

  // Disable interrupts for polling mode stability
  e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFF);

  serial_printf(COM1_BASE, "[E1000] Interrupts DISABLED (Polling mode)\r\n");
}

bool e1000_init(void) {
  serial_printf(COM1_BASE, "\r\n=== Intel E1000 Network Driver ===\r\n");

  // 1. Detectar dispositivo
  if (!e1000_detect_device()) {
    return false;
  }

  // 2. Mapear memoria si es necesario
  if (e1000_device.mem_base && !e1000_map_memory()) {
    return false;
  }

  // 3. Resetear dispositivo
  e1000_reset();

  // 4. Allocar buffers
  if (!e1000_alloc_buffers()) {
    return false;
  }

  // 5. Leer MAC address
  e1000_read_mac();

  // 6. Inicializar recepción
  e1000_init_rx();

  // 7. Inicializar transmisión
  e1000_init_tx();

  // 8. Configurar control general
  uint32_t ctrl = e1000_read_reg(E1000_REG_CTRL);
  ctrl |= E1000_CTRL_SLU;      // Set Link Up
  ctrl &= ~E1000_CTRL_LRST;    // No link reset
  ctrl &= ~E1000_CTRL_PHY_RST; // No PHY reset
  e1000_write_reg(E1000_REG_CTRL, ctrl);

  // 9. Habilitar interrupciones
  e1000_enable_interrupts();

  // 10. Verificar link status
  uint32_t status = e1000_read_reg(E1000_REG_STATUS);
  e1000_device.link_up = (status & 0x02) ? true : false;

  serial_printf(COM1_BASE, "[E1000] Link status: %s\r\n",
                e1000_device.link_up ? "UP" : "DOWN");

  e1000_device.initialized = true;
  e1000_device.tx_packets = 0;
  e1000_device.rx_packets = 0;

  serial_printf(COM1_BASE, "[E1000] Driver initialized successfully!\r\n");

  return true;
}

// ===================================================
// FUNCIONES DE TRANSMISIÓN
// ===================================================

bool e1000_send_packet(const uint8_t *data, uint32_t length) {
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  if (!e1000_device.initialized) {
    terminal_puts(&main_terminal, "[E1000] Device not initialized\r\n");
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return false;
  }

  if (length > E1000_MAX_PKT_SIZE || length == 0) {
    terminal_printf(&main_terminal, "[E1000] Invalid packet length: %u\r\n",
                    length);
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return false;
  }

  // Verificar si el link está up
  if (!e1000_is_link_up()) {
    terminal_puts(&main_terminal, "[E1000] Link is down, cannot send\r\n");
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return false;
  }

  // Obtener descriptor actual
  uint32_t tx_idx = e1000_device.tx_curr;
  e1000_tx_desc_t *desc = &e1000_device.tx_descs[tx_idx];

  // DEBUG: Mostrar estado del descriptor
  // terminal_printf(&main_terminal, "[E1000] TX idx=%u, status=0x%02x\r\n",
  //                tx_idx, desc->status);

  // Esperar a que el descriptor esté listo (DD bit = 1)
  int timeout = 1000000;
  while (!(desc->status & E1000_TXD_STAT_DD) && timeout-- > 0) {
    // Puede necesitar actualizar el status desde el hardware
    // En algunos casos necesitas leer el registro de nuevo
    __asm__ volatile("pause");
  }

  if (timeout <= 0) {
    terminal_printf(&main_terminal, "[E1000] TX timeout, resetting TX...\r\n");

    // Resetear transmisión
    uint32_t tctl = e1000_read_reg(E1000_REG_TCTL);
    tctl &= ~E1000_TCTL_EN;
    e1000_write_reg(E1000_REG_TCTL, tctl);

    // Re-inicializar
    e1000_init_tx();

    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return false;
  }

  // Copiar datos al buffer
  memcpy(e1000_device.tx_buffers[tx_idx], data, length);

  // Configurar descriptor
  desc->length = length;
  desc->cmd = E1000_TXD_CMD_EOP |  // End of packet
              E1000_TXD_CMD_IFCS | // Insert FCS (CRC)
              E1000_TXD_CMD_RS;    // Report status

  // Limpiar status bit DD (lo hará el hardware cuando complete)
  desc->status &= ~E1000_TXD_STAT_DD;

  // Memory barrier para asegurar que los datos están escritos
  __asm__ volatile("" ::: "memory");

  // Actualizar tail pointer - ¡IMPORTANTE! Esto notifica al hardware
  uint32_t next_idx = (tx_idx + 1) % E1000_NUM_TX_DESC;
  e1000_write_reg(E1000_REG_TDT, next_idx);

  // Actualizar índice
  e1000_device.tx_curr = next_idx;

  // Estadísticas
  e1000_device.tx_packets++;
  e1000_device.tx_bytes += length;

  // terminal_printf(&main_terminal,
  //                 "[E1000] Packet sent: %u bytes, next_idx=%u\r\n", length,
  //                 next_idx);

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
  return true;
}

// ===================================================
// FUNCIONES DE RECEPCIÓN
// ===================================================

uint32_t e1000_receive_packet(uint8_t *buffer, uint32_t max_len) {
  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  if (!e1000_device.initialized) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  uint32_t rx_idx = e1000_device.rx_curr;
  // Usar volatile para asegurar que leemos desde memoria (RAM) y no cache
  volatile e1000_rx_desc_t *desc =
      (volatile e1000_rx_desc_t *)&e1000_device.rx_descs[rx_idx];

  // Verificar si hay paquete disponible (Check DD bit)
  if (!(desc->status & E1000_RXD_STAT_DD)) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  // Obtener longitud
  uint16_t length = desc->length;
  if (length == 0 || length > max_len) {
    // Limpiar descriptor
    desc->status = 0;

    // Avanzar índice SW
    uint32_t old_rx_idx = rx_idx;
    e1000_device.rx_curr = (rx_idx + 1) % E1000_NUM_RX_DESC;

    // Notificar al hardware que el descriptor "old_rx_idx" está libre de nuevo
    // RDT debe apuntar al descriptor que el hardware NO debe usar todavía.
    // Al escribir old_rx_idx, liberamos el descriptor para que el hardware lo
    // use
    e1000_write_reg(E1000_REG_RDT, old_rx_idx);

    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  // Copiar datos del buffer DMA al buffer del usuario
  memcpy(buffer, e1000_device.rx_buffers[rx_idx], length);

  // Limpiar descriptor para reuso
  desc->status = 0;

  // Actualizar tail pointer
  // Avanzar índice SW y actualizar RDT al índice que acabamos de procesar
  uint32_t old_rx_idx = rx_idx;
  e1000_device.rx_curr = (rx_idx + 1) % E1000_NUM_RX_DESC;
  e1000_write_reg(E1000_REG_RDT, old_rx_idx);

  // Estadísticas
  e1000_device.rx_packets++;
  e1000_device.rx_bytes += length;

  // DEBUG: Packet received
  // terminal_printf(&main_terminal, "[E1000] RX packet: %u bytes\r\n", length);

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
  return length;
}

// ===================================================
// FUNCIONES DE UTILIDAD
// ===================================================

void e1000_get_mac(uint8_t *mac) {
  if (mac && e1000_device.initialized) {
    memcpy(mac, e1000_device.mac_addr, 6);
  }
}

bool e1000_is_link_up(void) {
  if (!e1000_device.initialized) {
    return false;
  }

  // Verificar link status
  uint32_t status = e1000_read_reg(E1000_REG_STATUS);
  e1000_device.link_up = (status & 0x02) ? true : false;

  return e1000_device.link_up;
}

void e1000_handle_interrupt(void) {
  // Leear interrupt cause
  uint32_t icr = e1000_read_reg(E1000_REG_ICR);

  // Limpiar interrupciones
  e1000_write_reg(E1000_REG_ICR, icr);

  if (icr & E1000_ICR_LSC) {
    // Cambio en link status
    e1000_is_link_up();
    terminal_printf(&main_terminal, "[E1000] Link status changed: %s\r\n",
                    e1000_device.link_up ? "UP" : "DOWN");
  }

  if (icr & E1000_ICR_RXT0) {
    // Timer de recepción
    // Podemos usar esto para polling si no queremos interrupciones
  }

  if (icr & E1000_ICR_RXO) {
    // Overrun de recepción
    e1000_device.rx_errors++;
    terminal_puts(&main_terminal, "[E1000] Receive overrun\r\n");
  }

  if (icr & E1000_ICR_TXDW) {
    // Descriptor de transmisión escrito
    // Podemos liberar buffers aquí
  }
}

void e1000_print_stats(void) {
  terminal_puts(&main_terminal, "\r\n=== E1000 Statistics ===\r\n");
  terminal_printf(&main_terminal, "TX Packets: %u\r\n",
                  e1000_device.tx_packets);
  terminal_printf(&main_terminal, "RX Packets: %u\r\n",
                  e1000_device.rx_packets);
  terminal_printf(&main_terminal, "TX Bytes: %u\r\n", e1000_device.tx_bytes);
  terminal_printf(&main_terminal, "RX Bytes: %u\r\n", e1000_device.rx_bytes);
  terminal_printf(&main_terminal, "TX Errors: %u\r\n", e1000_device.tx_errors);
  terminal_printf(&main_terminal, "RX Errors: %u\r\n", e1000_device.rx_errors);
  terminal_printf(&main_terminal, "Link: %s\r\n",
                  e1000_device.link_up ? "UP" : "DOWN");
}

void e1000_check_status(void) {
  if (!e1000_device.initialized) {
    return;
  }

  terminal_puts(&main_terminal, "\r\n=== E1000 Status Check ===\r\n");

  // 1. Link status
  uint32_t status = e1000_read_reg(E1000_REG_STATUS);
  terminal_printf(&main_terminal, "Status register: 0x%08x\r\n", status);
  terminal_printf(&main_terminal, "Link up: %s\r\n",
                  (status & 0x02) ? "YES" : "NO");
  terminal_printf(&main_terminal, "Full duplex: %s\r\n",
                  (status & 0x01) ? "YES" : "NO");
  terminal_printf(&main_terminal, "Speed: ");
  switch ((status >> 6) & 0x03) {
  case 0:
    terminal_puts(&main_terminal, "10 Mbps\r\n");
    break;
  case 1:
    terminal_puts(&main_terminal, "100 Mbps\r\n");
    break;
  case 2:
    terminal_puts(&main_terminal, "1000 Mbps\r\n");
    break;
  default:
    terminal_puts(&main_terminal, "Unknown\r\n");
    break;
  }

  // 2. Control registers
  uint32_t ctrl = e1000_read_reg(E1000_REG_CTRL);
  terminal_printf(&main_terminal, "Control: 0x%08x\r\n", ctrl);

  uint32_t rctl = e1000_read_reg(E1000_REG_RCTL);
  terminal_printf(&main_terminal, "Receive control: 0x%08x\r\n", rctl);

  uint32_t tctl = e1000_read_reg(E1000_REG_TCTL);
  terminal_printf(&main_terminal, "Transmit control: 0x%08x\r\n", tctl);

  // 3. Descriptor ring status
  uint32_t tdh = e1000_read_reg(E1000_REG_TDH);
  uint32_t tdt = e1000_read_reg(E1000_REG_TDT);
  uint32_t rdh = e1000_read_reg(E1000_REG_RDH);
  uint32_t rdt = e1000_read_reg(E1000_REG_RDT);

  terminal_printf(&main_terminal, "TX: Head=%u, Tail=%u, Curr=%u\r\n", tdh, tdt,
                  e1000_device.tx_curr);
  terminal_printf(&main_terminal, "RX: Head=%u, Tail=%u, Curr=%u\r\n", rdh, rdt,
                  e1000_device.rx_curr);

  // 4. Interrupt status
  uint32_t icr = e1000_read_reg(E1000_REG_ICR);
  terminal_printf(&main_terminal, "Interrupt cause: 0x%08x\r\n", icr);
}

// ===================================================
// INTEGRACIÓN CON DRIVER SYSTEM
// ===================================================

static int e1000_driver_init(driver_instance_t *drv, void *config) {
  (void)config;

  if (!drv) {
    return -1;
  }

  terminal_printf(&main_terminal,
                  "[E1000] Initializing driver instance: %s\r\n", drv->name);

  return e1000_init() ? 0 : -1;
}

static int e1000_driver_start(driver_instance_t *drv) {
  if (!drv) {
    return -1;
  }

  terminal_printf(&main_terminal, "[E1000] Starting driver: %s\r\n", drv->name);

  if (!e1000_device.initialized) {
    terminal_puts(&main_terminal, "[E1000] ERROR: Device not initialized\r\n");
    return -1;
  }

  // Habilitar recepción y transmisión
  uint32_t rctl = e1000_read_reg(E1000_REG_RCTL);
  rctl |= E1000_RCTL_EN;
  e1000_write_reg(E1000_REG_RCTL, rctl);

  uint32_t tctl = e1000_read_reg(E1000_REG_TCTL);
  tctl |= E1000_TCTL_EN;
  e1000_write_reg(E1000_REG_TCTL, tctl);

  return 0;
}

static int e1000_driver_stop(driver_instance_t *drv) {
  (void)drv;

  // Deshabilitar recepción y transmisión
  uint32_t rctl = e1000_read_reg(E1000_REG_RCTL);
  rctl &= ~E1000_RCTL_EN;
  e1000_write_reg(E1000_REG_RCTL, rctl);

  uint32_t tctl = e1000_read_reg(E1000_REG_TCTL);
  tctl &= ~E1000_TCTL_EN;
  e1000_write_reg(E1000_REG_TCTL, tctl);

  return 0;
}

static int e1000_driver_cleanup(driver_instance_t *drv) {
  (void)drv;

  terminal_puts(&main_terminal, "[E1000] Cleaning up driver\r\n");

  // Liberar buffers
  if (e1000_device.tx_descs) {
    kernel_free(e1000_device.tx_descs);
  }

  if (e1000_device.rx_descs) {
    kernel_free(e1000_device.rx_descs);
  }

  for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
    if (e1000_device.tx_buffers[i]) {
      kernel_free(e1000_device.tx_buffers[i]);
    }
  }

  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    if (e1000_device.rx_buffers[i]) {
      kernel_free(e1000_device.rx_buffers[i]);
    }
  }

  memset(&e1000_device, 0, sizeof(e1000_device_t));

  return 0;
}

static int e1000_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
  (void)drv;

  switch (cmd) {
  case 0x4001: // Get MAC address
    if (arg) {
      e1000_get_mac((uint8_t *)arg);
      return 0;
    }
    break;

  case 0x4002: // Get link status
    if (arg) {
      *(bool *)arg = e1000_is_link_up();
      return 0;
    }
    break;

  case 0x4003: // Send packet
    if (arg) {
      struct {
        uint8_t *data;
        uint32_t length;
      } *pkt = arg;
      return e1000_send_packet(pkt->data, pkt->length) ? 0 : -1;
    }
    break;

  case 0x4004: // Receive packet
    if (arg) {
      struct {
        uint8_t *buffer;
        uint32_t max_len;
        uint32_t *actual_len;
      } *pkt = arg;
      uint32_t len = e1000_receive_packet(pkt->buffer, pkt->max_len);
      if (pkt->actual_len) {
        *pkt->actual_len = len;
      }
      return len > 0 ? 0 : -1;
    }
    break;

  case 0x4005: // Get statistics
    e1000_print_stats();
    return 0;

  default:
    break;
  }

  return -1;
}

static driver_ops_t e1000_driver_ops = {.init = e1000_driver_init,
                                        .start = e1000_driver_start,
                                        .stop = e1000_driver_stop,
                                        .cleanup = e1000_driver_cleanup,
                                        .ioctl = e1000_driver_ioctl,
                                        .read = NULL,
                                        .write = NULL,
                                        .load_data = NULL};

static driver_type_info_t e1000_driver_type = {.type = DRIVER_TYPE_NETWORK,
                                               .type_name = "e1000",
                                               .version = "1.0.0",
                                               .priv_data_size = 0,
                                               .default_ops = &e1000_driver_ops,
                                               .validate_data = NULL,
                                               .print_info = NULL};

int e1000_driver_register_type(void) {
  return driver_register_type(&e1000_driver_type);
}

driver_instance_t *e1000_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_NETWORK, name);
}