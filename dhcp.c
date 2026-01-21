#include "dhcp.h"
#include "e1000.h"
#include "irq.h"
#include "kernel.h"
#include "memory.h"
#include "network_stack.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"
#include "udp.h"

static dhcp_state_t dhcp_state = DHCP_STATE_IDLE;
static uint32_t dhcp_xid = 0x12345678;
static uint32_t dhcp_timeout_ticks = 0;
static int dhcp_retries = 0;

static ip_addr_t offered_ip;
static ip_addr_t server_id;
static ip_addr_t dhcp_netmask;
static ip_addr_t dhcp_gateway;
static ip_addr_t dhcp_dns;

void dhcp_handle_packet(ip_addr_t src_ip, uint16_t src_port, uint8_t *data,
                        uint32_t length);

void dhcp_init(void) {
  udp_bind(DHCP_CLIENT_PORT, dhcp_handle_packet);
  dhcp_state = DHCP_STATE_IDLE;
}

static void dhcp_add_option(uint8_t *options, int *offset, uint8_t type,
                            uint8_t len, const void *data) {
  options[(*offset)++] = type;
  options[(*offset)++] = len;
  memcpy(&options[*offset], data, len);
  *offset += len;
}

static void dhcp_send_discover(void) {
  dhcp_packet_t packet;
  memset(&packet, 0, sizeof(dhcp_packet_t));

  packet.op = 1;    // BOOTREQUEST
  packet.htype = 1; // Ethernet
  packet.hlen = 6;
  packet.xid = htonl(dhcp_xid);
  packet.flags = htons(0x8000); // Broadcast flag
  packet.magic_cookie = htonl(0x63825363);

  e1000_get_mac(packet.chaddr);

  int offset = 0;
  uint8_t msg_type = DHCP_DISCOVER;
  dhcp_add_option(packet.options, &offset, DHCP_OPT_MSG_TYPE, 1, &msg_type);

  // Parameter request list
  uint8_t param_list[] = {DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS,
                          DHCP_OPT_DOMAIN_NAME};
  dhcp_add_option(packet.options, &offset, DHCP_OPT_PARAMETER_LIST,
                  sizeof(param_list), param_list);

  packet.options[offset++] = DHCP_OPT_END;

  ip_addr_t broadcast = {255, 255, 255, 255};
  if (udp_send_packet(broadcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                      (uint8_t *)&packet, sizeof(dhcp_packet_t))) {
    serial_printf(COM1_BASE, "[DHCP] DISCOVER sent\r\n");
  } else {
    serial_printf(COM1_BASE, "[DHCP] Send failed!\r\n");
  }
}

static void dhcp_send_request(void) {
  dhcp_packet_t packet;
  memset(&packet, 0, sizeof(dhcp_packet_t));

  packet.op = 1;    // BOOTREQUEST
  packet.htype = 1; // Ethernet
  packet.hlen = 6;
  packet.xid = htonl(dhcp_xid);
  packet.flags = htons(0x8000); // Broadcast flag
  packet.magic_cookie = htonl(0x63825363);

  e1000_get_mac(packet.chaddr);

  int offset = 0;
  uint8_t msg_type = DHCP_REQUEST;
  dhcp_add_option(packet.options, &offset, DHCP_OPT_MSG_TYPE, 1, &msg_type);
  dhcp_add_option(packet.options, &offset, DHCP_OPT_REQUESTED_IP, 4,
                  offered_ip);
  dhcp_add_option(packet.options, &offset, DHCP_OPT_SERVER_ID, 4, server_id);

  packet.options[offset++] = DHCP_OPT_END;

  ip_addr_t broadcast = {255, 255, 255, 255};
  udp_send_packet(broadcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                  (uint8_t *)&packet, sizeof(dhcp_packet_t));

  serial_printf(COM1_BASE, "[DHCP] REQUEST sent\r\n");
}

