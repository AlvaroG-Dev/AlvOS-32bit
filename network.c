#include "network.h"
#include "e1000.h"
#include "kernel.h"
#include "string.h"
#include "terminal.h"

static bool network_initialized = false;

void network_init(void) {
  terminal_puts(&main_terminal, "\r\n=== Network Layer Initialization ===\r\n");

  if (e1000_init()) {
    network_initialized = true;
    terminal_puts(&main_terminal, "[NETWORK] Successfully initialized\r\n");
  } else {
    terminal_puts(&main_terminal, "[NETWORK] Failed to initialize\r\n");
  }
}

bool network_send_packet(const uint8_t *data, uint32_t length) {
  if (!network_initialized) {
    return false;
  }

  return e1000_send_packet(data, length);
}

uint32_t network_receive_packet(uint8_t *buffer, uint32_t max_len) {
  if (!network_initialized) {
    return 0;
  }

  return e1000_receive_packet(buffer, max_len);
}

void network_get_mac(mac_addr_t mac) {
  if (network_initialized) {
    e1000_get_mac(mac);
  } else {
    memset(mac, 0, 6);
  }
}

bool network_is_link_up(void) {
  if (!network_initialized) {
    return false;
  }

  return e1000_is_link_up();
}

void network_print_stats(void) { e1000_print_stats(); }

void mac_to_string(const mac_addr_t mac, char *buffer) {
  snprintf(buffer, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

bool string_to_mac(const char *str, mac_addr_t mac) {
  int values[6];

  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
             &values[3], &values[4], &values[5]) != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    mac[i] = (uint8_t)values[i];
  }

  return true;
}

uint16_t htons(uint16_t hostshort) {
  return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

uint16_t ntohs(uint16_t netshort) { return htons(netshort); }

uint32_t htonl(uint32_t hostlong) {
  return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
         ((hostlong >> 8) & 0xFF00) | ((hostlong >> 24) & 0xFF);
}

uint32_t ntohl(uint32_t netlong) { return htonl(netlong); }