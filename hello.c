// minimal_ioctl_test.c - ~1500 bytes, usa "com_ports"
#include <stdint.h>

void _start(void) {
  char buffer[128];
  char *p;
  int result;

  // 1. Mostrar mensaje inicial
  p = buffer;
  *p++ = 'I';
  *p++ = 'O';
  *p++ = 'C';
  *p++ = 'T';
  *p++ = 'L';
  *p++ = ' ';
  *p++ = 'T';
  *p++ = 'e';
  *p++ = 's';
  *p++ = 't';
  *p++ = '\n';
  *p++ = '=';
  *p++ = '=';
  *p++ = '=';
  *p++ = '\n';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // 2. Probar Serial Driver (com_ports)
  p = buffer;
  *p++ = '1';
  *p++ = '.';
  *p++ = ' ';
  *p++ = 'S';
  *p++ = 'e';
  *p++ = 'r';
  *p++ = 'i';
  *p++ = 'a';
  *p++ = 'l';
  *p++ = ' ';
  *p++ = '(';
  *p++ = 'c';
  *p++ = 'o';
  *p++ = 'm';
  *p++ = '_';
  *p++ = 'p';
  *p++ = 'o';
  *p++ = 'r';
  *p++ = 't';
  *p++ = 's';
  *p++ = ')';
  *p++ = ':';
  *p++ = ' ';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // Preparar IOCTL para com_ports
  char ioctl_buf[296] = {0}; // 32 + 4 + 4 + 256

  // Nombre: "com_ports" (9 caracteres)
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

  // Mensaje: "Hi!"
  char msg[] = "Hi from user!\r\n";
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

  // Llamar IOCTL
  __asm__("int $0x80" : "=a"(result) : "a"(0x19), "b"((uint32_t)ioctl_buf));

  // Mostrar resultado
  p = buffer;
  if (result == 0) {
    *p++ = 'O';
    *p++ = 'K';
    *p++ = '\n';
  } else {
    *p++ = 'E';
    *p++ = 'r';
    *p++ = 'r';
    *p++ = ':';

    // Convertir error a decimal simple
    int err = -result;
    if (err > 99)
      err = 99;

    if (err >= 10) {
      *p++ = '0' + (err / 10);
      *p++ = '0' + (err % 10);
    } else if (err > 0) {
      *p++ = '0' + err;
    } else {
      *p++ = '0';
    }
    *p++ = '\n';
  }

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // 3. Probar Keyboard Driver
  p = buffer;
  *p++ = '2';
  *p++ = '.';
  *p++ = ' ';
  *p++ = 'K';
  *p++ = 'e';
  *p++ = 'y';
  *p++ = 'b';
  *p++ = 'o';
  *p++ = 'a';
  *p++ = 'r';
  *p++ = 'd';
  *p++ = ':';
  *p++ = ' ';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // Preparar IOCTL para keyboard
  char ioctl_buf2[296] = {0};

  // Nombre: "system-keyboard"
  ioctl_buf2[0] = 's';
  ioctl_buf2[1] = 'y';
  ioctl_buf2[2] = 's';
  ioctl_buf2[3] = 't';
  ioctl_buf2[4] = 'e';
  ioctl_buf2[5] = 'm';
  ioctl_buf2[6] = '-';
  ioctl_buf2[7] = 'k';
  ioctl_buf2[8] = 'e';
  ioctl_buf2[9] = 'y';
  ioctl_buf2[10] = 'b';
  ioctl_buf2[11] = 'o';
  ioctl_buf2[12] = 'a';
  ioctl_buf2[13] = 'r';
  ioctl_buf2[14] = 'd';

  // Comando: 0x1001 (KBD_IOCTL_SET_LAYOUT)
  cmd_ptr = (uint32_t *)(ioctl_buf2 + 32);
  *cmd_ptr = 0x1001;

  // Layout: "US-QWERTY"
  char layout[] = "US-QWERTY";
  int layout_len = 0;
  while (layout[layout_len])
    layout_len++;

  size_ptr = (uint32_t *)(ioctl_buf2 + 36);
  *size_ptr = layout_len;

  // Copiar layout
  arg_ptr = ioctl_buf2 + 40;
  for (int i = 0; i < layout_len; i++) {
    arg_ptr[i] = layout[i];
  }

  // Llamar IOCTL
  __asm__("int $0x80" : "=a"(result) : "a"(0x19), "b"((uint32_t)ioctl_buf2));

  // Mostrar resultado
  p = buffer;
  if (result == 0) {
    *p++ = 'O';
    *p++ = 'K';
    *p++ = '\n';
  } else {
    *p++ = 'E';
    *p++ = 'r';
    *p++ = 'r';
    *p++ = ':';

    int err = -result;
    if (err > 99)
      err = 99;

    if (err >= 10) {
      *p++ = '0' + (err / 10);
      *p++ = '0' + (err % 10);
    } else if (err > 0) {
      *p++ = '0' + err;
    } else {
      *p++ = '0';
    }
    *p++ = '\n';
  }

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // 4. Mensaje final y salir
  p = buffer;
  *p++ = '=';
  *p++ = '=';
  *p++ = '=';
  *p++ = '\n';
  *p++ = 'D';
  *p++ = 'o';
  *p++ = 'n';
  *p++ = 'e';
  *p++ = '!';
  *p++ = '\n';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(buffer), "d"(p - buffer));

  // Salir
  __asm__("int $0x80" : : "a"(0), "b"(0));
}