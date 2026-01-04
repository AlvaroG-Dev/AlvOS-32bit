#include "tcp.h"
#include "apic.h"
#include "irq.h"
#include "kernel.h"
#include "network.h"
#include "network_daemon.h"
#include "network_stack.h"
#include "string.h"
#include "task.h"
#include "terminal.h"

tcp_pcb_t tcp_pcbs[TCP_MAX_CONNECTIONS];

void tcp_init(void) { memset(tcp_pcbs, 0, sizeof(tcp_pcbs)); }

tcp_pcb_t *tcp_new_pcb(void) {
  for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
    if (tcp_pcbs[i].state == TCP_CLOSED) {
      memset(&tcp_pcbs[i], 0, sizeof(tcp_pcb_t));
      return &tcp_pcbs[i];
    }
  }
  return NULL;
}

uint16_t tcp_get_ephemeral_port(void) {
  static uint16_t next_port = 49152;
  return next_port++;
}

tcp_pcb_t *tcp_find_pcb(ip_addr_t remote_ip, uint16_t remote_port,
                        uint16_t local_port) {
  for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
    if (tcp_pcbs[i].state != TCP_CLOSED &&
        tcp_pcbs[i].remote_port == remote_port &&
        tcp_pcbs[i].local_port == local_port &&
        memcmp(tcp_pcbs[i].remote_ip, remote_ip, 4) == 0) {
      return &tcp_pcbs[i];
    }
  }
  return NULL;
}

uint16_t tcp_checksum(uint8_t *tcp_header, uint32_t tcp_length, ip_addr_t src,
                      ip_addr_t dest) {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i += 2) {
    sum += (src[i] << 8) | src[i + 1];
    sum += (dest[i] << 8) | dest[i + 1];
  }
  sum += IP_PROTOCOL_TCP;
  sum += tcp_length;
  uint16_t *ptr = (uint16_t *)tcp_header;
  int len = tcp_length;
  while (len > 1) {
    sum += ntohs(*ptr++);
    len -= 2;
  }
  if (len > 0)
    sum += (*(uint8_t *)ptr) << 8;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

bool tcp_send_packet(tcp_pcb_t *pcb, uint8_t flags, const uint8_t *data,
                     uint32_t len) {
  uint32_t tcp_len = sizeof(tcp_header_t) + len;
  uint8_t *buffer = kernel_malloc(tcp_len);
  if (!buffer)
    return false;

  tcp_header_t *tcp = (tcp_header_t *)buffer;
  tcp->src_port = htons(pcb->local_port);
  tcp->dest_port = htons(pcb->remote_port);
  tcp->seq_num = htonl(pcb->snd_nxt);
  tcp->ack_num = htonl(pcb->rcv_nxt);
  tcp->header_len = 5;
  tcp->reserved = 0;
  tcp->flags = flags;
  tcp->window_size = htons(8192);
  tcp->checksum = 0;
  tcp->urgent_ptr = 0;

  if (len > 0)
    memcpy(buffer + sizeof(tcp_header_t), data, len);
  tcp->checksum =
      htons(tcp_checksum(buffer, tcp_len, pcb->local_ip, pcb->remote_ip));

  uint32_t f;
  __asm__ __volatile__("pushf; cli; pop %0" : "=r"(f));
  bool success =
      ip_send_packet(pcb->remote_ip, IP_PROTOCOL_TCP, buffer, tcp_len);
  __asm__ __volatile__("push %0; popf" : : "r"(f));

  kernel_free(buffer);
  if (success) {
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))
      pcb->snd_nxt++;
    else
      pcb->snd_nxt += len;
  }
  return success;
}

int tcp_connect(ip_addr_t dest_ip, uint16_t dest_port) {
  tcp_pcb_t *pcb = tcp_new_pcb();
  if (!pcb)
    return -1;

  network_config_t config;
  network_get_config(&config);
  memcpy(pcb->local_ip, config.ip_address, 4);
  memcpy(pcb->remote_ip, dest_ip, 4);
  pcb->local_port = tcp_get_ephemeral_port();
  pcb->remote_port = dest_port;

  pcb->snd_una = ticks_since_boot * 1234567;
  pcb->snd_nxt = pcb->snd_una;
  pcb->rcv_nxt = 0;
  pcb->state = TCP_SYN_SENT;
  pcb->last_activity = ticks_since_boot;
  pcb->retransmit_timeout = 50;
  pcb->retransmit_count = 0;
  pcb->internal_rx_len = 0;

  if (!tcp_send_packet(pcb, TCP_FLAG_SYN, NULL, 0))
    return -1;

  uint32_t start_time = ticks_since_boot;
  while (ticks_since_boot - start_time < 500) {
    uint32_t f;
    __asm__ __volatile__("pushf; cli; pop %0" : "=r"(f));
    network_stack_tick();
    __asm__ __volatile__("push %0; popf" : : "r"(f));

    if (pcb->state == TCP_ESTABLISHED)
      return (pcb - tcp_pcbs);

    if (ticks_since_boot - pcb->last_activity > pcb->retransmit_timeout) {
      pcb->retransmit_count++;
      if (pcb->retransmit_count >= 5)
        break;
      pcb->retransmit_timeout *= 2;
      pcb->last_activity = ticks_since_boot;
      tcp_send_packet(pcb, TCP_FLAG_SYN, NULL, 0);
    }
    for (volatile int i = 0; i < 5000; i++)
      __asm__ __volatile__("pause");
  }

  pcb->state = TCP_CLOSED;
  return -1;
}

