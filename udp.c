#include "udp.h"
#include "ipv4.h"
#include "kernel.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"

#define UDP_MAX_HANDLERS 16

// Tabla de manejadores de puertos (sockets simples)
typedef struct {
  uint16_t port;
  udp_handler_t handler;
  bool used;
} udp_socket_entry_t;

static udp_socket_entry_t udp_sockets[UDP_MAX_HANDLERS];

void udp_init(void) {
  memset(udp_sockets, 0, sizeof(udp_sockets));
  terminal_puts(&main_terminal, "[UDP] Protocol stack initialized\r\n");
}

void udp_bind(uint16_t port, udp_handler_t handler) {
  for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
    if (!udp_sockets[i].used) {
      udp_sockets[i].port = port;
      udp_sockets[i].handler = handler;
      udp_sockets[i].used = true;
      return;
    }
  }
  terminal_printf(&main_terminal,
                  "[UDP] Error: No more sockets available for port %u\r\n",
                  port);
}

// Pseudo-header para checksum UDP
typedef struct {
  uint8_t src_ip[4];
  uint8_t dest_ip[4];
  uint8_t zero;
  uint8_t protocol;
  uint16_t udp_length;
} __attribute__((packed)) udp_pseudo_header_t;

bool udp_send_packet(ip_addr_t dest_ip, uint16_t src_port, uint16_t dest_port,
                     uint8_t *data, uint32_t length) {

  // Calcular tamaño total
  uint32_t total_len = sizeof(udp_header_t) + length;

  // Evitar malloc para paquetes pequeños (DNS queries comunes)
  // Usar buffer estático o de pila si es pequeño
  uint8_t stack_buffer[1024];
  uint8_t *packet = NULL;
  bool using_malloc = false;

  // Terminal debugging
  // terminal_printf(&main_terminal, "[UDP] Sending %d bytes to port %d\r\n",
  // length, dest_port);

  if (total_len <= sizeof(stack_buffer)) {
    packet = stack_buffer;
  } else {
    packet = (uint8_t *)kernel_malloc(total_len);
    using_malloc = true;
  }

  if (!packet) {
    return false;
  }

  udp_header_t *udp = (udp_header_t *)packet;

  udp->src_port = htons(src_port);
  udp->dest_port = htons(dest_port);
  udp->length = htons((uint16_t)total_len);
  udp->checksum = 0; // Checksum opcional en IPv4, 0 = deshabilitado

  // Copiar datos del payload
  memcpy(packet + sizeof(udp_header_t), data, length);

  /*
     NOTA: El checksum UDP es opcional en IPv4, pero recomendado.
     Para simplificar inicialmente lo dejamos en 0.
     Para implementarlo necesitamos construir el pseudo-header,
     sumar el header UDP y los datos.
  */

  // Enviar vía IPv4 (Protocolo 17 = UDP)
  bool result = ip_send_packet(dest_ip, 17, packet, total_len);

  if (using_malloc) {
    kernel_free(packet);
  }
  return result;
}

void udp_input(uint8_t *packet, uint32_t length, ip_addr_t src_ip) {
  if (length < sizeof(udp_header_t)) {
    return;
  }

  udp_header_t *udp = (udp_header_t *)packet;
  uint16_t dest_port = ntohs(udp->dest_port);
  uint16_t src_port = ntohs(udp->src_port);
  uint16_t udp_len = ntohs(udp->length);

  // Validar longitud real
  if (length < udp_len) {
    // Paquete truncado
    return;
  }

  uint32_t payload_len = udp_len - sizeof(udp_header_t);
  uint8_t *payload = packet + sizeof(udp_header_t);

  // Buscar manejador para este puerto
  for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
    if (udp_sockets[i].used && udp_sockets[i].port == dest_port) {
      if (udp_sockets[i].handler) {
        udp_sockets[i].handler(src_ip, src_port, payload, payload_len);
      }
      return;
    }
  }

  // Si llegamos aquí, puerto cerrado (ICMP Port Unreachable sería lo correcto
  // aquí) terminal_printf(&main_terminal, "[UDP] Dropped packet for closed port
  // %u\r\n", dest_port);
}
