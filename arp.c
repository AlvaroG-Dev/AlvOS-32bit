#include "arp.h"
#include "e1000.h"
#include "ipv4.h"
#include "irq.h"
#include "kernel.h"
#include "network_stack.h"

#include "network.h"
#include "string.h"
#include "terminal.h"

// Increased cache size to prevent thrashing in bridged networks
#define ARP_CACHE_SIZE 64
#define ARP_REQUEST_TIMEOUT 5000 // 5 segundos

// Cache ARP
typedef struct {
  ip_addr_t ip;
  uint8_t mac[6];
  uint32_t timestamp;
  bool valid;
  bool permanent;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint32_t arp_sequence = 0;

static bool is_qemu_mode(void) {
  // Verificar si estamos en la red típica de QEMU (10.0.2.x)
  ip_addr_t our_ip;
  ip_get_address(our_ip);

  return (our_ip[0] == 10 && our_ip[1] == 0 && our_ip[2] == 2);
}

// Inicializar cache ARP
void arp_init(void) {
  memset(arp_cache, 0, sizeof(arp_cache));

  // Añadir broadcast (255.255.255.255)
  arp_cache[0].valid = true;
  arp_cache[0].permanent = true;
  memset(arp_cache[0].ip, 255, 4);
  memset(arp_cache[0].mac, 0xFF, 6);
  arp_cache[0].timestamp = ticks_since_boot;

  // Añadir multicast 224.0.0.22
  arp_cache[1].valid = true;
  arp_cache[1].permanent = true;
  arp_cache[1].ip[0] = 224;
  arp_cache[1].ip[1] = 0;
  arp_cache[1].ip[2] = 0;
  arp_cache[1].ip[3] = 22;
  arp_cache[1].mac[0] = 0x01;
  arp_cache[1].mac[1] = 0x00;
  arp_cache[1].mac[2] = 0x5E;
  arp_cache[1].mac[3] = 0x00;
  arp_cache[1].mac[4] = 0x00;
  arp_cache[1].mac[5] = 0x16;
  arp_cache[1].timestamp = ticks_since_boot;

  // Si estamos en QEMU, añadir gateway automáticamente
  ip_addr_t our_ip;
  ip_get_address(our_ip);

  if (our_ip[0] == 10 && our_ip[1] == 0 && our_ip[2] == 2) {
    // Estamos en QEMU (10.0.2.x)
    ip_addr_t gateway = {10, 0, 2, 2};
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    arp_add_entry(gateway, broadcast_mac);

    // Marcar el gateway como permanente en QEMU para evitar que se borre
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
      if (arp_cache[i].valid && memcmp(arp_cache[i].ip, gateway, 4) == 0) {
        arp_cache[i].permanent = true;
        break;
      }
    }

    terminal_puts(&main_terminal, "[ARP] QEMU environment detected: Gateway "
                                  "10.0.2.2 -> broadcast MAC\r\n");
  }

  terminal_puts(&main_terminal, "[ARP] Cache initialized\r\n");
}

// Buscar MAC por IP
bool arp_lookup(ip_addr_t ip, uint8_t *mac) {
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
      memcpy(mac, arp_cache[i].mac, 6);
      return true;
    }
  }
  return false;
}

