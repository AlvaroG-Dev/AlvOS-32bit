#include "arp.h"
#include "e1000.h"
#include "ipv4.h"
#include "irq.h"
#include "kernel.h"
#include "memory.h"
#include "network.h"
#include "string.h"
#include "terminal.h"

#define ARP_CACHE_SIZE 16
#define ARP_REQUEST_TIMEOUT 5000 // 5 segundos

// Cache ARP
typedef struct {
  ip_addr_t ip;
  uint8_t mac[6];
  uint32_t timestamp;
  bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint32_t arp_sequence = 0;

// Inicializar cache ARP
void arp_init(void) {
  memset(arp_cache, 0, sizeof(arp_cache));

  // Añadir broadcast (255.255.255.255) y multicast por defecto
  arp_cache[0].valid = true;
  memset(arp_cache[0].ip, 255, 4);
  memset(arp_cache[0].mac, 0xFF, 6);

  arp_cache[1].valid = true;
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
  // Buscar slot vacío o reemplazar el más antiguo
  int oldest = 0;
  uint32_t oldest_time = 0xFFFFFFFF;

  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (!arp_cache[i].valid) {
      // Slot vacío encontrado
      memcpy(arp_cache[i].ip, ip, 4);
      memcpy(arp_cache[i].mac, mac, 6);
      arp_cache[i].timestamp = ticks_since_boot;
      arp_cache[i].valid = true;

      terminal_printf(
          &main_terminal,
          "[ARP] Added: %d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x\r\n",
          ip[0], ip[1], ip[2], ip[3], mac[0], mac[1], mac[2], mac[3], mac[4],
          mac[5]);
      return;
    }

    if (arp_cache[i].timestamp < oldest_time) {
      oldest_time = arp_cache[i].timestamp;
      oldest = i;
    }
  }

  // Reemplazar el más antiguo
  memcpy(arp_cache[oldest].ip, ip, 4);
  memcpy(arp_cache[oldest].mac, mac, 6);
  arp_cache[oldest].timestamp = ticks_since_boot;

  terminal_printf(&main_terminal, "[ARP] Replaced old entry: %d.%d.%d.%d\r\n",
                  ip[0], ip[1], ip[2], ip[3]);
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
  ip_get_address(our_ip); // <-- AÑADE ESTA LÍNEA

  terminal_printf(
      &main_terminal,
      "[ARP] Sending request: Who has %d.%d.%d.%d? Tell %d.%d.%d.%d\r\n",
      target_ip[0], target_ip[1], target_ip[2], target_ip[3], our_ip[0],
      our_ip[1], our_ip[2], our_ip[3]);

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
  memcpy(arp->sender_ip, our_ip, 4); // <-- USA NUESTRA IP REAL

  memset(arp->target_mac, 0, 6); // Desconocido
  memcpy(arp->target_ip, target_ip, 4);

  // Padding para llegar a 60 bytes
  memset(packet + sizeof(ethernet_header_t) + sizeof(arp_packet_t), 0,
         60 - (sizeof(ethernet_header_t) + sizeof(arp_packet_t)));

  // DEBUG: Mostrar paquete
  terminal_puts(&main_terminal, "[ARP] Packet dump:\r\n");
  for (int i = 0; i < 42; i++) { // Solo primeros 42 bytes
    terminal_printf(&main_terminal, "%02x ", packet[i]);
    if ((i + 1) % 16 == 0)
      terminal_puts(&main_terminal, "\r\n");
  }
  terminal_puts(&main_terminal, "\r\n");

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
  ip_addr_t our_ip = {192, 168, 1, 100}; // IP temporal

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
}

// Obtener MAC (con ARP si es necesario)
bool arp_resolve(ip_addr_t ip, uint8_t *mac, bool send_request) {
  // Primero buscar en cache
  if (arp_lookup(ip, mac)) {
    return true;
  }

  // Si no está en cache y podemos enviar solicitud
  if (send_request) {
    arp_send_request(ip);

    // Esperar respuesta (polling simple)
    for (int i = 0; i < 50; i++) {
      // Procesar paquetes recibidos
      uint8_t buffer[1522];
      uint32_t len = e1000_receive_packet(buffer, sizeof(buffer));

      if (len > 0) {
        // Verificar si es ARP
        if (len >= 14 && buffer[12] == 0x08 && buffer[13] == 0x06) {
          arp_process_packet(buffer, len);

          // Verificar si ahora tenemos la MAC
          if (arp_lookup(ip, mac)) {
            return true;
          }
        }
      }

      // Esperar 10ms
      for (volatile int j = 0; j < 100000; j++)
        ;
    }
  }

  return false;
}

// Comando para mostrar cache ARP
void arp_show_cache(void) {
  terminal_puts(&main_terminal, "\r\n=== ARP Cache ===\r\n");
  terminal_puts(&main_terminal,
                "IP Address        MAC Address         Age\r\n");
  terminal_puts(&main_terminal,
                "------------------------------------------------\r\n");

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

      terminal_printf(&main_terminal, "%-16s %-18s %3u s\r\n", ip_str, mac_str,
                      age);
    }
  }
}