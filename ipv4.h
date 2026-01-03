#ifndef IPV4_H
#define IPV4_H

#include <stdbool.h>
#include <stdint.h>


// Dirección IP
typedef uint8_t ip_addr_t[4];

// Cabecera IPv4
typedef struct {
  uint8_t version_ihl;
  uint8_t tos;
  uint16_t total_length;
  uint16_t identification;
  uint16_t flags_fragment;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t header_checksum;
  uint8_t source_ip[4];
  uint8_t dest_ip[4];
} __attribute__((packed)) ip_header_t;

// Protocolos IP
#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP 6
#define IP_PROTOCOL_UDP 17

// Funciones de utilidad
uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

// Funciones IP
uint16_t ip_checksum(void *data, uint16_t length);
bool ip_send_packet(ip_addr_t dest_ip, uint8_t protocol, uint8_t *payload,
                    uint32_t payload_len);
bool ip_process_packet(uint8_t *packet, uint32_t length, ip_addr_t *src_ip,
                       uint8_t *protocol);
void ip_set_address(ip_addr_t ip, ip_addr_t mask, ip_addr_t gw);
void ip_get_address(ip_addr_t ip);

#endif // IPV4_H