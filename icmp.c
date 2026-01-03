#include "icmp.h"
#include "ipv4.h"
#include "irq.h"
#include "kernel.h"
#include "memutils.h"
#include "string.h"
#include "terminal.h"

// Enviar respuesta PING (echo reply)
bool icmp_send_reply(ip_addr_t dest_ip, uint16_t id, uint16_t seq,
                     uint8_t *data, uint16_t data_len) {

  uint8_t packet[sizeof(icmp_header_t) + data_len];
  icmp_header_t *icmp = (icmp_header_t *)packet;

  // Configurar cabecera ICMP
  icmp->type = ICMP_TYPE_ECHO_REPLY;
  icmp->code = 0;
  icmp->checksum = 0;
  icmp->id = htons(id);
  icmp->sequence = htons(seq);

  // Copiar datos
  if (data && data_len > 0) {
    memcpy(packet + sizeof(icmp_header_t), data, data_len);
  }

  // Calcular checksum
  icmp->checksum = ip_checksum(icmp, sizeof(icmp_header_t) + data_len);

  // Enviar por IP
  return ip_send_packet(dest_ip, IP_PROTOCOL_ICMP, packet,
                        sizeof(icmp_header_t) + data_len);
}

// Enviar solicitud PING (echo request)
bool icmp_send_request(ip_addr_t dest_ip, uint16_t id, uint16_t seq,
                       uint8_t *data, uint16_t data_len) {

  uint8_t packet[sizeof(icmp_header_t) + data_len];
  icmp_header_t *icmp = (icmp_header_t *)packet;

  // Configurar cabecera ICMP
  icmp->type = ICMP_TYPE_ECHO_REQUEST;
  icmp->code = 0;
  icmp->checksum = 0;
  icmp->id = htons(id);
  icmp->sequence = htons(seq);

  // Timestamp o datos
  if (data_len >= 4) {
    uint32_t *timestamp = (uint32_t *)(packet + sizeof(icmp_header_t));
    *timestamp = ticks_since_boot;
  }

  // Calcular checksum
  icmp->checksum = ip_checksum(icmp, sizeof(icmp_header_t) + data_len);

  // Enviar por IP
  return ip_send_packet(dest_ip, IP_PROTOCOL_ICMP, packet,
                        sizeof(icmp_header_t) + data_len);
}

// Procesar paquete ICMP
void icmp_process_packet(ip_addr_t src_ip, uint8_t *packet, uint32_t length) {
  if (length < sizeof(icmp_header_t)) {
    return;
  }

  icmp_header_t *icmp = (icmp_header_t *)packet;

  // Verificar checksum
  uint16_t received_checksum = icmp->checksum;
  icmp->checksum = 0;

  if (ip_checksum(icmp, length) != received_checksum) {
    terminal_puts(&main_terminal, "[ICMP] Bad checksum\r\n");
    return;
  }

  switch (icmp->type) {
  case ICMP_TYPE_ECHO_REQUEST: {
    // Responder al ping
    uint16_t id = ntohs(icmp->id);
    uint16_t seq = ntohs(icmp->sequence);

    terminal_printf(&main_terminal,
                    "[ICMP] Ping from %d.%d.%d.%d (id=%u, seq=%u)\r\n",
                    src_ip[0], src_ip[1], src_ip[2], src_ip[3], id, seq);

    // Enviar reply con los mismos datos
    icmp_send_reply(src_ip, id, seq, packet + sizeof(icmp_header_t),
                    length - sizeof(icmp_header_t));
    break;
  }

  case ICMP_TYPE_ECHO_REPLY: {
    uint16_t id = ntohs(icmp->id);
    uint16_t seq = ntohs(icmp->sequence);

    // Calcular RTT si tenemos timestamp
    if (length >= sizeof(icmp_header_t) + 4) {
      uint32_t *timestamp = (uint32_t *)(packet + sizeof(icmp_header_t));
      uint32_t rtt = ticks_since_boot - *timestamp;

      terminal_printf(
          &main_terminal,
          "[ICMP] Ping reply from %d.%d.%d.%d: seq=%u, rtt=%u ms\r\n",
          src_ip[0], src_ip[1], src_ip[2], src_ip[3], seq,
          rtt * 10); // Convertir ticks a ms
    } else {
      terminal_printf(&main_terminal,
                      "[ICMP] Ping reply from %d.%d.%d.%d: seq=%u\r\n",
                      src_ip[0], src_ip[1], src_ip[2], src_ip[3], seq);
    }
    break;
  }

  default:
    terminal_printf(&main_terminal, "[ICMP] Unknown type: %u\r\n", icmp->type);
    break;
  }
}