// Añadir entrada a cache ARP
void arp_add_entry(ip_addr_t ip, uint8_t *mac) {
  // 1. Verificar si la IP ya existe en la caché para actualizarla
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
      // Encontrado: actualizar MAC y timestamp

      // Verificar si la MAC realmente cambió
      if (memcmp(arp_cache[i].mac, mac, 6) != 0) {
        memcpy(arp_cache[i].mac, mac, 6);
        if (!arp_cache[i].permanent) {
          terminal_printf(&main_terminal, "[ARP] Updated: %d.%d.%d.%d\r\n",
                          ip[0], ip[1], ip[2], ip[3]);
        }
      }

      arp_cache[i].timestamp = ticks_since_boot;
      return;
    }
  }

  // 2. Si no existe, buscar slot vacío o reemplazar el más antiguo
  int oldest = -1;
  uint32_t oldest_time = 0xFFFFFFFF;

  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (!arp_cache[i].valid) {
      // Slot vacío encontrado
      memcpy(arp_cache[i].ip, ip, 4);
      memcpy(arp_cache[i].mac, mac, 6);
      arp_cache[i].timestamp = ticks_since_boot;
      arp_cache[i].valid = true;
      arp_cache[i].permanent = false;

      terminal_printf(
          &main_terminal,
          "[ARP] Added: %d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x\r\n",
          ip[0], ip[1], ip[2], ip[3], mac[0], mac[1], mac[2], mac[3], mac[4],
          mac[5]);
      return;
    }

    // Solo considerar reemplazar si NO es permanente
    if (!arp_cache[i].permanent && arp_cache[i].timestamp <= oldest_time) {
      oldest_time = arp_cache[i].timestamp;
      oldest = i;
    }
  }

  // Reemplazar el más antiguo
  if (oldest != -1) {
    memcpy(arp_cache[oldest].ip, ip, 4);
    memcpy(arp_cache[oldest].mac, mac, 6);
    arp_cache[oldest].timestamp = ticks_since_boot;
    arp_cache[oldest].permanent = false;

    terminal_printf(&main_terminal, "[ARP] Replaced old entry: %d.%d.%d.%d\r\n",
                    ip[0], ip[1], ip[2], ip[3]);
  }
}

// Enviar solicitud ARP
bool arp_send_request(ip_addr_t target_ip) {
  uint8_t packet[60]; // Tamaño mínimo Ethernet
  arp_packet_t *arp = (arp_packet_t *)(packet + 14);

  // Obtener nuestra MAC
  uint8_t our_mac[6];
  e1000_get_mac(our_mac);

  // Obtener nuestra IP
  ip_addr_t our_ip;
  ip_get_address(our_ip);

  /* Verbose log disabled to reduce spam
  terminal_printf(
      &main_terminal,
      "[ARP] Sending request: Who has %d.%d.%d.%d? Tell %d.%d.%d.%d\r\n",
      target_ip[0], target_ip[1], target_ip[2], target_ip[3], our_ip[0],
      our_ip[1], our_ip[2], our_ip[3]);
  */

  // Configurar cabecera Ethernet
  memset(packet, 0xFF, 6);        // Destino: broadcast
  memcpy(packet + 6, our_mac, 6); // Origen: nuestra MAC
  packet[12] = 0x08;              // ARP EtherType high
  packet[13] = 0x06;              // ARP EtherType low

  // Configurar paquete ARP
  arp->hardware_type = htons(1);      // Ethernet
  arp->protocol_type = htons(0x0800); // IPv4
  arp->hardware_len = 6;
  arp->protocol_len = 4;
  arp->opcode = htons(ARP_OP_REQUEST);

  memcpy(arp->sender_mac, our_mac, 6);
  memcpy(arp->sender_ip, our_ip, 4);

  memset(arp->target_mac, 0, 6); // Desconocido
  memcpy(arp->target_ip, target_ip, 4);

  // Padding para llegar a 60 bytes
  memset(packet + sizeof(ethernet_header_t) + sizeof(arp_packet_t), 0,
         60 - (sizeof(ethernet_header_t) + sizeof(arp_packet_t)));

  // Enviar paquete
  if (e1000_send_packet(packet, 60)) {
    return true;
  }

  return false;
}

