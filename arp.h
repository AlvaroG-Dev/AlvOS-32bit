#ifndef ARP_H
#define ARP_H

#include <stdbool.h>
#include <stdint.h>

// Direcciones
typedef uint8_t ip_addr_t[4];
typedef uint8_t mac_addr_t[6];

// Cabecera Ethernet
typedef struct {
  uint8_t dest[6];
  uint8_t src[6];
  uint16_t type;
} __attribute__((packed)) ethernet_header_t;

// Paquete ARP
typedef struct {
  uint16_t hardware_type;
  uint16_t protocol_type;
  uint8_t hardware_len;
  uint8_t protocol_len;
  uint16_t opcode;
  uint8_t sender_mac[6];
  uint8_t sender_ip[4];
  uint8_t target_mac[6];
  uint8_t target_ip[4];
} __attribute__((packed)) arp_packet_t;

// Opcodes ARP
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

// Funciones ARP
void arp_init(void);
bool arp_lookup(ip_addr_t ip, uint8_t *mac);
void arp_add_entry(ip_addr_t ip, uint8_t *mac);
bool arp_send_request(ip_addr_t target_ip);
void arp_process_packet(uint8_t *packet, uint32_t length);
bool arp_resolve(ip_addr_t ip, uint8_t *mac, bool send_request);
void arp_show_cache(void);
void arp_cleanup_old_entries(void);

#endif // ARP_H