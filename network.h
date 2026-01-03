#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stdint.h>

// Protocolos Ethernet
#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV6 0x86DD

// Direcciones MAC
typedef uint8_t mac_addr_t[6];

// Estructura de paquete de red
typedef struct {
  uint8_t *data;
  uint32_t length;
  uint32_t capacity;
} network_packet_t;

// Funciones de la capa de red
void network_init(void);
bool network_send_packet(const uint8_t *data, uint32_t length);
uint32_t network_receive_packet(uint8_t *buffer, uint32_t max_len);
void network_get_mac(mac_addr_t mac);
bool network_is_link_up(void);
void network_print_stats(void);

// Funciones de utilidad
void mac_to_string(const mac_addr_t mac, char *buffer);
bool string_to_mac(const char *str, mac_addr_t mac);

// Funciones de conversión de endianness
uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

#endif // NETWORK_H