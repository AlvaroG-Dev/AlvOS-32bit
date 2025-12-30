// calc-simple.c - Calculadora super simple
void _start(void) {
  char b[64];
  int n1 = 0, n2 = 0, i, k;
  char *p;

  // Leer primer número
  p = b;
  *p++ = 'N';
  *p++ = '1';
  *p++ = ':';
  *p++ = ' ';
  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(b), "d"(4));

  i = 0;
  while (1) {
    __asm__("int $0x80" : "=a"(k) : "a"(14) :);
    if (k == '\n' && i > 0)
      break;
    if (k >= '0' && k <= '9') {
      b[i++] = k;
      __asm__("int $0x80" : : "a"(1), "b"(1), "c"(&k), "d"(1) :);
      n1 = n1 * 10 + (k - '0');
    }
  }

  // Leer segundo número
  p = b;
  *p++ = 'N';
  *p++ = '2';
  *p++ = ':';
  *p++ = ' ';
  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(b), "d"(4));

  i = 0;
  while (1) {
    __asm__("int $0x80" : "=a"(k) : "a"(14) :);
    if (k == '\n' && i > 0)
      break;
    if (k >= '0' && k <= '9') {
      b[i++] = k;
      __asm__("int $0x80" : : "a"(1), "b"(1), "c"(&k), "d"(1) :);
      n2 = n2 * 10 + (k - '0');
    }
  }

  // Resultado
  int sum = n1 + n2;
  p = b;
  *p++ = '\n';

  // Convertir números a string (simple)
  if (n1 == 0)
    *p++ = '0';
  else {
    int d = 1000000000;
    while (n1 / d == 0)
      d /= 10;
    while (d > 0) {
      *p++ = '0' + (n1 / d);
      n1 %= d;
      d /= 10;
    }
  }

  *p++ = ' ';
  *p++ = '+';
  *p++ = ' ';

  if (n2 == 0)
    *p++ = '0';
  else {
    int d = 1000000000;
    while (n2 / d == 0)
      d /= 10;
    while (d > 0) {
      *p++ = '0' + (n2 / d);
      n2 %= d;
      d /= 10;
    }
  }

  *p++ = ' ';
  *p++ = '=';
  *p++ = ' ';

  if (sum == 0)
    *p++ = '0';
  else {
    int d = 1000000000;
    while (sum / d == 0)
      d /= 10;
    while (d > 0) {
      *p++ = '0' + (sum / d);
      sum %= d;
      d /= 10;
    }
  }

  *p++ = '\n';

  __asm__("int $0x80" : : "a"(1), "b"(1), "c"(b), "d"(p - b) :);

  // Salir
  __asm__("int $0x80" : : "a"(0), "b"(0) :);
}