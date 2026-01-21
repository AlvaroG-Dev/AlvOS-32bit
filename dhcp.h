#ifndef DHCP_H
#define DHCP_H

#include "ipv4.h"
#include <stdbool.h>
#include <stdint.h>

// DHCP Ports
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

// DHCP Message Types
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_DECLINE 4
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_RELEASE 7

// DHCP Options
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_HOST_NAME 12
#define DHCP_OPT_DOMAIN_NAME 15
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_PARAMETER_LIST 55
#define DHCP_OPT_END 255

typedef struct {
  uint8_t op;            // Opcode: 1 = bootrequest, 2 = bootreply
  uint8_t htype;         // Hardware address type (1 = Ethernet)
  uint8_t hlen;          // Hardware address length (6 for Ethernet)
  uint8_t hops;          // Relay agent hops
  uint32_t xid;          // Transaction ID
  uint16_t secs;         // Seconds elapsed since client began
  uint16_t flags;        // Flags (0x8000 = broadcast)
  uint32_t ciaddr;       // Client IP address
  uint32_t yiaddr;       // 'Your' (client) IP address
  uint32_t siaddr;       // Next server IP address
  uint32_t giaddr;       // Relay agent IP address
  uint8_t chaddr[16];    // Client hardware address
  uint8_t sname[64];     // Server host name
  char file[128];        // Boot file name
  uint32_t magic_cookie; // Magic cookie: 0x63 0x82 0x53 0x63
  uint8_t options[312];  // Options field
} __attribute__((packed)) dhcp_packet_t;

// DHCP States
typedef enum {
  DHCP_STATE_IDLE,
  DHCP_STATE_DISCOVER,
  DHCP_STATE_REQUEST,
  DHCP_STATE_BOUND,
  DHCP_STATE_FAILED
} dhcp_state_t;

void dhcp_init(void);
bool dhcp_start(void);
void dhcp_tick(void);
dhcp_state_t dhcp_get_state(void);

#endif // DHCP_H
