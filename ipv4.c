#include "ipv4.h"
#include "arp.h"
#include "e1000.h"
#include "kernel.h"

#include "network.h"
#include "string.h"
#include "terminal.h"

// Configuración IP estática por defecto (VirtualBox NAT friendly)
// Use 10.0.2.15 for VirtualBox/QEMU NAT default
static ip_addr_t our_ip = {10, 0, 2, 15};
static ip_addr_t netmask = {255, 255, 255, 0};
static ip_addr_t gateway = {10, 0, 2, 2};

// Calcular checksum IP
uint16_t ip_checksum(void *data, uint16_t length) {
  uint32_t sum = 0;
  uint16_t *ptr = (uint16_t *)data;

  while (length > 1) {
    sum += *ptr++;
    length -= 2;
  }

  if (length > 0) {
    sum += *(uint8_t *)ptr;
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return (uint16_t)(~sum);
}

// Enviar paquete IP
bool ip_send_packet(ip_addr_t dest_ip, uint8_t protocol, uint8_t *payload,
                    uint32_t payload_len) {

  // Construir paquete IP
  uint8_t packet[1522];
  ip_header_t *ip = (ip_header_t *)packet;

  // Llenar cabecera IP
  ip->version_ihl = 0x45; // IPv4, 5 palabras de 32-bit (20 bytes)
  ip->tos = 0;
  ip->total_length = htons((uint16_t)(sizeof(ip_header_t) + payload_len));
  ip->identification = htons(0x1234);
  ip->flags_fragment = htons(0x4000); // Don't fragment
  ip->ttl = 64;
  ip->protocol = protocol;

  memcpy(ip->source_ip, our_ip, 4);
  memcpy(ip->dest_ip, dest_ip, 4);

  ip->header_checksum = 0;
  ip->header_checksum = ip_checksum(ip, sizeof(ip_header_t));

  // Copiar payload
  memcpy(packet + sizeof(ip_header_t), payload, payload_len);

  // Determinar dirección MAC destino
  uint8_t dest_mac[6];

  // Primero verificar si es local o necesita gateway
  bool is_local = true;

  for (int i = 0; i < 4; i++) {
    if ((our_ip[i] & netmask[i]) != (dest_ip[i] & netmask[i])) {
      is_local = false;
      break;
    }
  }

  // CORRECCIÓN: No se puede inicializar array con operador ternario
  ip_addr_t target_ip;
  if (is_local) {
    memcpy(target_ip, dest_ip, 4);
  } else {
    memcpy(target_ip, gateway, 4);
  }

  // Resolver ARP para obtener MAC
  if (!arp_resolve(target_ip, dest_mac, true)) {
    terminal_printf(&main_terminal,
                    "[IP] Failed to resolve MAC for %d.%d.%d.%d\r\n",
                    target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
    return false;
  }

  // Añadir cabecera Ethernet
  uint8_t eth_packet[1522];
  ethernet_header_t *eth = (ethernet_header_t *)eth_packet;

  memcpy(eth->dest, dest_mac, 6);

  uint8_t our_mac[6];
  e1000_get_mac(our_mac);
  memcpy(eth->src, our_mac, 6);

  eth->type = htons(ETHERTYPE_IP);

  // Copiar paquete IP después de la cabecera Ethernet
  memcpy(eth_packet + sizeof(ethernet_header_t), packet,
         sizeof(ip_header_t) + payload_len);

  // Enviar
  uint32_t total_len =
      sizeof(ethernet_header_t) + sizeof(ip_header_t) + payload_len;

  // Asegurar tamaño mínimo
  if (total_len < 60) {
    total_len = 60;
  }

  return e1000_send_packet(eth_packet, total_len);
}

// Procesar paquete IP recibido
bool ip_process_packet(uint8_t *packet, uint32_t length, ip_addr_t *src_ip,
                       uint8_t *protocol) {

  if (length < sizeof(ethernet_header_t) + sizeof(ip_header_t)) {
    return false;
  }

  ethernet_header_t *eth = (ethernet_header_t *)packet;

  // Verificar que es IP
  uint16_t ethertype = ntohs(eth->type);
  if (ethertype != ETHERTYPE_IP) {
    return false;
  }

  ip_header_t *ip = (ip_header_t *)(packet + sizeof(ethernet_header_t));

  // Verificar checksum
  uint16_t received_checksum = ip->header_checksum;
  ip->header_checksum = 0;

  if (ip_checksum(ip, sizeof(ip_header_t)) != received_checksum) {
    terminal_puts(&main_terminal, "[IP] Bad checksum\r\n");
    return false;
  }

  // Verificar que es para nosotros
  if (memcmp(ip->dest_ip, our_ip, 4) != 0) {
    // No es para nosotros
    return false;
  }

  // Extraer información
  if (src_ip) {
    memcpy(src_ip, ip->source_ip, 4);
  }

  if (protocol) {
    *protocol = ip->protocol;
  }

  // Actualizar cache ARP con el remitente
  // CORRECTION: Only add to ARP cache if the sender is on the same subnet!
  bool is_local_sender = true;
  for (int i = 0; i < 4; i++) {
    if ((ip->source_ip[i] & netmask[i]) != (our_ip[i] & netmask[i])) {
      is_local_sender = false;
      break;
    }
  }

  if (is_local_sender) {
    arp_add_entry(ip->source_ip, eth->src);
  }

  return true;
}

// Configurar IP
void ip_set_address(ip_addr_t ip, ip_addr_t mask, ip_addr_t gw) {
  memcpy(our_ip, ip, 4);
  memcpy(netmask, mask, 4);
  memcpy(gateway, gw, 4);

  terminal_printf(
      &main_terminal,
      "[IP] Configured: %d.%d.%d.%d/%d.%d.%d.%d GW: %d.%d.%d.%d\r\n", ip[0],
      ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1],
      gw[2], gw[3]);
}

// Obtener nuestra IP
void ip_get_address(ip_addr_t ip) {
  if (ip) {
    memcpy(ip, our_ip, 4);
  }
}