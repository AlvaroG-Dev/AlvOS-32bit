#ifndef DNS_H
#define DNS_H

#include "ipv4.h"
#include <stdbool.h>
#include <stdint.h>


// Definiciones DNS
#define DNS_PORT 53

// Estructura de cabecera DNS
typedef struct {
  uint16_t id;
  uint16_t flags;
  uint16_t q_count;    // Number of questions
  uint16_t ans_count;  // Number of answers
  uint16_t auth_count; // Number of authority records
  uint16_t add_count;  // Number of additional records
} __attribute__((packed)) dns_header_t;

// Flags DNS
#define DNS_FLAG_QR (1 << 15)    // Query/Response
#define DNS_FLAG_OPCODE (0x7800) // Opcode mask
#define DNS_FLAG_RD (1 << 8)     // Recursion Desired
#define DNS_FLAG_RA (1 << 7)     // Recursion Available
#define DNS_FLAG_RCODE (0x000F)  // Response Code mask

// Funciones
void dns_init(void);

// Resolver un nombre de dominio a una dirección IP
// Bloqueante (espera hasta timeout o respuesta)
bool dns_resolve(const char *hostname, ip_addr_t ip);

#endif // DNS_H