// Procesar paquete ARP recibido
void arp_process_packet(uint8_t *packet, uint32_t length) {
  if (length < sizeof(ethernet_header_t) + sizeof(arp_packet_t)) {
    return;
  }

  arp_packet_t *arp = (arp_packet_t *)(packet + 14);

  // Verificar que es ARP para IPv4 sobre Ethernet
  if (ntohs(arp->hardware_type) != 1 || ntohs(arp->protocol_type) != 0x0800 ||
      arp->hardware_len != 6 || arp->protocol_len != 4) {
    return;
  }

  uint16_t opcode = ntohs(arp->opcode);

  // Actualizar cache con la información del remitente
  arp_add_entry(arp->sender_ip, arp->sender_mac);

  // Si es una solicitud ARP para nosotros, responder
  ip_addr_t our_ip;
  ip_get_address(our_ip); // Get actual IP address

  if (opcode == ARP_OP_REQUEST && memcmp(arp->target_ip, our_ip, 4) == 0) {
    // Responder con ARP reply
    uint8_t reply[60];
    arp_packet_t *arp_reply = (arp_packet_t *)(reply + 14);
    uint8_t our_mac[6];
    e1000_get_mac(our_mac);

    // Cabecera Ethernet
    memcpy(reply, arp->sender_mac, 6); // Destino: quien preguntó
    memcpy(reply + 6, our_mac, 6);     // Origen: nosotros
    reply[12] = 0x08;
    reply[13] = 0x06;

    // Paquete ARP
    arp_reply->hardware_type = htons(1);
    arp_reply->protocol_type = htons(0x0800);
    arp_reply->hardware_len = 6;
    arp_reply->protocol_len = 4;
    arp_reply->opcode = htons(ARP_OP_REPLY);

    memcpy(arp_reply->sender_mac, our_mac, 6);
    memcpy(arp_reply->sender_ip, our_ip, 4);
    memcpy(arp_reply->target_mac, arp->sender_mac, 6);
    memcpy(arp_reply->target_ip, arp->sender_ip, 4);

    // Padding
    memset(reply + sizeof(ethernet_header_t) + sizeof(arp_packet_t), 0,
           60 - (sizeof(ethernet_header_t) + sizeof(arp_packet_t)));

    // Enviar respuesta
    e1000_send_packet(reply, 60);

    terminal_printf(&main_terminal, "[ARP] Sent reply to %d.%d.%d.%d\r\n",
                    arp->sender_ip[0], arp->sender_ip[1], arp->sender_ip[2],
                    arp->sender_ip[3]);
  }
  // Omit reply logging to avoid spam
}

