#ifndef E1000_H
#define E1000_H

#include <stdbool.h>
#include <stdint.h>

// Registros E1000
#define E1000_REG_CTRL 0x00000     // Device Control
#define E1000_REG_STATUS 0x00008   // Device Status
#define E1000_REG_EEPROM 0x00014   // EEPROM
#define E1000_REG_CTRL_EXT 0x00018 // Extended Device Control
#define E1000_REG_IMASK 0x000D0    // Interrupt Mask
#define E1000_REG_ICR 0x000C0      // Interrupt Cause Read
#define E1000_REG_ICS 0x000C8      // Interrupt Cause Set
#define E1000_REG_IMS 0x000D0      // Interrupt Mask Set/Read
#define E1000_REG_IMC 0x000D8      // Interrupt Mask Clear
#define E1000_REG_RCTL 0x00100     // Receive Control
#define E1000_REG_TCTL 0x00400     // Transmit Control
#define E1000_REG_TIPG 0x00410     // Transmit IPG
#define E1000_REG_RDBAL 0x02800    // Receive Descriptor Base Address Low
#define E1000_REG_RDBAH 0x02804    // Receive Descriptor Base Address High
#define E1000_REG_RDLEN 0x02808    // Receive Descriptor Length
#define E1000_REG_RDH 0x02810      // Receive Descriptor Head
#define E1000_REG_RDT 0x02818      // Receive Descriptor Tail
#define E1000_REG_TDBAL 0x03800    // Transmit Descriptor Base Address Low
#define E1000_REG_TDBAH 0x03804    // Transmit Descriptor Base Address High
#define E1000_REG_TDLEN 0x03808    // Transmit Descriptor Length
#define E1000_REG_TDH 0x03810      // Transmit Descriptor Head
#define E1000_REG_TDT 0x03818      // Transmit Descriptor Tail
#define E1000_REG_RAL 0x05400      // Receive Address Low (MAC address)
#define E1000_REG_RAH 0x05404      // Receive Address High

// Comandos de control
#define E1000_CTRL_FD 0x00000001         // Full Duplex
#define E1000_CTRL_LRST 0x00000008       // Link Reset
#define E1000_CTRL_ASDE 0x00000020       // Auto-Speed Detection Enable
#define E1000_CTRL_SLU 0x00000040        // Set Link Up
#define E1000_CTRL_ILOS 0x00000080       // Invert Loss-of-Signal
#define E1000_CTRL_SPEED_10 0x00000000   // Speed 10Mb/s
#define E1000_CTRL_SPEED_100 0x00000100  // Speed 100Mb/s
#define E1000_CTRL_SPEED_1000 0x00000200 // Speed 1000Mb/s
#define E1000_CTRL_FRCSPD 0x00000800     // Force Speed
#define E1000_CTRL_FRCDPLX 0x00001000    // Force Duplex
#define E1000_CTRL_RST 0x04000000        // Device Reset
#define E1000_CTRL_PHY_RST 0x80000000    // PHY Reset

// Comandos de recepción
#define E1000_RCTL_EN 0x00000002         // Receiver Enable
#define E1000_RCTL_SBP 0x00000004        // Store Bad Packets
#define E1000_RCTL_UPE 0x00000008        // Unicast Promiscuous Enable
#define E1000_RCTL_MPE 0x00000010        // Multicast Promiscuous Enable
#define E1000_RCTL_LPE 0x00000020        // Long Packet Enable
#define E1000_RCTL_LBM_NO 0x00000000     // No Loopback
#define E1000_RCTL_LBM_PHY 0x000000C0    // PHY Loopback
#define E1000_RCTL_RDMTS_HALF 0x00000000 // Free Buffer Threshold
#define E1000_RCTL_RDMTS_QUARTER 0x00000100
#define E1000_RCTL_RDMTS_EIGHTH 0x00000200
#define E1000_RCTL_MO_36 0x00000400      // Multicast Offset
#define E1000_RCTL_BAM 0x00008000        // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 0x00000000 // Buffer Size 2048
#define E1000_RCTL_BSIZE_1024 0x00010000
#define E1000_RCTL_BSIZE_512 0x00020000
#define E1000_RCTL_BSIZE_256 0x00030000
#define E1000_RCTL_SECRC 0x04000000 // Strip Ethernet CRC
#define E1000_RCTL_BSEX 0x08000000  // Buffer Size Extension

// Comandos de transmisión
#define E1000_TCTL_EN 0x00000002     // Transmit Enable
#define E1000_TCTL_PSP 0x00000008    // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4        // Collision Threshold shift
#define E1000_TCTL_COLD_SHIFT 12     // Collision Distance shift
#define E1000_TCTL_SWXOFF 0x00400000 // Software XOFF Transmission
#define E1000_TCTL_RTLC 0x01000000   // Re-transmit on Late Collision

