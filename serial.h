#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include "kernel.h"
#include "isr.h"

// Puertos base para COM1 y COM2
#define COM1_BASE 0x3F8
#define COM2_BASE 0x2F8

// Offsets de registros UART 16550
#define UART_DATA   0x00  // RBR/THR
#define UART_DLL    0x00  // Divisor Latch Low (cuando DLAB=1)
#define UART_DLH    0x01  // Divisor Latch High (cuando DLAB=1)
#define UART_IER    0x01  // Interrupt Enable Register
#define UART_IIR    0x02  // Interrupt Identification Register (read)
#define UART_FCR    0x02  // FIFO Control Register (write)
#define UART_LCR    0x03  // Line Control Register
#define UART_MCR    0x04  // Modem Control Register
#define UART_LSR    0x05  // Line Status Register
#define UART_MSR    0x06  // Modem Status Register
#define UART_SCR    0x07  // Scratch Register

// Line Control Register bits
#define UART_LCR_DLAB    0x80 // Divisor Latch Access Bit
#define UART_LCR_8N1     0x03 // 8 bits, no parity, 1 stop bit

// Interrupt Enable Register bits
#define UART_IER_RX      0x01 // Enable Received Data Available Interrupt
#define UART_IER_THRE    0x02 // Enable Transmitter Holding Register Empty Interrupt

// Line Status Register bits
#define UART_LSR_DR      0x01 // Data Ready
#define UART_LSR_THRE    0x20 // Transmitter Holding Register Empty

// Modem Control Register bits
#define UART_MCR_DTR     0x01 // Data Terminal Ready
#define UART_MCR_RTS     0x02 // Request To Send
#define UART_MCR_OUT2    0x08 // OUT2 (necesario para interrupciones)

// Función para inicializar puertos seriales
void serial_init(void);

// Funciones para manejar interrupciones
void serial_irq_handler_line(int irq);

// Funciones de E/S
int serial_write_char(uint16_t port, char c);
void serial_write_string(uint16_t port, const char* str);
char serial_read_char(uint16_t port);
void serial_printf(uint16_t port, const char *fmt, ...);
int serial_can_write(uint16_t port);
int serial_can_read(uint16_t port);

#endif