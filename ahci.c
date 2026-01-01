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
  terminal_puts(&main_terminal, "AHCI: Scanning for AHCI controllers...\r\n");

  // Lista priorizada de controladores conocidos (Intel primero)
  static const struct {
    uint16_t vendor;
    uint16_t device;
    const char *name;
    bool is_intel;
  } known_controllers[] = {// Intel ICH9 Family (crítico para tu caso)
                           {0x8086, 0x2922, "Intel ICH9M AHCI", true},
                           {0x8086, 0x2929, "Intel ICH9 AHCI", true},
                           {0x8086, 0x2829, "Intel ICH8 AHCI", true},
                           {0x8086, 0x2681, "Intel ICH6 AHCI", true},
                           {0x8086, 0x2652, "Intel ICH5 AHCI", true},
                           {0x8086, 0x3A22, "Intel ICH10 AHCI", true},
                           {0x8086, 0x3B22, "Intel PCH AHCI", true},
                           {0x8086, 0x3B29, "Intel PCH Mobile AHCI", true},

                           // Otros fabricantes
                           {0x1002, 0x4391, "AMD SB7xx/SB8xx AHCI", false},
                           {0x1002, 0x4392, "AMD SB7xx/SB8xx RAID", false},
                           {0x1002, 0x4393, "AMD SB7xx/SB8xx IDE", false},
                           {0x1002, 0x4394, "AMD SB7xx/SB8xx RAID", false},
                           {0x10DE, 0x03F6, "NVIDIA MCP55 AHCI", false},
                           {0x10DE, 0x03F7, "NVIDIA MCP55 RAID", false},
                           {0x10DE, 0x0448, "NVIDIA MCP65 AHCI", false},
                           {0x10DE, 0x0449, "NVIDIA MCP65 RAID", false},
                           {0x1B4B, 0x9172, "Marvell 88SE9172 AHCI", false},
                           {0x1B4B, 0x91A3, "Marvell 88SE91A3 AHCI", false},
                           {0, 0, NULL, false}};

  // Método 1: Buscar controladores Intel específicamente (prioridad alta para
  // ICH9)
  terminal_puts(&main_terminal, "AHCI: Checking for Intel controllers...\r\n");
  for (int i = 0; known_controllers[i].vendor != 0; i++) {
    if (known_controllers[i].is_intel) {
      ahci_controller.pci_device = pci_find_device(known_controllers[i].vendor,
                                                   known_controllers[i].device);
      if (ahci_controller.pci_device) {
        terminal_printf(&main_terminal, "AHCI: Found %s\r\n",
                        known_controllers[i].name);
        break;
      }
    }
  }

  // Método 2: Si no encontramos Intel, buscar por clase genérica
  if (!ahci_controller.pci_device) {
    terminal_puts(
        &main_terminal,
        "AHCI: No Intel controllers found, trying generic AHCI class...\r\n");
    ahci_controller.pci_device =
        pci_find_device_by_class(AHCI_PCI_CLASS, AHCI_PCI_SUBCLASS);

    if (ahci_controller.pci_device) {
      terminal_puts(&main_terminal, "AHCI: Found generic AHCI controller\r\n");
    }
  }

  // Método 3: Buscar cualquier controlador SATA con program interface 0x01
  // (AHCI)
  if (!ahci_controller.pci_device) {
    terminal_puts(&main_terminal,
                  "AHCI: Scanning all PCI devices for AHCI...\r\n");

    for (uint32_t i = 0; i < pci_device_count; i++) {
      pci_device_t *dev = &pci_devices[i];

      // Buscar dispositivos de almacenamiento masivo (class 0x01)
      if (dev->class_code == PCI_CLASS_STORAGE) {
        // Verificar si es SATA (subclass 0x06)
        if (dev->subclass == 0x06) {
          // Verificar program interface (0x01 = AHCI, 0x02 = RAID)
          if (dev->prog_if == 0x01 || dev->prog_if == 0x02) {
            ahci_controller.pci_device = dev;
            terminal_printf(
                &main_terminal,
                "AHCI: Found SATA controller via manual scan: %04x:%04x\r\n",
                dev->vendor_id, dev->device_id);
            break;
          }
        }
      }
    }
  }

  // Método 4: Buscar específicamente AMD
  if (!ahci_controller.pci_device) {
    terminal_puts(&main_terminal, "AHCI: Checking for AMD controllers...\r\n");
    for (int i = 0; known_controllers[i].vendor != 0; i++) {
      if (known_controllers[i].vendor == 0x1002) {
        ahci_controller.pci_device = pci_find_device(
            known_controllers[i].vendor, known_controllers[i].device);
        if (ahci_controller.pci_device) {
          terminal_printf(&main_terminal, "AHCI: Found %s\r\n",
                          known_controllers[i].name);
          break;
        }
      }
    }
  }

  if (!ahci_controller.pci_device) {
    terminal_puts(&main_terminal, "AHCI: No AHCI controller found\r\n");
    return false;
  }

  terminal_printf(
      &main_terminal, "AHCI: Controller %04x:%04x at %02x:%02x.%x\r\n",
      ahci_controller.pci_device->vendor_id,
      ahci_controller.pci_device->device_id, ahci_controller.pci_device->bus,
      ahci_controller.pci_device->device, ahci_controller.pci_device->function);

  // Registrar información detallada del controlador
  terminal_printf(&main_terminal,
                  "AHCI: Class %02x, Subclass %02x, Prog IF %02x\r\n",
                  ahci_controller.pci_device->class_code,
                  ahci_controller.pci_device->subclass,
                  ahci_controller.pci_device->prog_if);

  return true;
}

