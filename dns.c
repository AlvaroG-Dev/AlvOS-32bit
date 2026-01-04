#include "dns.h"
#include "irq.h"
#include "kernel.h"        // Para malloc
#include "network_stack.h" // Para obtener DNS server IP y network_stack_tick
#include "string.h"
#include "terminal.h"
#include "udp.h"

// Variables estáticas para manejar la respuesta
static bool dns_response_received = false;
static ip_addr_t dns_resolved_ip;
static uint16_t dns_pending_id = 0;

// Callback para UDP cuando llega respuesta DNS
static void dns_udp_handler(ip_addr_t src_ip, uint16_t src_port, uint8_t *data,
                            uint32_t length) {
  if (length < sizeof(dns_header_t))
    return;

  dns_header_t *header = (dns_header_t *)data;
  uint16_t id = ntohs(header->id);

  // Verificar si es la respuesta que esperamos
  if (id == dns_pending_id) {
    // Verificar flags: QR bit debe ser 1 (Response) y RCODE debe ser 0 (No
    // Error)
    uint16_t flags = ntohs(header->flags);
    if ((flags & DNS_FLAG_QR) && (flags & DNS_FLAG_RCODE) == 0) {

      // Parsear respuesta (simplificado)
      // Asumimos 1 pregunta y buscamos la primera respuesta tipo A

      uint8_t *ptr = data + sizeof(dns_header_t);
      int q_count = ntohs(header->q_count);
      int ans_count = ntohs(header->ans_count);

      // Saltar preguntas
      for (int i = 0; i < q_count; i++) {
        // Saltar nombre
        while (*ptr != 0) {
          if (*ptr >= 192) { // Puntero comprimido
            ptr += 2;
            goto name_skipped;
          }
          ptr += (*ptr) + 1;
        }
        ptr++; // Saltar el byte 0 final

      name_skipped:
        ptr += 4; // Saltar Type y Class
      }

      // Procesar respuestas
      for (int i = 0; i < ans_count; i++) {
        // Saltar nombre de respuesta (normalmente un puntero 0xC0xx)
        if (*ptr >= 192) {
          ptr += 2;
        } else {
          while (*ptr != 0) {
            ptr += (*ptr) + 1;
          }
          ptr++;
        }

        uint16_t type = ntohs(*(uint16_t *)ptr);
        ptr += 2;
        uint16_t class = ntohs(*(uint16_t *)ptr);
        ptr += 2;
        ptr += 4; // TTL
        uint16_t data_len = ntohs(*(uint16_t *)ptr);
        ptr += 2;

        if (type == 1 && class == 1 && data_len == 4) { // Type A, Class IN
          memcpy(dns_resolved_ip, ptr, 4);
          dns_response_received = true;
          return;
        }

        ptr += data_len; // Saltar datos si no es lo que buscamos
      }
    }
  }
}

void dns_init(void) {
  udp_bind(5353,
           dns_udp_handler); // Usamos un puerto origen arbitrario, ej 5353
  terminal_puts(&main_terminal, "[DNS] Resolver initialized\r\n");
}

// Convertir "google.com" a formato DNS "\x06google\x03com\x00"
static void dns_format_hostname(uint8_t *dest, const char *hostname) {
  int lock = 0;
  const char *ptr = hostname;

  // Calcular longitud del segmento
  while (*ptr) {
    const char *next = strchr(ptr, '.');
    int len;
    if (next) {
      len = next - ptr;
    } else {
      len = strlen(ptr);
    }

    // Limitar longitud para evitar overflow de uint8_t
    if (len > 63)
      len = 63;

    dest[lock++] = len;
    memcpy(&dest[lock], ptr, len);
    lock += len;

    if (next) {
      ptr = next + 1;
    } else {
      break;
    }
  }
  dest[lock] = 0; // Terminador
}

bool dns_resolve(const char *hostname, ip_addr_t ip) {
  network_config_t config;
  network_get_config(&config);

  // Si no tenemos DNS server configurado, fallar
  ip_addr_t empty_ip = {0, 0, 0, 0};
  if (memcmp(config.dns_server, empty_ip, 4) == 0) {
    terminal_puts(&main_terminal, "[DNS] No DNS server configured\r\n");
    return false;
  }

  uint16_t query_id = (uint16_t)(ticks_since_boot & 0xFFFF);
  dns_pending_id = query_id;
  dns_response_received = false;

  // Construir paquete query
  uint8_t packet[512];
  memset(packet, 0, sizeof(packet));

  dns_header_t *header = (dns_header_t *)packet;
  header->id = htons(query_id);
  header->flags = htons(DNS_FLAG_RD);
  header->q_count = htons(1);
  header->ans_count = 0;
  header->auth_count = 0;
  header->add_count = 0;

  uint8_t *q_ptr = packet + sizeof(dns_header_t);
  dns_format_hostname(q_ptr, hostname);

  uint8_t *walker = q_ptr;
  while (*walker != 0)
    walker += (*walker) + 1;
  walker++;

  *(uint16_t *)walker = htons(1); // Type A
  walker += 2;
  *(uint16_t *)walker = htons(1); // Class IN
  walker += 2;

  uint32_t packet_len = walker - packet;

  terminal_printf(&main_terminal, "[DNS] Resolving %s via %d.%d.%d.%d...\r\n",
                  hostname, config.dns_server[0], config.dns_server[1],
                  config.dns_server[2], config.dns_server[3]);

  // Enviar con retransmisión
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      terminal_printf(&main_terminal, "[DNS] Attempt %d/3\r\n", attempt + 1);
    }

    udp_send_packet(config.dns_server, 5353, 53, packet, packet_len);

    uint32_t start = ticks_since_boot;
    uint32_t timeout = 200; // 2 segundos por intento

    while (ticks_since_boot - start < timeout) {
      // CRÍTICO: Procesar pila de red regularmente
      network_stack_tick();

      if (dns_response_received) {
        memcpy(ip, dns_resolved_ip, 4);
        terminal_printf(&main_terminal, "[DNS] Resolved %s to %d.%d.%d.%d\r\n",
                        hostname, ip[0], ip[1], ip[2], ip[3]);
        return true;
      }

      // NO usar HLT aquí - puede bloquear si no hay interrupciones
      // En su lugar, pequeña pausa activa
      for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("pause");
      }
    }

    terminal_puts(&main_terminal, "[DNS] Timeout\r\n");
  }

  terminal_puts(&main_terminal, "[DNS] Resolution failed\r\n");
  return false;
}