#include "serial.h"
#include "io.h"
#include "irq.h"
#include "terminal.h"
#include <stdbool.h>

#define SERIAL_TX_QUEUE_SIZE 256

static volatile char tx_queue_com1[SERIAL_TX_QUEUE_SIZE];
static volatile int tx_head_com1 = 0, tx_tail_com1 = 0;
static volatile bool tx_busy_com1 = false;

static volatile char tx_queue_com2[SERIAL_TX_QUEUE_SIZE];
static volatile int tx_head_com2 = 0, tx_tail_com2 = 0;
static volatile bool tx_busy_com2 = false;

static inline void serial_enable_thre(uint16_t port) {
    uint8_t ier = inb(port + UART_IER);
    if (!(ier & UART_IER_THRE)) {
        outb(port + UART_IER, ier | UART_IER_THRE);
    }
}

static inline void serial_disable_thre(uint16_t port) {
    uint8_t ier = inb(port + UART_IER);
    if (ier & UART_IER_THRE) {
        outb(port + UART_IER, ier & ~UART_IER_THRE);
    }
}

void serial_init(void) {
    const uint16_t ports[] = { COM1_BASE, COM2_BASE };
    const char* names[] = { "COM1", "COM2" };

    for (int i = 0; i < 2; ++i) {
        uint16_t port = ports[i];

        outb(port + UART_IER, 0x00);                 // deshabilita IRQs
        outb(port + UART_LCR, UART_LCR_DLAB);        // DLAB=1
        const uint16_t divisor = 1;                  // 115200
        outb(port + UART_DLL, divisor & 0xFF);
        outb(port + UART_DLH, (divisor >> 8) & 0xFF);
        outb(port + UART_LCR, UART_LCR_8N1);         // 8N1, DLAB=0
        outb(port + UART_FCR, 0xC7);                 // FIFO on, clear, trig=14
        outb(port + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

        // Arranca con interrupción de RX habilitada (THRE se habilita bajo demanda)
        outb(port + UART_IER, UART_IER_RX);

        terminal_printf(&main_terminal, "%s initialized (115200 8N1)\r\n", names[i]);
    }
}

/**
 * Handler específico para IRQ 3 o 4 (serial). Pásale 3 ó 4 directamente desde el stub ASM.
 */
void serial_irq_handler_line(int irq)
{
    if (irq != 3 && irq != 4) {
        pic_send_eoi(irq);
        return;
    }

    uint16_t port = (irq == 4) ? COM1_BASE : COM2_BASE;

    volatile char* tx_queue = (port == COM1_BASE) ? tx_queue_com1 : tx_queue_com2;
    volatile int*  tx_head  = (port == COM1_BASE) ? &tx_head_com1  : &tx_head_com2;
    volatile int*  tx_tail  = (port == COM1_BASE) ? &tx_tail_com1  : &tx_tail_com2;
    volatile bool* tx_busy  = (port == COM1_BASE) ? &tx_busy_com1  : &tx_busy_com2;

    // Atiende todas las causas pendientes
    for (;;) {
        uint8_t iir = inb(port + UART_IIR);
        if (iir & 0x01) break; // no pending

        switch (iir & 0x0E) {
            case 0x04: { // Received Data Available
                while (serial_can_read(port)) {
                    char c = serial_read_char(port);
                    terminal_handle_key(&main_terminal, c);
                }
            } break;

            case 0x02: { // THRE (Transmitter Holding Register Empty)
                // Rellena tanto como puedas (FIFO TX) o hasta vaciar la cola
                int sent = 0;
                while (*tx_tail != *tx_head && serial_can_write(port)) {
                    outb(port + UART_DATA, tx_queue[*tx_tail]);
                    *tx_tail = (*tx_tail + 1) % SERIAL_TX_QUEUE_SIZE;
                    ++sent;
                }
                if (*tx_tail == *tx_head) {
                    // Cola vacía: ya no estamos ocupados y no necesitamos más THRE
                    *tx_busy = false;
                    serial_disable_thre(port);
                } else {
                    // Aún queda cola: asegura THRE habilitado
                    serial_enable_thre(port);
                }
                (void)sent;
            } break;

            case 0x06: {
                // Line Status (errors). Leer LSR limpia estados.
                (void)inb(port + UART_LSR);
            } break;

            case 0x00: {
                // Modem Status. Leer MSR limpia.
                (void)inb(port + UART_MSR);
            } break;

            default:
                // Otras causas (character timeout, etc.). Leer RBR si aplica.
                if (serial_can_read(port)) {
                    (void)serial_read_char(port);
                }
                break;
        }
    }

    pic_send_eoi(irq);
}

int serial_write_char(uint16_t port, char c) {
    if (port != COM1_BASE && port != COM2_BASE) {
        return -1;
    }

    volatile char* tx_queue = (port == COM1_BASE) ? tx_queue_com1 : tx_queue_com2;
    volatile int*  tx_head  = (port == COM1_BASE) ? &tx_head_com1  : &tx_head_com2;
    volatile int*  tx_tail  = (port == COM1_BASE) ? &tx_tail_com1  : &tx_tail_com2;
    volatile bool* tx_busy  = (port == COM1_BASE) ? &tx_busy_com1  : &tx_busy_com2;

    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

    int next_head = (*tx_head + 1) % SERIAL_TX_QUEUE_SIZE;
    if (next_head == *tx_tail) {
        __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
        return -1; // cola llena
    }

    if (*tx_busy) {
        tx_queue[*tx_head] = c;
        *tx_head = next_head;
        serial_enable_thre(port); // por si acaso se deshabilitó
    } else {
        *tx_busy = true;
        // Si THR está libre, envía directo; si no, encola.
        if (serial_can_write(port)) {
            outb(port + UART_DATA, c);
            // Asegura que el próximo vaciado ocurra por IRQ THRE
            serial_enable_thre(port);
        } else {
            tx_queue[*tx_head] = c;
            *tx_head = next_head;
            serial_enable_thre(port);
        }
    }

    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
}

void serial_write_string(uint16_t port, const char* s) {
    while (*s) {
        if (serial_write_char(port, *s++) != 0) break;
    }
}

/**
 * Formatted print to serial port.
 */
void serial_printf(uint16_t port, const char *fmt, ...) {
    if (port != COM1_BASE && port != COM2_BASE) {
        return;
    }

    char buf[1024]; // Buffer for formatted string
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0 || len >= (int)sizeof(buf)) {
        serial_write_string(port, "serial_printf: Format error or buffer overflow\n");
        return;
    }

    serial_write_string(port, buf);
}

char serial_read_char(uint16_t port) {
    while (!serial_can_read(port));
    return inb(port + UART_DATA);
}

int serial_can_write(uint16_t port) {
    return (inb(port + UART_LSR) & UART_LSR_THRE) != 0;
}

int serial_can_read(uint16_t port) {
    return (inb(port + UART_LSR) & UART_LSR_DR) != 0;
}
