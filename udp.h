#ifndef UDP_H
#define UDP_H

#include "ipv4.h"
#include <stdbool.h>
#include <stdint.h>

// Cabecera UDP
typedef struct {
  uint16_t src_port;
  uint16_t dest_port;
  uint16_t length;
  uint16_t checksum;
} __attribute__((packed)) udp_header_t;

// Tipo de callback para manejar paquetes UDP entrantes
typedef void (*udp_handler_t)(ip_addr_t src_ip, uint16_t src_port,
                              uint8_t *data, uint32_t length);

// Funciones
void udp_init(void);

// Enviar datos UDP
bool udp_send_packet(ip_addr_t dest_ip, uint16_t src_port, uint16_t dest_port,
                     uint8_t *data, uint32_t length);

// Registrar un manejador para un puerto específico (Bind)
void udp_bind(uint16_t port, udp_handler_t handler);

// Función llamada por IPv4 cuando llega un paquete UDP
void udp_input(uint8_t *packet, uint32_t length, ip_addr_t src_ip);

#endif // UDP_H
