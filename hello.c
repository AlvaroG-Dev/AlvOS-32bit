#include <stdint.h>

// minimal-serial-ioctl.c - Lo más mínimo posible
void _start(void) {
  char buffer[128];
  char *p;
  int result;

  // Estructura para IOCTL
  // [32 nombre][4 cmd][4 arg_size][256 arg]
  char ioctl_buf[296] = {0}; // 32+4+4+256

  // Nombre del driver: "serial0"
  ioctl_buf[0] = 'c';
  ioctl_buf[1] = 'o';
  ioctl_buf[2] = 'm';
  ioctl_buf[3] = '_';
  ioctl_buf[4] = 'p';
  ioctl_buf[5] = 'o';
  ioctl_buf[6] = 'r';
  ioctl_buf[7] = 't';
  ioctl_buf[8] = 's';

  // Comando: 0x1001 (escribir a COM1)
  uint32_t *cmd_ptr = (uint32_t *)(ioctl_buf + 32);
  *cmd_ptr = 0x1001;

  // Tamaño del mensaje
  char msg[] = "User IOCTL test!\r\n";
  int msg_len = 0;
  while (msg[msg_len])
    msg_len++;

  uint32_t *size_ptr = (uint32_t *)(ioctl_buf + 36);
  *size_ptr = msg_len;

  // Copiar mensaje
  char *arg_ptr = ioctl_buf + 40;
  for (int i = 0; i < msg_len; i++) {
    arg_ptr[i] = msg[i];
  }

  // Mostrar mensaje inicial
  p = buffer;
  *p++ = 'S';
  *p++ = 'e';
  *p++ = 'n';
  *p++ = 'd';
  *p++ = 'i';
  *p++ = 'n';
  *p++ = 'g';
  *p++ = ' ';
  *p++ = 'I';
  *p++ = 'O';
  *p++ = 'C';
  *p++ = 'T';
  *p++ = 'L';
  *p++ = '.';
  *p++ = '.';
  *p++ = '.';
  *p++ = '\n';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // Llamar a IOCTL
  __asm__("int $0x80" : "=a"(result) : "a"(0x19), "b"((uint32_t)ioctl_buf));

  // Mostrar resultado
  p = buffer;

  if (result == 0) {
    *p++ = 'S';
    *p++ = 'u';
    *p++ = 'c';
    *p++ = 'c';
    *p++ = 'e';
    *p++ = 's';
    *p++ = 's';
    *p++ = '!';
    *p++ = '\n';
  } else {
    *p++ = 'E';
    *p++ = 'r';
    *p++ = 'r';
    *p++ = 'o';
    *p++ = 'r';
    *p++ = ':';
    *p++ = ' ';

    // Convertir error
    char num[16];
    char *n = num + 15;
    *n = '\0';
    int err = -result;
    if (err == 0)
      err = result;

    do {
      *--n = '0' + (err % 10);
      err /= 10;
    } while (err > 0);

    while (*n)
      *p++ = *n++;
    *p++ = '\n';
  }

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // Salir
  __asm__("int $0x80" : : "a"(0), "b"(0));
}