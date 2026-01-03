#ifndef ICMP_H
#define ICMP_H

#include <stdbool.h>
#include <stdint.h>


// Dirección IP
typedef uint8_t ip_addr_t[4];

// Cabecera ICMP
typedef struct {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t id;
  uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

// Tipos ICMP
#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_TYPE_ECHO_REQUEST 8

// Funciones ICMP
bool icmp_send_reply(ip_addr_t dest_ip, uint16_t id, uint16_t seq,
                     uint8_t *data, uint16_t data_len);
bool icmp_send_request(ip_addr_t dest_ip, uint16_t id, uint16_t seq,
                       uint8_t *data, uint16_t data_len);
void icmp_process_packet(ip_addr_t src_ip, uint8_t *packet, uint32_t length);

#endif // ICMP_H