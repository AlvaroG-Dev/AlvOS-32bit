#ifndef NETWORK_STACK_H
#define NETWORK_STACK_H

#include <stdbool.h>
#include <stdint.h>

// Direcciones IP
typedef uint8_t ip_addr_t[4];

// Constantes de red (ahora como arrays estáticos, no macros)
static const ip_addr_t DEFAULT_IP_ADDR = {10, 0, 2, 15};
static const ip_addr_t DEFAULT_NETMASK = {255, 255, 255, 0};
static const ip_addr_t DEFAULT_GATEWAY = {10, 0, 2, 2};
static const ip_addr_t DEFAULT_DNS = {10, 0, 2, 3};

// Estados de la pila de red
typedef enum {
  NET_STATE_DOWN,
  NET_STATE_INIT,
  NET_STATE_IP_CONFIGURED,
  NET_STATE_DHCP_REQUESTING,
  NET_STATE_DHCP_ACK,
  NET_STATE_READY
} network_state_t;

// Estructura de configuración de red
typedef struct {
  ip_addr_t ip_address;
  ip_addr_t netmask;
  ip_addr_t gateway;
  ip_addr_t dns_server;
  uint8_t mac_address[6];
  network_state_t state;
  bool dhcp_enabled;
} network_config_t;

// Funciones principales
void network_stack_init(void);
void network_stack_tick(void);
bool network_send_ip_packet(const uint8_t *data, uint32_t length,
                            ip_addr_t dest_ip, uint8_t protocol);
uint32_t network_receive_ip_packet(uint8_t *buffer, uint32_t max_len,
                                   ip_addr_t *src_ip, uint8_t *protocol);

// Funciones de configuración
void network_set_static_ip(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway);
void network_apply_config(const network_config_t *config);
bool network_dhcp_request(void);
void network_get_config(network_config_t *config);
void network_print_config(void);

// Utilidades
void ip_to_string(ip_addr_t ip, char *buffer);
bool string_to_ip(const char *str, ip_addr_t ip);

#endif // NETWORK_STACK_H