// Interrupciones
#define E1000_ICR_TXDW 0x00000001    // Transmit Descriptor Written Back
#define E1000_ICR_TXQE 0x00000002    // Transmit Queue Empty
#define E1000_ICR_LSC 0x00000004     // Link Status Change
#define E1000_ICR_RXSEQ 0x00000008   // Receive Sequence Error
#define E1000_ICR_RXDMT0 0x00000010  // Receive Descriptor Minimum Threshold
#define E1000_ICR_RXO 0x00000040     // Receive Overrun
#define E1000_ICR_RXT0 0x00000080    // Receive Timer Interrupt
#define E1000_ICR_MDAC 0x00000200    // MDIO Access Complete
#define E1000_ICR_RXCFG 0x00000400   // Receive /C/ Ordered Sets
#define E1000_ICR_GPI0 0x00000800    // General Purpose Interrupt 0
#define E1000_ICR_GPI1 0x00001000    // General Purpose Interrupt 1
#define E1000_ICR_TXD_LOW 0x00008000 // Transmit Descriptor Low Threshold
#define E1000_ICR_SRPD 0x00100000    // Small Receive Packet Detection
#define E1000_ICR_ACK 0x00200000     // Receive ACK Frame
#define E1000_ICR_MNG 0x00400000     // Manageability Event

// Descriptor flags
#define E1000_TXD_CMD_EOP 0x01  // End of Packet
#define E1000_TXD_CMD_IFCS 0x02 // Insert FCS (CRC)
#define E1000_TXD_CMD_IC 0x04   // Insert Checksum
#define E1000_TXD_CMD_RS 0x08   // Report Status
#define E1000_TXD_CMD_RPS 0x10  // Report Packet Sent
#define E1000_TXD_CMD_DEXT 0x20 // Descriptor Extension
#define E1000_TXD_CMD_VLE 0x40  // VLAN Packet Enable
#define E1000_TXD_STAT_DD 0x01  // Descriptor Done
#define E1000_TXD_STAT_EC 0x02  // Excess Collisions
#define E1000_TXD_STAT_LC 0x04  // Late Collision
#define E1000_TXD_STAT_TU 0x08  // Transmit Underrun
#define E1000_RXD_STAT_DD 0x01  // Descriptor Done
#define E1000_RXD_STAT_EOP 0x02 // End of Packet

// Tamaños
#define E1000_NUM_TX_DESC 64
#define E1000_NUM_RX_DESC 64
#define E1000_MAX_PKT_SIZE 1522 // 1518 + 4 para VLAN

// Estructuras de descriptores
typedef struct {
  uint64_t buffer_addr; // Dirección física del buffer
  uint16_t length;      // Longitud del paquete
  uint8_t cso;          // Checksum Offset
  uint8_t cmd;          // Comando
  uint8_t status;       // Estado
  uint8_t css;          // Checksum Start
  uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
  uint64_t buffer_addr; // Dirección física del buffer
  uint16_t length;      // Longitud recibida
  uint16_t checksum;    // Checksum
  uint8_t status;       // Estado
  uint8_t errors;       // Errores
  uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

// Estructura principal del driver
typedef struct {
  uint32_t io_base;  // Base I/O
  uint32_t mem_base; // Base de memoria (si es memory-mapped)
  uint8_t *mem_virt; // Dirección virtual mapeada
  uint8_t irq_line;  // Línea de interrupción

  // Buffers y descriptores
  e1000_tx_desc_t *tx_descs;
  e1000_rx_desc_t *rx_descs;
  uint8_t *tx_buffers[E1000_NUM_TX_DESC];
  uint8_t *rx_buffers[E1000_NUM_RX_DESC];

  // Índices
  uint32_t tx_curr; // Índice de transmisión actual
  uint32_t rx_curr; // Índice de recepción actual

  // MAC address
  uint8_t mac_addr[6];

  // Estado
  bool initialized;
  bool link_up;

  // Estadísticas
  uint32_t tx_packets;
  uint32_t rx_packets;
  uint32_t tx_bytes;
  uint32_t rx_bytes;
  uint32_t tx_errors;
  uint32_t rx_errors;
} e1000_device_t;

// Funciones públicas
bool e1000_init(void);
bool e1000_send_packet(const uint8_t *data, uint32_t length);
uint32_t e1000_receive_packet(uint8_t *buffer, uint32_t max_len);
void e1000_get_mac(uint8_t *mac);
bool e1000_is_link_up(void);
void e1000_handle_interrupt(void);
void e1000_print_stats(void);
void e1000_check_status(void);
void e1000_reset_tx_ring(void);

extern e1000_device_t e1000_device;

// Funciones de driver system
int e1000_driver_register_type(void);
struct driver_instance *e1000_driver_create(const char *name);

#endif // E1000_H