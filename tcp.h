#ifndef TCP_H
#define TCP_H

#include "ipv4.h"
#include <stdbool.h>
#include <stdint.h>

#define TCP_MAX_CONNECTIONS 16

// Cabecera TCP
typedef struct {
  uint16_t src_port;
  uint16_t dest_port;
  uint32_t seq_num;
  uint32_t ack_num;
  uint8_t reserved : 4;
  uint8_t header_len : 4; // En palabras de 32 bits
  uint8_t flags;
  uint16_t window_size;
  uint16_t checksum;
  uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

// Flags TCP
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

// Estados de conexión TCP (Simplificado)
typedef enum {
  TCP_CLOSED,
  TCP_LISTEN,
  TCP_SYN_SENT,
  TCP_SYN_RECEIVED,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT_1,
  TCP_FIN_WAIT_2,
  TCP_CLOSE_WAIT,
  TCP_CLOSING,
  TCP_LAST_ACK,
  TCP_TIME_WAIT
} tcp_state_t;

// Estructura de control de conexión (PCB)
typedef struct {
  ip_addr_t local_ip;
  uint16_t local_port;
  ip_addr_t remote_ip;
  uint16_t remote_port;

  tcp_state_t state;

  uint32_t snd_una; // Send Unacknowledged
  uint32_t snd_nxt; // Send Next
  uint32_t snd_wnd; // Send Window

  uint32_t rcv_nxt; // Receive Next
  uint32_t rcv_wnd; // Receive Window

  // Buffer de recepción interno para evitar pérdida de datos
  uint8_t internal_rx_buffer[4096];
  uint32_t internal_rx_len;

  uint32_t last_activity;        // Ticks de última actividad
  uint32_t retransmit_timeout;   // Timeout para retransmisión
  uint32_t retransmit_count;     // Contador de reintentos
  uint8_t retransmit_data[1024]; // Datos a retransmitir
  uint32_t retransmit_len;       // Longitud de datos a retransmitir
} tcp_pcb_t;

// Funciones
void tcp_init(void);
void tcp_input(uint8_t *packet, uint32_t length, ip_addr_t src_ip);

// API simple de sockets (bloqueante para facilitar)
int tcp_connect(ip_addr_t dest_ip, uint16_t dest_port);
int tcp_send(int socket_id, const uint8_t *data, uint32_t len);
int tcp_receive(int socket_id, uint8_t *buffer, uint32_t len);
void tcp_close(int socket_id);
void tcp_maintenance(void);

#endif // TCP_H