void tcp_input(uint8_t *packet, uint32_t length, ip_addr_t src_ip) {
  if (length < sizeof(tcp_header_t))
    return;
  tcp_header_t *tcp = (tcp_header_t *)packet;
  tcp_pcb_t *pcb =
      tcp_find_pcb(src_ip, ntohs(tcp->src_port), ntohs(tcp->dest_port));
  if (!pcb)
    return;

  pcb->last_activity = ticks_since_boot;
  uint32_t seq = ntohl(tcp->seq_num);
  uint32_t ack = ntohl(tcp->ack_num);

  if (pcb->state == TCP_SYN_SENT) {
    if ((tcp->flags & TCP_FLAG_SYN) && (tcp->flags & TCP_FLAG_ACK)) {
      pcb->rcv_nxt = seq + 1;
      pcb->state = TCP_ESTABLISHED;
      tcp_send_packet(pcb, TCP_FLAG_ACK, NULL, 0);
    }
  } else if (pcb->state == TCP_ESTABLISHED) {
    uint32_t header_len = tcp->header_len * 4;
    uint32_t data_len = length - header_len;

    if (data_len > 0) {
      // ✅ GUARDAR EN BUFFER INTERNO SIEMPRE
      if (pcb->internal_rx_len + data_len <= sizeof(pcb->internal_rx_buffer)) {
        memcpy(pcb->internal_rx_buffer + pcb->internal_rx_len,
               packet + header_len, data_len);
        pcb->internal_rx_len += data_len;
        pcb->rcv_nxt += data_len;
        tcp_send_packet(pcb, TCP_FLAG_ACK, NULL, 0);
      }
    }

    if (tcp->flags & TCP_FLAG_FIN) {
      pcb->state = TCP_CLOSE_WAIT;
      pcb->rcv_nxt++;
      tcp_send_packet(pcb, TCP_FLAG_ACK, NULL, 0);
    }
  }
}

int tcp_send(int socket_id, const uint8_t *data, uint32_t len) {
  if (socket_id < 0 || socket_id >= TCP_MAX_CONNECTIONS)
    return -1;
  tcp_pcb_t *pcb = &tcp_pcbs[socket_id];
  if (pcb->state != TCP_ESTABLISHED)
    return -1;
  return tcp_send_packet(pcb, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len) ? len
                                                                      : -1;
}

int tcp_receive(int socket_id, uint8_t *buffer, uint32_t len) {
  if (socket_id < 0 || socket_id >= TCP_MAX_CONNECTIONS)
    return -1;
  tcp_pcb_t *pcb = &tcp_pcbs[socket_id];

  uint32_t start = ticks_since_boot;
  while (ticks_since_boot - start < 500) {
    // 1. COPIAR DEL BUFFER INTERNO SI HAY DATOS (SIEMPRE PRIORIZAR DATOS)
    if (pcb->internal_rx_len > 0) {
      uint32_t to_copy =
          (pcb->internal_rx_len < len) ? pcb->internal_rx_len : len;
      memcpy(buffer, pcb->internal_rx_buffer, to_copy);

      if (to_copy < pcb->internal_rx_len) {
        memmove(pcb->internal_rx_buffer, pcb->internal_rx_buffer + to_copy,
                pcb->internal_rx_len - to_copy);
      }
      pcb->internal_rx_len -= to_copy;
      return (int)to_copy;
    }

    // 2. SI NO HAY DATOS, VERIFICAR SI LA CONEXIÓN SIGUE ABIERTA
    if (pcb->state == TCP_CLOSED || pcb->state == TCP_CLOSE_WAIT) {
      return -2; // Conexión cerrada y sin datos
    }

    // 3. PROCESAR RED
    uint32_t f;
    __asm__ __volatile__("pushf; cli; pop %0" : "=r"(f));
    network_stack_tick();
    __asm__ __volatile__("push %0; popf" : : "r"(f));

    for (volatile int i = 0; i < 5000; i++)
      __asm__ __volatile__("pause");
  }
  return 0; // Timeout, reintentar
}

void tcp_close(int socket_id) {
  if (socket_id >= 0 && socket_id < TCP_MAX_CONNECTIONS) {
    tcp_pcb_t *pcb = &tcp_pcbs[socket_id];
    if (pcb->state == TCP_ESTABLISHED) {
      tcp_send_packet(pcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    }
    pcb->state = TCP_CLOSED;
  }
}

void tcp_maintenance(void) {}
