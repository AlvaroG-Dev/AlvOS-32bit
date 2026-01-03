#include "network_stack.h"
#include "arp.h"
#include "e1000.h"
#include "icmp.h"
#include "ipv4.h"
#include "irq.h"
#include "kernel.h"
#include "network.h"
#include "string.h"
#include "terminal.h"

static network_config_t net_config;

void network_stack_init(void) {
  terminal_puts(&main_terminal, "\r\n=== Network Stack Initialization ===\r\n");

  // Inicializar configuración por defecto
  memcpy(net_config.ip_address, DEFAULT_IP_ADDR, 4);
  memcpy(net_config.netmask, DEFAULT_NETMASK, 4);
  memcpy(net_config.gateway, DEFAULT_GATEWAY, 4);
  memcpy(net_config.dns_server, DEFAULT_DNS, 4);
  e1000_get_mac(net_config.mac_address);

  net_config.state = NET_STATE_INIT;
  net_config.dhcp_enabled = false;

  // Inicializar subsistemas
  arp_init();

  // Configurar IP
  ip_set_address(net_config.ip_address, net_config.netmask, net_config.gateway);

  net_config.state = NET_STATE_IP_CONFIGURED;

  terminal_puts(&main_terminal, "[NET] Network stack initialized\r\n");
  network_print_config();
}

// Procesar paquetes recibidos
void network_stack_tick(void) {
  static uint32_t last_arp_cleanup = 0;

  // Recibir y procesar paquetes
  uint8_t buffer[1522];
  uint32_t length = e1000_receive_packet(buffer, sizeof(buffer));

  if (length > 0) {
    // Determinar tipo de paquete
    if (length >= 14) {
      ethernet_header_t *eth = (ethernet_header_t *)buffer;
      uint16_t ethertype = ntohs(eth->type);

      switch (ethertype) {
      case ETHERTYPE_ARP:
        arp_process_packet(buffer, length);
        break;

      case ETHERTYPE_IP: {
        ip_addr_t src_ip;
        uint8_t protocol;

        // CORRECCIÓN: Convertir endianness
        uint16_t ethertype = (eth->type << 8) | (eth->type >> 8);

        if (ethertype == ETHERTYPE_IP &&
            ip_process_packet(buffer, length, &src_ip, &protocol)) {
          // Determinar protocolo de capa 4
          uint8_t *ip_payload = buffer + sizeof(ethernet_header_t) + 20;
          uint32_t ip_payload_len = length - sizeof(ethernet_header_t) - 20;

          switch (protocol) {
          case IP_PROTOCOL_ICMP:
            icmp_process_packet(src_ip, ip_payload, ip_payload_len);
            break;

          case IP_PROTOCOL_TCP:
            // TCP vendrá después
            break;

          case IP_PROTOCOL_UDP:
            // UDP vendrá después
            break;

          default:
            terminal_printf(&main_terminal, "[NET] Unknown IP protocol: %u\r\n",
                            protocol);
            break;
          }
        }
        break;
      }

      default:
        // Ignorar otros tipos por ahora
        break;
      }
    }
  }

  // Limpiar cache ARP periódicamente (cada 60 segundos)
  if (ticks_since_boot - last_arp_cleanup >
      6000) { // 6000 ticks = 60 segundos a 100Hz
    // Podríamos implementar aging aquí
    last_arp_cleanup = ticks_since_boot;
  }
}

// Enviar paquete IP
bool network_send_ip_packet(const uint8_t *data, uint32_t length,
                            ip_addr_t dest_ip, uint8_t protocol) {
  return ip_send_packet(dest_ip, protocol, (uint8_t *)data, length);
}

// Configuración estática
void network_set_static_ip(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway) {
  memcpy(net_config.ip_address, ip, 4);
  memcpy(net_config.netmask, netmask, 4);
  memcpy(net_config.gateway, gateway, 4);

  ip_set_address(ip, netmask, gateway);
  net_config.state = NET_STATE_IP_CONFIGURED;
  net_config.dhcp_enabled = false;
}

// Obtener configuración
void network_get_config(network_config_t *config) {
  if (config) {
    memcpy(config, &net_config, sizeof(network_config_t));
  }
}

// Mostrar configuración
void network_print_config(void) {
  char ip_str[16], mask_str[16], gw_str[16], dns_str[16], mac_str[18];

  ip_to_string(net_config.ip_address, ip_str);
  ip_to_string(net_config.netmask, mask_str);
  ip_to_string(net_config.gateway, gw_str);
  ip_to_string(net_config.dns_server, dns_str);

  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           net_config.mac_address[0], net_config.mac_address[1],
           net_config.mac_address[2], net_config.mac_address[3],
           net_config.mac_address[4], net_config.mac_address[5]);

  terminal_puts(&main_terminal, "\r\n=== Network Configuration ===\r\n");
  terminal_printf(&main_terminal, "Interface: eth0\r\n");
  terminal_printf(&main_terminal, "MAC Address: %s\r\n", mac_str);
  terminal_printf(&main_terminal, "IP Address:  %s\r\n", ip_str);
  terminal_printf(&main_terminal, "Netmask:     %s\r\n", mask_str);
  terminal_printf(&main_terminal, "Gateway:     %s\r\n", gw_str);
  terminal_printf(&main_terminal, "DNS Server:  %s\r\n", dns_str);
  terminal_printf(&main_terminal, "DHCP:        %s\r\n",
                  net_config.dhcp_enabled ? "Enabled" : "Disabled");

  const char *state_str;
  switch (net_config.state) {
  case NET_STATE_DOWN:
    state_str = "DOWN";
    break;
  case NET_STATE_INIT:
    state_str = "INIT";
    break;
  case NET_STATE_IP_CONFIGURED:
    state_str = "IP CONFIGURED";
    break;
  case NET_STATE_READY:
    state_str = "READY";
    break;
  default:
    state_str = "UNKNOWN";
    break;
  }
  terminal_printf(&main_terminal, "State:       %s\r\n", state_str);
}

// Utilidades
void ip_to_string(ip_addr_t ip, char *buffer) {
  snprintf(buffer, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

bool string_to_ip(const char *str, ip_addr_t ip) {
  int values[4];

  if (sscanf(str, "%d.%d.%d.%d", &values[0], &values[1], &values[2],
             &values[3]) != 4) {
    return false;
  }

  for (int i = 0; i < 4; i++) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    ip[i] = (uint8_t)values[i];
  }

  return true;
}