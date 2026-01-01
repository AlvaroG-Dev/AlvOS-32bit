#include "serial.h"
#include "chardev.h"  // <-- Añadido para usar estructuras comunes
#include "io.h"
#include "irq.h"
#include "terminal.h"
#include "vfs.h"
#include <stdbool.h>
#include "task.h"

#define SERIAL_TX_QUEUE_SIZE 256
#define SERIAL_RX_QUEUE_SIZE 1024

// ============================================================================
// QUEUES FOR TX/RX
// ============================================================================
static volatile char tx_queue_com1[SERIAL_TX_QUEUE_SIZE];
static volatile int tx_head_com1 = 0, tx_tail_com1 = 0;
static volatile bool tx_busy_com1 = false;

static volatile char tx_queue_com2[SERIAL_TX_QUEUE_SIZE];
static volatile int tx_head_com2 = 0, tx_tail_com2 = 0;
static volatile bool tx_busy_com2 = false;

static volatile char rx_queue_com1[SERIAL_RX_QUEUE_SIZE];
static volatile int rx_head_com1 = 0, rx_tail_com1 = 0;

static volatile char rx_queue_com2[SERIAL_RX_QUEUE_SIZE];
static volatile int rx_head_com2 = 0, rx_tail_com2 = 0;

static bool serial_initialized = false;

// ============================================================================
// ESTRUCTURAS PRIVADAS PARA SERIAL COMO CHARDEF
// ============================================================================
typedef struct {
    uint16_t port;
    const char *name;
    uint8_t minor;
} serial_priv_t;

// ============================================================================
// OPERACIONES CHARDEF PARA SERIAL
// ============================================================================
static int serial_chardev_read(uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)offset;
    if (!priv || !buf || size == 0) return 0;
    
    serial_priv_t *serial = (serial_priv_t *)priv;
    uint16_t port = serial->port;
    
    volatile char *rx_queue = (port == COM1_BASE) ? rx_queue_com1 : rx_queue_com2;
    volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
    volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;
    
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    
    int available = (*rx_head - *rx_tail + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
    uint32_t to_read = (available < size) ? available : size;
    uint32_t read_bytes = 0;
    
    while (read_bytes < to_read) {
        buf[read_bytes] = rx_queue[*rx_tail];
        *rx_tail = (*rx_tail + 1) % SERIAL_RX_QUEUE_SIZE;
        read_bytes++;
    }
    
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    
    if (read_bytes > 0) {
        serial_printf(COM1_BASE, "serial_chardev_read: Read %u bytes from %s\r\n", 
                     read_bytes, serial->name);
    }
    
    return read_bytes;
}

static int serial_chardev_write(const uint8_t *buf, uint32_t size, uint32_t offset, void *priv) {
    (void)offset;
    if (!priv || !buf || size == 0) return 0;
    
    serial_priv_t *serial = (serial_priv_t *)priv;
    uint16_t port = serial->port;
    
    uint32_t bytes_written = 0;
    
    for (uint32_t i = 0; i < size; i++) {
        if (serial_write_char(port, buf[i]) != 0) {
            break; // Error
        }
        bytes_written++;
    }
    
    return bytes_written;
}