// Obtener MAC (con ARP si es necesario)
bool arp_resolve(ip_addr_t ip, uint8_t *mac, bool send_request) {
  // 1. Primero buscar en cache
  if (arp_lookup(ip, mac)) {
    return true;
  }

  // 2. Intentar resolución ARP estándar
  if (send_request) {
    // Intentar resolver hasta 3 veces
    for (int retry = 0; retry < 3; retry++) {
      arp_send_request(ip);

      // Esperar respuesta (Timeout de 200ms por intento)
      uint32_t start_time = ticks_since_boot;
      uint32_t timeout_ticks = 20; // 200ms aprox (si 100Hz)
      uint32_t loop_count = 0;
      const uint32_t max_loops = 2000000;

      __asm__ __volatile__("sti");
      while (loop_count < max_loops) {
        // Verificar timeout por tiempo
        if (ticks_since_boot > start_time &&
            (ticks_since_boot - start_time) > timeout_ticks) {
          break;
        }

        // CRÍTICO: Procesar pila de red mientras esperamos
        network_stack_tick();

        // Verificar si ya se resolvió
        if (arp_lookup(ip, mac)) {
          // terminal_printf(&main_terminal, "[ARP] Resolved %d.%d.%d.%d\r\n",
          //                 ip[0], ip[1], ip[2], ip[3]);
          return true;
        }

        // Pequeña pausa
        for (volatile int k = 0; k < 1000; k++)
          ;
        loop_count++;
      }

      if (retry < 2) {
        // terminal_puts(&main_terminal, "[ARP] Retry...\r\n");
      }
    }
  }

  // 3. FALLBACK: Si ARP falla y estamos en modo QEMU (10.0.2.x), usar hacks
  if (is_qemu_mode()) {
    // 3a. Gateway (10.0.2.2) y DNS (10.0.2.3) -> usar broadcast MAC si ARP
    // falló
    ip_addr_t qemu_gateway = {10, 0, 2, 2};
    ip_addr_t qemu_dns = {10, 0, 2, 3};

    if (memcmp(ip, qemu_gateway, 4) == 0 || memcmp(ip, qemu_dns, 4) == 0) {
      uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      memcpy(mac, broadcast_mac, 6);
      arp_add_entry(ip, mac);

      // Proteger esta fallback también
      for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
          arp_cache[i].permanent = true;
          break;
        }
      }
      return true;
    }

    // 3b. Broadcast address
    ip_addr_t broadcast_ip = {255, 255, 255, 255};
    if (memcmp(ip, broadcast_ip, 4) == 0) {
      uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      memcpy(mac, broadcast_mac, 6);
      arp_add_entry(ip, mac);
      return true;
    }

    // 3c. IP Externa -> Usar Gateway
    ip_addr_t netmask = {255, 255, 255, 0};
    ip_addr_t our_ip_addr;
    ip_get_address(our_ip_addr);

    bool is_local = true;
    for (int i = 0; i < 4; i++) {
      if ((our_ip_addr[i] & netmask[i]) != (ip[i] & netmask[i])) {
        is_local = false;
        break;
      }
    }

    if (!is_local) {
      return arp_resolve(qemu_gateway, mac, false);
    }
  }

  terminal_printf(&main_terminal, "[ARP] Failed to resolve %d.%d.%d.%d\r\n",
                  ip[0], ip[1], ip[2], ip[3]);
  return false;
}

// Añade esta función para limpiar entradas antiguas
void arp_cleanup_old_entries(void) {
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    // CRITICAL FIX: Do not remove permanent entries!
    if (arp_cache[i].valid && !arp_cache[i].permanent) {
      uint32_t diff = ticks_since_boot - arp_cache[i].timestamp;
      uint32_t age = diff / 100; // en segundos

      if (age > 300) {
        terminal_printf(&main_terminal,
                        "[ARP] Removing stale entry: %d.%d.%d.%d\r\n",
                        arp_cache[i].ip[0], arp_cache[i].ip[1],
                        arp_cache[i].ip[2], arp_cache[i].ip[3]);
        arp_cache[i].valid = false;
      }
    }
  }
}

// Comando para mostrar cache ARP
void arp_show_cache(void) {
  terminal_puts(&main_terminal, "\r\n=== ARP Cache ===\r\n");
  terminal_puts(&main_terminal,
                "IP Address        MAC Address         Age     State\r\n");
  terminal_puts(&main_terminal,
                "-----------------------------------------------------\r\n");

  char ip_str[16];
  char mac_str[18];

  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid) {
      // Convertir IP a string
      snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", arp_cache[i].ip[0],
               arp_cache[i].ip[1], arp_cache[i].ip[2], arp_cache[i].ip[3]);

      // Convertir MAC a string
      snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
               arp_cache[i].mac[0], arp_cache[i].mac[1], arp_cache[i].mac[2],
               arp_cache[i].mac[3], arp_cache[i].mac[4], arp_cache[i].mac[5]);

      uint32_t age = (ticks_since_boot - arp_cache[i].timestamp) / 100;

      // Determinar si es estático o dinámico
      const char *state = arp_cache[i].permanent ? "static" : "dynamic";

      terminal_printf(&main_terminal, "%-16s %-18s %3u s   %s\r\n", ip_str,
                      mac_str, age, state);
    }
  }

  // Mostrar estadísticas
  int count = 0;
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid)
      count++;
  }
  terminal_printf(&main_terminal, "Total entries: %d/%d\r\n", count,
                  ARP_CACHE_SIZE);
}