bool dhcp_start(void) {
  serial_printf(COM1_BASE, "[DHCP] Starting on port %d, XID: 0x%x\r\n",
                DHCP_CLIENT_PORT, dhcp_xid);
  dhcp_xid++;
  dhcp_state = DHCP_STATE_DISCOVER;
  dhcp_retries = 0;
  dhcp_timeout_ticks = ticks_since_boot + 300; // 3 seconds
  dhcp_send_discover();
  return true;
}

void dhcp_handle_packet(ip_addr_t src_ip, uint16_t src_port, uint8_t *data,
                        uint32_t length) {

  if (length < 240)
    return;

  dhcp_packet_t *packet = (dhcp_packet_t *)data;
  if (ntohl(packet->magic_cookie) != 0x63825363)
    return;
  if (ntohl(packet->xid) != dhcp_xid)
    return;

  uint8_t msg_type = 0;
  uint8_t *opt = packet->options;
  while (*opt != DHCP_OPT_END && (uintptr_t)opt < (uintptr_t)data + length) {
    if (*opt == 0) { // PAD
      opt++;
      continue;
    }
    uint8_t type = *opt++;
    uint8_t len = *opt++;

    if (type == DHCP_OPT_MSG_TYPE) {
      msg_type = *opt;
    } else if (type == DHCP_OPT_SERVER_ID) {
      memcpy(server_id, opt, 4);
    } else if (type == DHCP_OPT_SUBNET_MASK) {
      memcpy(dhcp_netmask, opt, 4);
    } else if (type == DHCP_OPT_ROUTER) {
      memcpy(dhcp_gateway, opt, 4);
    } else if (type == DHCP_OPT_DNS) {
      memcpy(dhcp_dns, opt, 4);
    }

    opt += len;
  }

  if (msg_type == DHCP_OFFER && dhcp_state == DHCP_STATE_DISCOVER) {
    memcpy(offered_ip, &packet->yiaddr, 4);
    serial_printf(COM1_BASE, "[DHCP] OFFER: %d.%d.%d.%d from %d.%d.%d.%d\r\n",
                  offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3],
                  src_ip[0], src_ip[1], src_ip[2], src_ip[3]);
    dhcp_state = DHCP_STATE_REQUEST;
    dhcp_timeout_ticks = ticks_since_boot + 300;
    dhcp_send_request();
  } else if (msg_type == DHCP_ACK && dhcp_state == DHCP_STATE_REQUEST) {
    terminal_printf(&main_terminal, "[DHCP] ACK: %d.%d.%d.%d\r\n",
                    offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3]);

    network_config_t config;
    network_get_config(&config);

    memcpy(config.ip_address, offered_ip, 4);
    memcpy(config.netmask, dhcp_netmask, 4);
    memcpy(config.gateway, dhcp_gateway, 4);
    memcpy(config.dns_server, dhcp_dns, 4);
    config.dhcp_enabled = true;
    config.state = NET_STATE_READY;

    network_apply_config(&config);
    dhcp_state = DHCP_STATE_BOUND;
  }
}

void dhcp_tick(void) {
  if (dhcp_state == DHCP_STATE_IDLE || dhcp_state == DHCP_STATE_BOUND ||
      dhcp_state == DHCP_STATE_FAILED) {
    return;
  }

  if (ticks_since_boot > dhcp_timeout_ticks) {
    dhcp_retries++;
    if (dhcp_retries > 5) {
      terminal_puts(&main_terminal, "[DHCP] FAILED: Max retries reached\r\n");
      dhcp_state = DHCP_STATE_FAILED;
      return;
    }

    serial_printf(COM1_BASE, "[DHCP] Timeout, retrying... (%d/5)\r\n",
                  dhcp_retries);
    dhcp_timeout_ticks = ticks_since_boot + 300;

    if (dhcp_state == DHCP_STATE_DISCOVER) {
      dhcp_send_discover();
    } else if (dhcp_state == DHCP_STATE_REQUEST) {
      dhcp_send_request();
    }
  }
}

dhcp_state_t dhcp_get_state(void) { return dhcp_state; }