static int serial_chardev_ioctl(uint32_t cmd, void *arg, void *priv) {
    if (!priv) return -1;
    
    serial_priv_t *serial = (serial_priv_t *)priv;
    uint16_t port = serial->port;
    
    switch (cmd) {
        case 0x5401: // TCGETS (get terminal attributes)
            if (arg) memset(arg, 0, 60); // Tamaño aproximado de termios
            return 0;
        case 0x5402: // TCSETS (set terminal attributes)
            return 0; // Aceptar cualquier configuración
        case 0x541B: // FIONREAD (bytes disponibles)
            if (arg) {
                volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
                volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;
                int available = (*rx_head - *rx_tail + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
                *(int *)arg = available;
            }
            return 0;
        case 0x5421: // TIOCGWINSZ (get window size)
            if (arg) {
                // Estructura winsize
                struct winsize {
                    unsigned short ws_row;
                    unsigned short ws_col;
                    unsigned short ws_xpixel;
                    unsigned short ws_ypixel;
                } *ws = arg;
                ws->ws_row = 25;
                ws->ws_col = 80;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
            }
            return 0;
        default:
            return -1;
    }
}

static int serial_chardev_poll(void *priv) {
    if (!priv) return 0;
    
    serial_priv_t *serial = (serial_priv_t *)priv;
    uint16_t port = serial->port;
    
    volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
    volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;
    
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    int available = (*rx_head - *rx_tail + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    
    return available > 0 ? 1 : 0;
}

static void serial_chardev_release(void *priv) {
    if (priv) {
        kernel_free(priv);
    }
}

static chardev_ops_t serial_ops = {
    .read = serial_chardev_read,
    .write = serial_chardev_write,
    .ioctl = serial_chardev_ioctl,
    .poll = serial_chardev_poll,
    .release = serial_chardev_release
};

// ============================================================================
// INTERNAL HELPERS
// ============================================================================
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

// ============================================================================
// INITIALIZATION
// ============================================================================
void serial_init(void) {
  if (serial_initialized) return;

  const uint16_t ports[] = {COM1_BASE, COM2_BASE};
  const char *names[] = {"COM1", "COM2"};

  for (int i = 0; i < 2; ++i) {
    uint16_t port = ports[i];

    outb(port + UART_IER, 0x00);          // Disable IRQs
    outb(port + UART_LCR, UART_LCR_DLAB); // DLAB=1
    const uint16_t divisor = 1;           // 115200 baud
    outb(port + UART_DLL, divisor & 0xFF);
    outb(port + UART_DLH, (divisor >> 8) & 0xFF);
    outb(port + UART_LCR, UART_LCR_8N1); // 8N1, DLAB=0
    outb(port + UART_FCR, 0x07);         // FIFO on, clear, trigger=1
    outb(port + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    // Enable RX interrupt only (THRE on-demand)
    outb(port + UART_IER, UART_IER_RX);

    // Unmask IRQ in PIC
    serial_unmask_pic(i == 0 ? 4 : 3);

    terminal_printf(&main_terminal, "%s initialized (115200 8N1)\r\n", names[i]);
  }

  serial_initialized = true;
}

// ============================================================================
// IRQ HANDLER
// ============================================================================
void serial_irq_handler_line(int irq) {
  if (irq != 3 && irq != 4) {
    pic_send_eoi(irq);
    return;
  }

  uint16_t port = (irq == 4) ? COM1_BASE : COM2_BASE;

  volatile char *tx_queue = (port == COM1_BASE) ? tx_queue_com1 : tx_queue_com2;
  volatile int *tx_head = (port == COM1_BASE) ? &tx_head_com1 : &tx_head_com2;
  volatile int *tx_tail = (port == COM1_BASE) ? &tx_tail_com1 : &tx_tail_com2;
  volatile bool *tx_busy = (port == COM1_BASE) ? &tx_busy_com1 : &tx_busy_com2;

  volatile char *rx_queue = (port == COM1_BASE) ? rx_queue_com1 : rx_queue_com2;
  volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
  volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;

  // Handle all pending interrupts
  for (;;) {
    uint8_t iir = inb(port + UART_IIR);
    if (iir & 0x01) break; // No pending

    switch (iir & 0x0E) {
    case 0x0C:   // Character Timeout
    case 0x04: { // Received Data Available
      while (serial_can_read(port)) {
        char c = (char)inb(port + UART_DATA);
        
        int next_head = (*rx_head + 1) % SERIAL_RX_QUEUE_SIZE;
        if (next_head != *rx_tail) {
          rx_queue[*rx_head] = c;
          *rx_head = next_head;
        }
      }
    } break;

    case 0x02: { // THRE (Transmitter Holding Register Empty)
      int sent = 0;
      while (*tx_tail != *tx_head && serial_can_write(port)) {
        outb(port + UART_DATA, tx_queue[*tx_tail]);
        *tx_tail = (*tx_tail + 1) % SERIAL_TX_QUEUE_SIZE;
        ++sent;
      }
      
      if (*tx_tail == *tx_head) {
        *tx_busy = false;
        serial_disable_thre(port);
      } else {
        serial_enable_thre(port);
      }
    } break;

    case 0x06: {
      // Line Status error
      (void)inb(port + UART_LSR);
    } break;

    case 0x00: {
      // Modem Status
      (void)inb(port + UART_MSR);
    } break;

    default:
      if (serial_can_read(port)) {
        (void)inb(port + UART_DATA);
      }
      break;
    }
  }

  pic_send_eoi(irq);
}

// ============================================================================
// WRITE OPERATIONS
// ============================================================================
int serial_write_char(uint16_t port, char c) {
  if (port != COM1_BASE && port != COM2_BASE) {
    return -1;
  }

  volatile char *tx_queue = (port == COM1_BASE) ? tx_queue_com1 : tx_queue_com2;
  volatile int *tx_head = (port == COM1_BASE) ? &tx_head_com1 : &tx_head_com2;
  volatile int *tx_tail = (port == COM1_BASE) ? &tx_tail_com1 : &tx_tail_com2;
  volatile bool *tx_busy = (port == COM1_BASE) ? &tx_busy_com1 : &tx_busy_com2;

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  // Polling mode if interrupts disabled or scheduler not running
  if (!(flags & 0x200) || !scheduler.scheduler_enabled) {
    uint32_t timeout = 1000000;
    while (!serial_can_write(port) && timeout > 0) {
      __asm__ __volatile__("pause");
      timeout--;
    }
    if (timeout == 0) {
      __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
      return -1;
    }
    outb(port + UART_DATA, c);
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return 0;
  }

  // Queue mode
  int next_head = (*tx_head + 1) % SERIAL_TX_QUEUE_SIZE;
  while (next_head == *tx_tail) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    task_yield();
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
  }

  if (*tx_busy) {
    tx_queue[*tx_head] = c;
    *tx_head = next_head;
    serial_enable_thre(port);
  } else {
    if (serial_can_write(port)) {
      *tx_busy = true;
      outb(port + UART_DATA, c);
      serial_enable_thre(port);
    } else {
      *tx_busy = true;
      tx_queue[*tx_head] = c;
      *tx_head = next_head;
      serial_enable_thre(port);
    }
  }

  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
  return 0;
}

void serial_write_string(uint16_t port, const char *s) {
  while (*s) {
    if (serial_write_char(port, *s++) != 0)
      break;
  }
}

void serial_printf(uint16_t port, const char *fmt, ...) {
  if (port != COM1_BASE && port != COM2_BASE) {
    return;
  }

  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len < 0 || len >= (int)sizeof(buf)) {
    serial_write_string(port, "serial_printf: Format error\n");
    return;
  }

  serial_write_string(port, buf);
}

// ============================================================================
// READ OPERATIONS
// ============================================================================
char serial_read_char(uint16_t port) {
  while (!serial_can_read(port))
    ;
  return inb(port + UART_DATA);
}

int serial_can_write(uint16_t port) {
  return (inb(port + UART_LSR) & UART_LSR_THRE) != 0;
}

int serial_can_read(uint16_t port) {
  return (inb(port + UART_LSR) & UART_LSR_DR) != 0;
}

int serial_available(uint16_t port) {
  if (port == COM1_BASE) {
    return (rx_head_com1 - rx_tail_com1 + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
  } else if (port == COM2_BASE) {
    return (rx_head_com2 - rx_tail_com2 + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
  }
  return 0;
}

int serial_read_nonblock(uint16_t port) {
  volatile char *rx_queue = (port == COM1_BASE) ? rx_queue_com1 : rx_queue_com2;
  volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
  volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;

  uint32_t flags;
  __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));

  if (*rx_head == *rx_tail) {
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    return -1;
  }

  char c = rx_queue[*rx_tail];
  *rx_tail = (*rx_tail + 1) % SERIAL_RX_QUEUE_SIZE;
  
  __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
  return (unsigned char)c;
}

void serial_unmask_pic(int irq) {
  uint16_t port = (irq < 8) ? 0x21 : 0xA1;
  uint8_t mask = inb(port);
  mask &= ~(1 << (irq % 8));
  outb(port, mask);
}

// ============================================================================
// CREACIÓN DE DISPOSITIVOS CHARDEF PARA SERIAL
// ============================================================================
static chardev_t *create_serial_chardev(uint16_t port, const char *name, uint8_t minor) {
    serial_priv_t *priv = (serial_priv_t *)kernel_malloc(sizeof(serial_priv_t));
    if (!priv) return NULL;
    
    priv->port = port;
    priv->name = name;
    priv->minor = minor;
    
    chardev_t *dev = (chardev_t *)kernel_malloc(sizeof(chardev_t));
    if (!dev) {
        kernel_free(priv);
        return NULL;
    }
    
    memset(dev, 0, sizeof(chardev_t));
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->type = CHARDEV_PORT;
    dev->ops = &serial_ops;
    dev->priv = priv;
    dev->refcount = 1;
    
    return dev;
}

// Función para registrar dispositivos serial como chardev
void serial_devices_init(void) {
    // Crear dispositivos seriales como chardev
    chardev_t *com1_dev = create_serial_chardev(COM1_BASE, "com1", 0);
    chardev_t *com2_dev = create_serial_chardev(COM2_BASE, "com2", 1);
    
    if (com1_dev) {
        chardev_register(com1_dev);
        terminal_printf(&main_terminal, "Registered serial chardev: /dev/com1\r\n");
    }
    
    if (com2_dev) {
        chardev_register(com2_dev);
        terminal_printf(&main_terminal, "Registered serial chardev: /dev/com2\r\n");
    }
}

// ============================================================================
// DEVFS INTEGRATION (usando chardev)
// ============================================================================
static int serial_vfs_read(vfs_node_t *node, uint8_t *buf, uint32_t size, uint32_t offset) {
    chardev_t *dev = (chardev_t *)node->fs_private;
    if (!dev || !dev->ops || !dev->ops->read) {
        return -1;
    }
    return dev->ops->read(buf, size, offset, dev->priv);
}

static int serial_vfs_write(vfs_node_t *node, const uint8_t *buf, uint32_t size, uint32_t offset) {
    chardev_t *dev = (chardev_t *)node->fs_private;
    if (!dev || !dev->ops || !dev->ops->write) {
        return -1;
    }
    return dev->ops->write(buf, size, offset, dev->priv);
}

static void serial_vfs_release(vfs_node_t *node) {
    chardev_t *dev = (chardev_t *)node->fs_private;
    if (dev) {
        dev->refcount--;
        if (dev->refcount == 0) {
            chardev_destroy(dev);
        }
    }
    kernel_free(node);
}

static vnode_ops_t serial_vnode_ops = {
    .read = serial_vfs_read,
    .write = serial_vfs_write,
    .lookup = NULL,
    .create = NULL,
    .mkdir = NULL,
    .readdir = NULL,
    .release = serial_vfs_release,
    .unlink = NULL,
    .symlink = NULL,
    .readlink = NULL,
    .truncate = NULL,
    .getattr = NULL
};

// Create serial device nodes in /dev
int serial_create_devfs_nodes(void) {
    // Crear nodos en /dev
    if (vfs_mknod("/dev/com1", VFS_DEV_CHAR, 4, 0) != VFS_OK) {
        terminal_printf(&main_terminal, "Failed to create /dev/com1\r\n");
        return -1;
    }
    
    if (vfs_mknod("/dev/com2", VFS_DEV_CHAR, 4, 1) != VFS_OK) {
        terminal_printf(&main_terminal, "Failed to create /dev/com2\r\n");
        return -1;
    }
    
    // Configurar operaciones VFS usando chardev
    const char *devices[] = {"/dev/com1", "/dev/com2"};
    
    for (int i = 0; i < 2; i++) {
        const char *rel;
        vfs_superblock_t *sb = find_mount_for_path(devices[i], &rel);
        if (sb) {
            vfs_node_t *node = resolve_path_to_vnode(sb, rel);
            if (node) {
                // Buscar el chardev correspondiente
                chardev_t *dev = chardev_find(i == 0 ? "com1" : "com2");
                if (dev) {
                    node->ops = &serial_vnode_ops;
                    node->fs_private = (void *)dev;
                    dev->refcount++; // Incrementar referencia
                    terminal_printf(&main_terminal, "Configured VFS node for %s with chardev\r\n", devices[i]);
                } else {
                    terminal_printf(&main_terminal, "Warning: chardev not found for %s\r\n", devices[i]);
                }
                node->refcount--;
                if (node->refcount == 0 && node->ops->release) {
                    node->ops->release(node);
                }
            }
        }
    }
    
    terminal_printf(&main_terminal, "Serial VFS nodes created with chardev backend\r\n");
    return 0;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================
int serial_queue_available(uint16_t port) {
    if (port == COM1_BASE) {
        return (rx_head_com1 - rx_tail_com1 + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
    } else if (port == COM2_BASE) {
        return (rx_head_com2 - rx_tail_com2 + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
    }
    return 0;
}

int serial_get_rx_queue_available(uint16_t port) {
    return serial_queue_available(port);
}

char serial_peek_rx_queue(uint16_t port, int index) {
    if (port == COM1_BASE) {
        int avail = serial_get_rx_queue_available(port);
        if (index >= avail) return 0;
        int idx = (rx_tail_com1 + index) % SERIAL_RX_QUEUE_SIZE;
        return rx_queue_com1[idx];
    } else if (port == COM2_BASE) {
        int avail = serial_get_rx_queue_available(port);
        if (index >= avail) return 0;
        int idx = (rx_tail_com2 + index) % SERIAL_RX_QUEUE_SIZE;
        return rx_queue_com2[idx];
    }
    return 0;
}

void serial_clear_rx_queue(uint16_t port) {
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tcli\n\tpop %0" : "=r"(flags));
    
    if (port == COM1_BASE) {
        rx_tail_com1 = rx_head_com1;
    } else if (port == COM2_BASE) {
        rx_tail_com2 = rx_head_com2;
    }
    
    __asm__ __volatile__("push %0\n\tpopf" : : "r"(flags));
    
    serial_printf(COM1_BASE, "Cleared RX queue for COM%u\r\n", 
                  port == COM1_BASE ? 1 : 2);
}

void serial_dump_status(uint16_t port) {
    const char *port_name = (port == COM1_BASE) ? "COM1" : "COM2";
    
    terminal_printf(&main_terminal, "\r\n=== %s Status ===\r\n", port_name);
    
    // Estado de registros
    uint8_t lsr = inb(port + UART_LSR);
    uint8_t msr = inb(port + UART_MSR);
    uint8_t ier = inb(port + UART_IER);
    uint8_t iir = inb(port + UART_IIR);
    
    terminal_printf(&main_terminal, "LSR: 0x%02x (", lsr);
    if (lsr & UART_LSR_DR)    terminal_printf(&main_terminal, "DATA ");
    if (lsr & UART_LSR_THRE)  terminal_printf(&main_terminal, "THRE ");
    terminal_printf(&main_terminal, ")\r\n");
    
    terminal_printf(&main_terminal, "MSR: 0x%02x\r\n", msr);
    terminal_printf(&main_terminal, "IER: 0x%02x (", ier);
    if (ier & UART_IER_RX)    terminal_printf(&main_terminal, "RX ");
    if (ier & UART_IER_THRE)  terminal_printf(&main_terminal, "THRE ");
    terminal_printf(&main_terminal, ")\r\n");
    
    terminal_printf(&main_terminal, "IIR: 0x%02x (pending: %s)\r\n", 
                    iir, (iir & 0x01) ? "no" : "yes");
    
    // Estado de colas
    volatile int *rx_head = (port == COM1_BASE) ? &rx_head_com1 : &rx_head_com2;
    volatile int *rx_tail = (port == COM1_BASE) ? &rx_tail_com1 : &rx_tail_com2;
    volatile int *tx_head = (port == COM1_BASE) ? &tx_head_com1 : &tx_head_com2;
    volatile int *tx_tail = (port == COM1_BASE) ? &tx_tail_com1 : &tx_tail_com2;
    volatile bool *tx_busy = (port == COM1_BASE) ? &tx_busy_com1 : &tx_busy_com2;
    
    int rx_avail = (*rx_head - *rx_tail + SERIAL_RX_QUEUE_SIZE) % SERIAL_RX_QUEUE_SIZE;
    int tx_avail = (*tx_head - *tx_tail + SERIAL_TX_QUEUE_SIZE) % SERIAL_TX_QUEUE_SIZE;
    
    terminal_printf(&main_terminal, "RX Queue: %d/%d bytes (head=%d, tail=%d)\r\n",
                    rx_avail, SERIAL_RX_QUEUE_SIZE, *rx_head, *rx_tail);
    terminal_printf(&main_terminal, "TX Queue: %d/%d bytes (head=%d, tail=%d, busy=%d)\r\n",
                    tx_avail, SERIAL_TX_QUEUE_SIZE, *tx_head, *tx_tail, *tx_busy ? 1 : 0);
    
    terminal_printf(&main_terminal, "=================\r\n\r\n");
}

// ============================================================================
// COMMAND HANDLER
// ============================================================================
void cmd_serial_test(const char *args) {
    if (!args) {
        terminal_printf(&main_terminal, 
            "Usage: serialtest <com1|com2> [text]\r\n"
            "  Ejemplos:\r\n"
            "    serialtest com1              # Muestra estado\r\n"
            "    serialtest com1 hello        # Escribe via serial directo\r\n"
            "    serialtest com1 --clear      # Limpia cola RX\r\n");
        return;
    }
    
    // Parsear manualmente
    char com_port[16] = "";
    char text[256] = "";
    bool clear_queue = false;
    
    // Copiar args para no modificar el original
    char args_copy[512];
    strncpy(args_copy, args, sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';
    
    // Primer token es el puerto
    char *token = strtok(args_copy, " ");
    if (!token) {
        terminal_printf(&main_terminal, "Error: Se requiere puerto (com1 o com2)\r\n");
        return;
    }
    strncpy(com_port, token, sizeof(com_port) - 1);
    
    // Segundo token (si existe)
    token = strtok(NULL, " ");
    if (token) {
        // Verificar si es --clear
        if (strcmp(token, "--clear") == 0) {
            clear_queue = true;
        } else {
            // Es texto normal
            strncpy(text, token, sizeof(text) - 1);
            
            // Concatenar tokens restantes
            token = strtok(NULL, "\0");
            if (token) {
                size_t current_len = strlen(text);
                if (current_len < sizeof(text) - 1) {
                    text[current_len] = ' ';
                    text[current_len + 1] = '\0';
                    strncat(text, token, sizeof(text) - strlen(text) - 1);
                }
            }
        }
    }
    
    // Validar puerto
    uint16_t port;
    const char *dev_path;
    
    if (strcmp(com_port, "com1") == 0) {
        port = COM1_BASE;
        dev_path = "/dev/com1";
    } else if (strcmp(com_port, "com2") == 0) {
        port = COM2_BASE;
        dev_path = "/dev/com2";
    } else {
        terminal_printf(&main_terminal, "Error: Puerto inválido '%s'. Use 'com1' o 'com2'\r\n", com_port);
        return;
    }
    
    // Opción --clear
    if (clear_queue) {
        terminal_printf(&main_terminal, "Limpiando cola RX de %s...\r\n", com_port);
        serial_clear_rx_queue(port);
        return;
    }
    
    // Si no hay texto, mostrar estado
    if (strlen(text) == 0) {
        terminal_printf(&main_terminal, "\r\n=== Estado de %s ===\r\n", com_port);
        
        // Mostrar información de registros
        serial_dump_status(port);
        
        // Mostrar cola RX usando funciones de acceso
        int available = serial_get_rx_queue_available(port);
        terminal_printf(&main_terminal, "Bytes en cola RX: %d\r\n", available);
        
        // Mostrar primeros bytes de la cola (sin consumirlos)
        if (available > 0) {
            terminal_printf(&main_terminal, "Primeros %d bytes en cola: ", 
                          available > 32 ? 32 : available);
            
            for (int i = 0; i < (available > 32 ? 32 : available); i++) {
                char c = serial_peek_rx_queue(port, i);
                if (c >= 32 && c < 127) {
                    terminal_printf(&main_terminal, "%c", c);
                } else if (c == '\r') {
                    terminal_printf(&main_terminal, "\\r");
                } else if (c == '\n') {
                    terminal_printf(&main_terminal, "\\n");
                } else if (c == '\t') {
                    terminal_printf(&main_terminal, "\\t");
                } else {
                    terminal_printf(&main_terminal, "\\x%02x", (unsigned char)c);
                }
            }
            terminal_printf(&main_terminal, "\r\n");
            
            // También mostrar hex dump
            terminal_printf(&main_terminal, "Hex dump: ");
            for (int i = 0; i < (available > 16 ? 16 : available); i++) {
                char c = serial_peek_rx_queue(port, i);
                terminal_printf(&main_terminal, "%02x ", (unsigned char)c);
            }
            terminal_printf(&main_terminal, "\r\n");
        }
        
        return;
    }
    
    // Escribir texto
    terminal_printf(&main_terminal, "Escribiendo a %s: '%s'\r\n", com_port, text);
    
    // Escribir via serial directo
    serial_write_string(port, text);
    serial_write_string(port, "\r\n");
    terminal_printf(&main_terminal, "Texto enviado via serial directo\r\n");
    
    // Opcional: también probar VFS
    terminal_printf(&main_terminal, "\r\nProbando también VFS...\r\n");
    int fd = vfs_open(dev_path, VFS_O_WRONLY);
    if (fd >= 0) {
        const char *vfs_text = "[VFS] ";
        vfs_write(fd, vfs_text, strlen(vfs_text));
        vfs_write(fd, text, strlen(text));
        vfs_write(fd, "\r\n", 2);
        vfs_close(fd);
        terminal_printf(&main_terminal, "✓ También enviado via VFS/devfs\r\n");
    } else {
        terminal_printf(&main_terminal, "✗ No se pudo abrir %s via VFS\r\n", dev_path);
    }
}

// ============================================================================
// DRIVER SYSTEM INTEGRATION
// ============================================================================
static int serial_driver_init(driver_instance_t *drv, void *config) {
  (void)config;
  if (!drv) return -1;
  
  serial_init();
  
  // Registrar dispositivos serial como chardev
  serial_devices_init();
  
  // Crear nodos devfs
  serial_create_devfs_nodes();
  
  return 0;
}

static int serial_driver_start(driver_instance_t *drv) {
  if (!drv) return -1;
  terminal_printf(&main_terminal, "Serial driver: Started\r\n");
  return 0;
}

static int serial_driver_stop(driver_instance_t *drv) {
  if (!drv) return -1;
  return 0;
}

static int serial_driver_cleanup(driver_instance_t *drv) {
  if (!drv) return -1;
  return 0;
}

static int serial_driver_ioctl(driver_instance_t *drv, uint32_t cmd, void *arg) {
  if (!drv) return -1;

  switch (cmd) {
  case 0x1001: { // Write string to COM1
    const char *str = (const char *)arg;
    if (!str) return -1;
    serial_write_string(COM1_BASE, str);
    return 0;
  }
  case 0x1002: { // Write string to COM2
    const char *str = (const char *)arg;
    if (!str) return -1;
    serial_write_string(COM2_BASE, str);
    return 0;
  }
  default:
    return -1;
  }
}

static driver_ops_t serial_driver_ops = {
  .init = serial_driver_init,
  .start = serial_driver_start,
  .stop = serial_driver_stop,
  .cleanup = serial_driver_cleanup,
  .ioctl = serial_driver_ioctl,
  .load_data = NULL
};

static driver_type_info_t serial_driver_type = {
  .type = DRIVER_TYPE_SERIAL,
  .type_name = "serial",
  .version = "1.0.2",
  .priv_data_size = 0,
  .default_ops = &serial_driver_ops,
  .validate_data = NULL,
  .print_info = NULL
};

int serial_driver_register_type(void) {
  return driver_register_type(&serial_driver_type);
}

driver_instance_t *serial_driver_create(const char *name) {
  return driver_create(DRIVER_TYPE_SERIAL, name);
}