bool ahci_initialize_controller(void) {
  pci_device_t *pci_dev = ahci_controller.pci_device;

  terminal_printf(&main_terminal, "AHCI: Initializing controller %04x:%04x\r\n",
                  pci_dev->vendor_id, pci_dev->device_id);

  // =================================================================
  // FASE 1: CONFIGURACIÓN PCI
  // =================================================================

  // 1. Habilitar bus mastering y memory space
  pci_enable_bus_mastering(pci_dev);
  pci_enable_memory_space(pci_dev);

  // 2. Obtener ABAR - intentar todas las BARs posibles
  uint32_t abar_physical = 0;
  uint32_t abar_size = 0;

  // Probar BAR5 (estándar para AHCI)
  if (pci_dev->bars[5].is_valid) {
    abar_physical = pci_dev->bars[5].address;
    abar_size = pci_dev->bars[5].size;
    terminal_puts(&main_terminal, "AHCI: Using BAR5 for ABAR\r\n");
  }
  // Fallback a BAR0 (algunos controladores)
  else if (pci_dev->bars[0].is_valid &&
           pci_dev->bars[0].type == PCI_BAR_TYPE_MEMORY) {
    abar_physical = pci_dev->bars[0].address;
    abar_size = pci_dev->bars[0].size;
    terminal_puts(&main_terminal, "AHCI: Using BAR0 for ABAR (fallback)\r\n");
  }
  // Fallback a BAR4 (otros casos)
  else if (pci_dev->bars[4].is_valid) {
    abar_physical = pci_dev->bars[4].address;
    abar_size = pci_dev->bars[4].size;
    terminal_puts(&main_terminal, "AHCI: Using BAR4 for ABAR (fallback)\r\n");
  } else {
    terminal_puts(&main_terminal, "AHCI: No valid BAR found for ABAR\r\n");
    return false;
  }

  // Validar que la dirección no sea 0
  if (abar_physical == 0) {
    terminal_puts(&main_terminal, "AHCI: ABAR address is 0\r\n");
    return false;
  }

  ahci_controller.abar_physical = abar_physical;

  // =================================================================
  // FASE 2: MAPEO DE MEMORIA
  // =================================================================

  // 3. Mapear ABAR a memoria virtual
  // ICH9 necesita al menos 4KB mapeados
  if (abar_size < 0x1000) {
    abar_size = 0x1000; // 4KB mínimo
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
                  "AHCI: ABAR mapped - phys=0x%08x, virt=0x%08x, size=0x%x\r\n",
                  ahci_controller.abar_physical, abar_virtual, abar_size);

  // =================================================================
  // FASE 3: BIOS HANDOFF (SÓLO SI ES NECESARIO)
  // =================================================================

  // 4. Intentar BIOS handoff solo si está marcado como owned por BIOS
  uint32_t bohc = ahci_controller.abar->bohc;
  if (bohc & 0x01) { // BIOS owns HBA
    terminal_puts(&main_terminal,
                  "AHCI: BIOS owns HBA, requesting handoff...\r\n");

    // Método suave: solicitar handoff
    ahci_controller.abar->bohc |= 0x02; // OS ownership request

    // Esperar máximo 100ms (tiempo razonable)
    uint32_t timeout = 10000; // 10,000 * 10µs = 100ms
    while ((ahci_controller.abar->bohc & 0x01) && timeout--) {
      for (volatile int i = 0; i < 10; i++)
        __asm__ volatile("pause");
    }

    if (ahci_controller.abar->bohc & 0x01) {
      terminal_puts(&main_terminal,
                    "AHCI: WARNING - BIOS handoff timeout, forcing...\r\n");

      // Forzar handoff para ICH9
      ahci_controller.abar->bohc &= ~0x01; // Clear BIOS ownership
      ahci_controller.abar->bohc |= 0x02;  // Set OS ownership

      // Pequeña pausa
      for (volatile int i = 0; i < 1000; i++)
        __asm__ volatile("pause");
    } else {
      terminal_puts(&main_terminal, "AHCI: BIOS handoff completed\r\n");
    }
  }

  // =================================================================
  // FASE 4: HABILITAR AHCI MODE (CRÍTICO PARA ICH9)
  // =================================================================

  // 5. Habilitar modo AHCI inmediatamente
  terminal_puts(&main_terminal, "AHCI: Enabling AHCI mode...\r\n");

  uint32_t ghc = ahci_controller.abar->ghc;
  if (!(ghc & AHCI_GHC_AE)) {
    ahci_controller.abar->ghc = ghc | AHCI_GHC_AE;

    // Esperar a que se estabilice (ICH9 puede necesitar esto)
    for (volatile int i = 0; i < 1000; i++)
      __asm__ volatile("pause");

    // Verificar que se habilitó
    if (ahci_controller.abar->ghc & AHCI_GHC_AE) {
      terminal_puts(&main_terminal, "AHCI: AHCI mode enabled successfully\r\n");
    } else {
      terminal_puts(&main_terminal,
                    "AHCI: ERROR - Failed to enable AHCI mode\r\n");
      return false;
    }
  } else {
    terminal_puts(&main_terminal, "AHCI: AHCI mode already enabled\r\n");
  }

  // =================================================================
  // FASE 5: LECTURA DE CAPACIDADES (DESPUÉS DE HABILITAR AHCI)
  // =================================================================

  // 6. Leer capacidades - AHORA que AHCI está habilitado
  uint32_t cap = ahci_controller.abar->cap;

  // IMPORTANTE: Leer PI después de habilitar AHCI
  ahci_controller.ports_implemented = ahci_controller.abar->pi;

  // TRUCO PARA ICH9: Si PI es 0, usar valor por defecto
  if (ahci_controller.ports_implemented == 0) {
    terminal_puts(&main_terminal,
                  "AHCI: WARNING - PI=0, using ICH9 defaults\r\n");

    // ICH9 normalmente tiene 6 puertos (bits 0-5)
    ahci_controller.ports_implemented = 0x3F; // Puertos 0-5

    // Verificar manualmente cada puerto
    for (int i = 0; i < 6; i++) {
      hba_port_t *port = (hba_port_t *)((uint8_t *)ahci_controller.abar +
                                        AHCI_PORT_BASE + (i * AHCI_PORT_SIZE));
      uint32_t sig = port->sig;

      // Si la signature no es 0 o 0xFFFFFFFF, el puerto existe
      if (sig != 0 && sig != 0xFFFFFFFF) {
        terminal_printf(&main_terminal,
                        "AHCI: Port %d has signature 0x%08x\r\n", i, sig);
      } else {
        // Marcar puerto como no presente
        ahci_controller.ports_implemented &= ~(1 << i);
      }
    }
  }

  ahci_controller.port_count = (cap & AHCI_CAP_NP_MASK) + 1;
  ahci_controller.command_slots =
      ((cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;
  ahci_controller.supports_64bit = (cap & AHCI_CAP_S64A) != 0;
  ahci_controller.supports_ncq = (cap & AHCI_CAP_SNCQ) != 0;

  // Verificar versión
  uint32_t version = ahci_controller.abar->vs;
  terminal_printf(&main_terminal, "AHCI: Version %u.%u%u\r\n",
                  (version >> 16) & 0xFFFF, (version >> 8) & 0xFF,
                  version & 0xFF);

  terminal_printf(
      &main_terminal,
      "AHCI: Capabilities - ports=%u, slots=%u, 64bit=%u, ncq=%u\r\n",
      ahci_controller.port_count, ahci_controller.command_slots,
      ahci_controller.supports_64bit, ahci_controller.supports_ncq);

  terminal_printf(&main_terminal, "AHCI: Ports implemented mask: 0x%08x\r\n",
                  ahci_controller.ports_implemented);

  // Mostrar puertos implementados en binario
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

  // Listar puertos implementados
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

  // =================================================================
  // FASE 6: HABILITAR INTERRUPCIONES
  // =================================================================

  // 7. Habilitar interrupciones globales
  ahci_controller.abar->ghc |= AHCI_GHC_IE;

  // Limpiar cualquier interrupción pendiente
  ahci_controller.abar->is = ~0U;

  terminal_puts(&main_terminal,
                "AHCI: Controller initialized successfully\r\n");

  return true;
}

bool ahci_initialize_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS) {
    terminal_printf(&main_terminal, "AHCI: Invalid port number %u\r\n",
                    port_num);
    return false;
  }

  // Verificar que el puerto esté implementado
  if (!(ahci_controller.ports_implemented & (1 << port_num))) {
    terminal_printf(&main_terminal, "AHCI: Port %u not implemented\r\n",
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

  // =================================================================
  // FASE 1: RESET COMPLETO DEL PUERTO (MEJORADO)
  // =================================================================

  // 1. Detener puerto si está corriendo - con timeout extendido
  uint32_t cmd = port->port_regs->cmd;
  if (cmd & (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE)) {
    terminal_printf(&main_terminal, "AHCI: Port %u is active, performing full reset...\r\n",
                    port_num);

    // Deshabilitar comandos
    port->port_regs->cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
    
    // Esperar a que se detenga (timeout extendido para hardware real)
    uint32_t timeout = 500000; // 500ms para hardware real
    while ((port->port_regs->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) && timeout--) {
      for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
    }

    if (port->port_regs->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) {
      terminal_printf(&main_terminal, "AHCI: WARNING - Port %u didn't stop cleanly, forcing...\r\n",
                      port_num);
      // Forzar reset
      port->port_regs->cmd |= AHCI_PORT_CMD_CLO; // Clear task file
    }
  }

  // 2. Limpiar TODOS los registros de error e interrupciones
  port->port_regs->serr = ~0U; // Clear all SERR bits
  port->port_regs->is = ~0U;   // Clear all interrupts
  
  // Esperar después de limpiar (crítico para hardware real)
  for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
                

  // =================================================================
  // FASE 2: DETECCIÓN DE DISPOSITIVO
  // =================================================================

  // 4. Verificar signature del puerto
  uint32_t sig = port->port_regs->sig;

  // Si la signature es 0xFFFFFFFF, el puerto no está físicamente presente
  if (sig == 0xFFFFFFFF) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u not physically present (SIG=0xFFFFFFFF)\r\n",
                    port_num);
    port->present = false;
    return false;
  }

  // 5. Verificar estado SATA
  uint32_t ssts = port->port_regs->ssts;
  uint8_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
  uint8_t ipm = (ssts >> AHCI_PORT_SSTS_IPM_SHIFT) & AHCI_PORT_SSTS_IPM_MASK;

  terminal_printf(
      &main_terminal,
      "AHCI: Port %u - SIG=0x%08x, SSTS=0x%08x (DET=%u, IPM=%u)\r\n", port_num,
      sig, ssts, det, ipm);

  // Verificar si hay dispositivo presente
  if (det == AHCI_PORT_DET_NONE || det == 0) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u - No device detected (DET=%u)\r\n", port_num,
                    det);

    // Intentar spin-up si está en modo slumber/partial
    if (det == AHCI_PORT_DET_PRESENT || ipm != AHCI_PORT_SSTS_IPM_ACTIVE) {
      terminal_printf(&main_terminal,
                      "AHCI: Attempting to wake device on port %u...\r\n",
                      port_num);

      // Intentar spin-up
      port->port_regs->cmd |= AHCI_PORT_CMD_SUD;

      // Esperar 100ms
      for (volatile int i = 0; i < 10000; i++)
        __asm__ volatile("pause");

      // Verificar nuevamente
      ssts = port->port_regs->ssts;
      det = ssts & AHCI_PORT_SSTS_DET_MASK;

      if (det == AHCI_PORT_DET_ESTABLISHED) {
        terminal_printf(&main_terminal,
                        "AHCI: Device woken up successfully\r\n");
      } else {
        terminal_printf(&main_terminal, "AHCI: Failed to wake device\r\n");
        port->present = false;
        return false;
      }
    } else {
      port->present = false;
      return false;
    }
  }

  port->present = true;
  port->signature = sig;

  // =================================================================
  // FASE 3: IDENTIFICACIÓN DEL TIPO DE DISPOSITIVO
  // =================================================================

  // 6. Determinar tipo de dispositivo
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

    // Para ICH9, a veces reporta signature incorrecta
    // Si DET=3 (device present) pero signature es rara, asumir SATA
    if (det == AHCI_PORT_DET_ESTABLISHED) {
      terminal_printf(&main_terminal, "AHCI: Assuming SATA due to DET=%u\r\n",
                      det);
      port->device_type = 1;
      port->signature = AHCI_SIG_ATA;
    } else {
      port->device_type = 0;
      port->present = false;
      return false;
    }
    break;
  }

  // Solo inicializar SATA y ATAPI
  if (port->device_type != 1 && port->device_type != 2) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u device type not supported\r\n", port_num);
    port->present = false;
    return false;
  }

  // =================================================================
  // FASE 4: INICIALIZACIÓN DE ESTRUCTURAS DMA
  // =================================================================

  // 7. Allocate command list (1KB aligned, 4KB para ICH9)
  port->cmd_list_buffer = dma_alloc_buffer(AHCI_CMD_SLOT_SIZE * 32, 4096);
  if (!port->cmd_list_buffer) {
    terminal_printf(&main_terminal,
                    "AHCI: Failed to allocate command list for port %u\r\n",
                    port_num);
    return false;
  }

  port->cmd_list = (hba_cmd_header_t *)port->cmd_list_buffer->virtual_address;
  memset(port->cmd_list, 0, AHCI_CMD_SLOT_SIZE * 32);

  // 8. Allocate FIS area (256 bytes aligned, 4KB para ICH9)
  port->fis_buffer = dma_alloc_buffer(AHCI_RX_FIS_SIZE, 4096);
  if (!port->fis_buffer) {
    terminal_printf(&main_terminal,
                    "AHCI: Failed to allocate FIS buffer for port %u\r\n",
                    port_num);
    dma_free_buffer(port->cmd_list_buffer);
    return false;
  }

  port->fis_base = (uint8_t *)port->fis_buffer->virtual_address;
  memset(port->fis_base, 0, AHCI_RX_FIS_SIZE);

  // 9. Initialize command tables
  for (int i = 0; i < AHCI_MAX_CMDS; i++) {
    // Usar tamaño de buffer mayor para ICH9
    port->cmd_table_buffers[i] =
        dma_alloc_buffer(sizeof(hba_cmd_tbl_t) * 2, 128);
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

    // Inicializar slot como libre
    port->command_slots[i] = false;
  }

  // =================================================================
  // FASE 5: CONFIGURACIÓN DE REGISTROS DEL PUERTO
  // =================================================================

  // 10. Set command list and FIS base addresses
  port->port_regs->clb = port->cmd_list_buffer->physical_address & 0xFFFFFFFF;

  if (ahci_controller.supports_64bit) {
    port->port_regs->clbu =
        (port->cmd_list_buffer->physical_address >> 32) & 0xFFFFFFFF;
  } else {
    port->port_regs->clbu = 0;
  }

  port->port_regs->fb = port->fis_buffer->physical_address & 0xFFFFFFFF;

  if (ahci_controller.supports_64bit) {
    port->port_regs->fbu =
        (port->fis_buffer->physical_address >> 32) & 0xFFFFFFFF;
  } else {
    port->port_regs->fbu = 0;
  }

  // 11. Clear interrupts again
  port->port_regs->is = ~0U;

  // 12. Enable interrupts
  port->port_regs->ie = AHCI_PORT_IE_MASK;

  // =================================================================
  // FASE 6: INICIAR EL PUERTO
  // =================================================================

  // 13. Start the port con timeout extendido
  if (!ahci_start_port(port_num)) {
    terminal_printf(&main_terminal, "AHCI: Failed to start port %u, retrying...\r\n",
                    port_num);
    
    // Segundo intento con reset completo
    ahci_stop_port(port_num);
    
    // Pequeña pausa
    for (volatile int i = 0; i < 1000000; i++) __asm__ volatile("pause");
    
    if (!ahci_start_port(port_num)) {
      terminal_printf(&main_terminal, "AHCI: Second attempt also failed for port %u\r\n",
                      port_num);
      return false;
    }
  }

  // =================================================================
  // FASE 7: VERIFICACIÓN FINAL
  // =================================================================

  // 14. Esperar a que el puerto se estabilice (crítico para hardware real)
  terminal_printf(&main_terminal, "AHCI: Waiting for port %u to stabilize...\r\n",
                  port_num);
  
  uint32_t stabilization_timeout = 1000000; // 1 segundo
  while (stabilization_timeout--) {
    cmd = port->port_regs->cmd;
    
    // Verificar que FR y CR estén activos
    if ((cmd & AHCI_PORT_CMD_FR) && (cmd & AHCI_PORT_CMD_CR)) {
      terminal_printf(&main_terminal, "AHCI: Port %u stabilized (CMD=0x%08x)\r\n",
                      port_num, cmd);
      break;
    }
    
    // Pequeña pausa
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
  }
  
  if (!(port->port_regs->cmd & AHCI_PORT_CMD_FR) || 
      !(port->port_regs->cmd & AHCI_PORT_CMD_CR)) {
    terminal_printf(&main_terminal,
                    "AHCI: Port %u not running properly after stabilization (CMD=0x%08x)\r\n",
                    port_num, port->port_regs->cmd);
    
    // Debug adicional
    uint32_t ssts = port->port_regs->ssts;
    uint8_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    uint8_t ipm = (ssts >> AHCI_PORT_SSTS_IPM_SHIFT) & AHCI_PORT_SSTS_IPM_MASK;
    
    terminal_printf(&main_terminal,
                    "AHCI: Port %u SSTS=0x%08x (DET=%u, IPM=%u)\r\n",
                    port_num, ssts, det, ipm);
    
    // Intentar recover si DET=3 (device present)
    if (det == AHCI_PORT_DET_ESTABLISHED) {
      terminal_printf(&main_terminal, "AHCI: Device present but port not running, attempting recovery...\r\n");
      
      // Forzar ICC a Active
      port->port_regs->cmd = (port->port_regs->cmd & ~AHCI_PORT_CMD_ICC_MASK) |
                            (AHCI_PORT_CMD_ICC_ACTIVE << AHCI_PORT_CMD_ICC_SHIFT);
      
      // Esperar y verificar de nuevo
      for (volatile int i = 0; i < 500000; i++) __asm__ volatile("pause");
      
      cmd = port->port_regs->cmd;
      if ((cmd & AHCI_PORT_CMD_FR) && (cmd & AHCI_PORT_CMD_CR)) {
        terminal_printf(&main_terminal, "AHCI: Port %u recovered (CMD=0x%08x)\r\n",
                        port_num, cmd);
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  port->initialized = true;

  terminal_printf(&main_terminal, "AHCI: Port %u initialized successfully (CMD=0x%08x)\r\n",
                  port_num, port->port_regs->cmd);

  return true;
}


bool ahci_start_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS) return false;
  
  ahci_port_t *port = &ahci_controller.ports[port_num];
  hba_port_t *regs = port->port_regs;
  
  // Asegurarse de que el puerto esté completamente detenido
  ahci_stop_port(port_num);
  
  // Pequeña pausa después de detener
  for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause");
  
  // Limpiar interrupciones pendientes
  regs->is = ~0U;
  
  // 1. Primero habilitar FIS Receive (FRE)
  regs->cmd |= AHCI_PORT_CMD_FRE;
  
  // Esperar a que FR se active (timeout extendido para hardware real)
  uint32_t timeout = 500000; // 500ms
  while (!(regs->cmd & AHCI_PORT_CMD_FR) && timeout--) {
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
  }
  
  if (!(regs->cmd & AHCI_PORT_CMD_FR)) {
    terminal_printf(&main_terminal, "AHCI: Port %u FIS receive not running after %u ms\r\n",
                    port_num, 500);
    return false;
  }
  
  terminal_printf(&main_terminal, "AHCI: Port %u FIS receive running (FR=1)\r\n", port_num);
  
  // 2. Forzar ICC a Active (crítico después de COMRESET)
  regs->cmd = (regs->cmd & ~AHCI_PORT_CMD_ICC_MASK) |
              (AHCI_PORT_CMD_ICC_ACTIVE << AHCI_PORT_CMD_ICC_SHIFT);
  
  // 3. Habilitar Start (ST)
  regs->cmd |= AHCI_PORT_CMD_ST;
  
  // Esperar a que CR se active (timeout extendido)
  timeout = 500000;
  while (!(regs->cmd & AHCI_PORT_CMD_CR) && timeout--) {
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
  }
  
  if (!(regs->cmd & AHCI_PORT_CMD_CR)) {
    terminal_printf(&main_terminal, "AHCI: Port %u command list not running after %u ms\r\n",
                    port_num, 500);
    return false;
  }
  
  terminal_printf(&main_terminal, "AHCI: Port %u command list running (CR=1)\r\n", port_num);
  
  // 4. Verificar estado completo
  uint32_t final_cmd = regs->cmd;
  if ((final_cmd & AHCI_PORT_CMD_FR) && (final_cmd & AHCI_PORT_CMD_CR)) {
    terminal_printf(&main_terminal, "AHCI: Port %u started successfully (CMD=0x%08x)\r\n",
                    port_num, final_cmd);
    return true;
  } else {
    terminal_printf(&main_terminal, "AHCI: Port %u started but not fully operational (CMD=0x%08x)\r\n",
                    port_num, final_cmd);
    return false;
  }
}

bool ahci_stop_port(uint8_t port_num) {
  if (port_num >= AHCI_MAX_PORTS) return false;
  
  ahci_port_t *port = &ahci_controller.ports[port_num];
  hba_port_t *regs = port->port_regs;
  
  // 1. Desactivar ST (Start) y FRE (FIS Receive Enable)
  regs->cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
  
  // 2. Esperar a que CR (Command List Running) se desactive
  uint32_t timeout = 100000; // 100ms
  while ((regs->cmd & AHCI_PORT_CMD_CR) && timeout--) {
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
  }
  
  // 3. Esperar a que FR (FIS Receive Running) se desactive
  timeout = 100000;
  while ((regs->cmd & AHCI_PORT_CMD_FR) && timeout--) {
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause");
  }
  
  // 4. Limpiar interrupciones
  regs->is = ~0U;
  
  if (regs->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) {
    terminal_printf(&main_terminal,
                    "AHCI: WARNING - Port %u didn't stop cleanly (CMD=0x%08x)\r\n",
                    port_num, regs->cmd);
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