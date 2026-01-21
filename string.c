#include "string.h"

static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

void __udivmoddi4(uint64_t dividend, uint64_t divisor, uint64_t *quotient,
                  uint64_t *remainder) {
  uint64_t quot = 0;
  uint64_t rem = dividend;

  if (divisor == 0) {
    // Handle division by zero (you might want to panic or return an error)
    if (quotient)
      *quotient = 0;
    if (remainder)
      *remainder = 0;
    return;
  }

  // Simple long division algorithm
  while (rem >= divisor) {
    rem -= divisor;
    quot++;
  }

  if (quotient)
    *quotient = quot;
  if (remainder)
    *remainder = rem;
}

char *kitoa(int value, char *str, int base) {
  if (base < 2 || base > 36) {
    *str = '\0';
    return str;
  }

  char *ptr = str;
  char *start = ptr;

  if (value < 0 && base == 10) {
    *ptr++ = '-';
    start = ptr;
    value = -value;
  }

  do {
    *ptr++ = digits[value % base];
    value /= base;
  } while (value);

  *ptr = '\0';

  ptr--;
  while (start < ptr) {
    char tmp = *start;
    *start++ = *ptr;
    *ptr-- = tmp;
  }

  return str;
}

int int_itoa(int value, char *str, int base) {
  if (base < 2 || base > 36) {
    *str = '\0';
    return 0;
  }

  char *ptr = str;
  char *start = ptr;
  int length = 0;

  if (value < 0 && base == 10) {
    *ptr++ = '-';
    start = ptr;
    value = -value;
    length++;
  }

  do {
    *ptr++ = digits[value % base];
    value /= base;
    length++;
  } while (value);

  *ptr = '\0';

  // Invertir la cadena
  ptr--;
  while (start < ptr) {
    char tmp = *start;
    *start++ = *ptr;
    *ptr-- = tmp;
  }

  return length; // Devuelve la longitud de la cadena
}

// Convierte string a entero
int atoi(const char *str) {
  int result = 0;
  int sign = 1;

  if (*str == '-') {
    sign = -1;
    str++;
  }

  while (*str >= '0' && *str <= '9') {
    result = result * 10 + (*str - '0');
    str++;
  }

  return sign * result;
}

// Convierte string a unsigned long
unsigned long strtoul(const char *str, char **endptr, int base) {
  unsigned long result = 0;

  while (*str) {
    int digit;
    if (*str >= '0' && *str <= '9')
      digit = *str - '0';
    else if (*str >= 'a' && *str <= 'f')
      digit = *str - 'a' + 10;
    else if (*str >= 'A' && *str <= 'F')
      digit = *str - 'A' + 10;
    else
      break;

    if (digit >= base)
      break;

    result = result * base + digit;
    str++;
  }

  if (endptr)
    *endptr = (char *)str;
  return result;
}

int memcmp(const void *ptr1, const void *ptr2, size_t num) {
  unsigned char *p1 = (unsigned char *)ptr1;
  unsigned char *p2 = (unsigned char *)ptr2;

  for (size_t i = 0; i < num; i++) {
    if (p1[i] != p2[i]) {
      return p1[i] - p2[i];
    }
  }
  return 0;
}

long strtol(const char *nptr, char **endptr, int base) {
  long result = 0;
  int sign = 1;

  while (*nptr == ' ' || *nptr == '\t')
    nptr++; // Saltar espacios

  if (*nptr == '-') {
    sign = -1;
    nptr++;
  } else if (*nptr == '+') {
    nptr++;
  }

  while (*nptr) {
    int digit;
    if (*nptr >= '0' && *nptr <= '9') {
      digit = *nptr - '0';
    } else if (*nptr >= 'A' && *nptr <= 'Z') {
      digit = *nptr - 'A' + 10;
    } else if (*nptr >= 'a' && *nptr <= 'z') {
      digit = *nptr - 'a' + 10;
    } else {
      break;
    }

    if (digit >= base)
      break;

    result = result * base + digit;
    nptr++;
  }

  if (endptr) {
    *endptr = (char *)nptr;
  }

  return sign * result;
}

// Versión para unsigned
char *uitoa(uint32_t value, char *str, int base) {
  if (base < 2 || base > 36) {
    *str = '\0';
    return str;
  }

  char *ptr = str;
  char *start = ptr;

  do {
    *ptr++ = digits[value % base];
    value /= base;
  } while (value);

  *ptr = '\0';

  // Invertir
  ptr--;
  while (start < ptr) {
    char tmp = *start;
    *start++ = *ptr;
    *ptr-- = tmp;
  }

  return str;
}

void ulltoa(unsigned long long value, char *buf, int base) {
  char *p = buf;
  char *p1, *p2;
  char tmp;
  int len = 0;

  // Generar dígitos en orden inverso
  do {
    int digit = value % base;
    *p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
    value /= base;
    len++;
  } while (value > 0);

  *p = '\0';

  // Invertir la cadena
  p1 = buf;
  p2 = p - 1;
  while (p1 < p2) {
    tmp = *p1;
    *p1++ = *p2;
    *p2-- = tmp;
  }
}

void to_hex(uint64_t val, char *buf) {
  // Asegurarse de que el buffer sea lo suficientemente grande
  const char *hex = "0123456789ABCDEF";
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    int shift = (15 - i) * 4;
    buf[i + 2] = hex[(val >> shift) & 0xF];
  }
  buf[18] = '\0';
}

void to_decimal(uint32_t val, char *buf) {
  // Asegurarse de que el buffer sea lo suficientemente grande
  char temp[11];
  int pos = 0;
  if (val == 0) {
    temp[pos++] = '0';
  } else {
    while (val > 0) {
      temp[pos++] = '0' + (val % 10);
      val /= 10;
    }
  }
  for (int i = 0; i < pos; i++) {
    buf[i] = temp[pos - i - 1];
  }
  buf[pos] = '\0';
}

// Implementación mínima de sprintf.
int sprintf(char *buf, const char *fmt, ...) {
  // Asegurarse de que el buffer no se desborde
  va_list args;
  va_start(args, fmt);
  char *str = buf;

  for (; *fmt != '\0'; fmt++) {
    if (*fmt != '%') {
      *str++ = *fmt;
      continue;
    }
    fmt++; // saltar '%'

    if (*fmt == 'd') {
      int num = va_arg(args, int);
      char temp[16];
      int i = 0;
      if (num == 0) {
        temp[i++] = '0';
      } else {
        if (num < 0) {
          *str++ = '-';
          num = -num;
        }
        while (num != 0) {
          temp[i++] = '0' + (num % 10);
          num /= 10;
        }
      }
      reverse(temp, i);
      for (int j = 0; j < i; j++) {
        *str++ = temp[j];
      }
    } else if (*fmt == 's') {
      char *s = va_arg(args, char *);
      while (*s) {
        *str++ = *s++;
      }
    } else if (*fmt == '%') {
      *str++ = '%';
    } else {
      // Si se encuentra un especificador no soportado, se copia literalmente.
      *str++ = *fmt;
    }
  }

  *str = '\0';
  va_end(args);
  return str - buf;
}

size_t strlen(const char *str) {
  size_t len = 0;
  while (str[len] != '\0') {
    len++;
  }
  return len;
}

uint64_t strtoull(const char *str, char **endptr, int base) {
  uint64_t result = 0;
  while (*str) {
    char c = *str;
    if (c >= '0' && c <= '9')
      c -= '0';
    else if (c >= 'A' && c <= 'F')
      c = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      c = c - 'a' + 10;
    else
      break;
    if (c >= base)
      break;
    result = result * base + c;
    str++;
  }
  if (endptr)
    *endptr = (char *)str;
  return result;
}

// Función auxiliar para copiar string con límite
size_t kstrlcpy(char *dst, const char *src, size_t size) {
  size_t i = 0;
  if (size > 0) {
    while (i < size - 1 && src[i]) {
      dst[i] = src[i];
      i++;
    }
    dst[i] = '\0';
  }
  while (src[i])
    i++;
  return i;
}

char *strcat(char *dest, const char *src) {
  char *ret = dest;
  while (*dest)
    dest++;
  while ((*dest++ = *src++))
    ;
  return ret;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

// Implementación mínima de strncmp
int strncmp(const char *s1, const char *s2, unsigned int n) {
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

size_t strnlen(const char *s, size_t maxlen) {
  size_t i;
  for (i = 0; i < maxlen && s[i] != '\0'; i++)
    ;
  return i;
}

int isspace(int c) {
  return (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
          c == '\r');
}

char *strcpy(char *dest, const char *src) {
  char *original_dest = dest;
  while ((*dest++ = *src++))
    ;
  return original_dest;
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
  if (size == 0)
    return 0;

  size_t written = 0;
  char num_buffer[32];
  const char *s;
  int num;
  unsigned int unum;
  char c;
  double dnum;

  while (*format && written < size - 1) {
    if (*format != '%') {
      str[written++] = *format++;
      continue;
    }

    format++; // Saltar '%'

    // Procesar flags opcionales
    int left_align = 0;
    int zero_pad = 0;
    int width = 0;
    int precision = -1;

    // Flags
    while (1) {
      if (*format == '-') {
        left_align = 1;
        format++;
      } else if (*format == '0') {
        zero_pad = 1;
        format++;
      } else {
        break;
      }
    }

    // Width
    while (*format >= '0' && *format <= '9') {
      width = width * 10 + (*format - '0');
      format++;
    }

    // Precision
    if (*format == '.') {
      format++;
      precision = 0;
      while (*format >= '0' && *format <= '9') {
        precision = precision * 10 + (*format - '0');
        format++;
      }
    }

    // Longitud (ignoramos 'l' y 'll' en 32 bits)
    if (*format == 'l') {
      format++;
      if (*format == 'l') {
        format++;
      }
    }

    // Especificador
    switch (*format) {
    case 'c':
      c = (char)va_arg(args, int);
      if (!left_align && width > 1) {
        for (int i = 0; i < width - 1 && written < size - 1; i++) {
          str[written++] = zero_pad ? '0' : ' ';
        }
      }
      if (written < size - 1)
        str[written++] = c;
      if (left_align && width > 1) {
        for (int i = 0; i < width - 1 && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }
      break;

    case 's':
      s = va_arg(args, const char *);
      if (!s)
        s = "(null)";
      int len = strlen(s);

      // Aplicar precisión para %.11s
      if (precision >= 0 && len > precision) {
        len = precision;
      }

      int pad = width > len ? width - len : 0;

      if (!left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }

      for (int i = 0; i < len && written < size - 1; i++) {
        str[written++] = s[i];
      }

      if (left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }
      break;

    case 'd':
    case 'i':
      num = va_arg(args, int);
      kitoa(num, num_buffer, 10);
      len = strlen(num_buffer);
      pad = width > len ? width - len : 0;

      if (!left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = zero_pad ? '0' : ' ';
        }
      }

      for (int i = 0; i < len && written < size - 1; i++) {
        str[written++] = num_buffer[i];
      }

      if (left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }
      break;

    case 'u':
      if (*(format - 1) == 'l' && *(format - 2) == 'l') { // %llu
        unsigned long long ullnum = va_arg(args, unsigned long long);
        ulltoa(ullnum, num_buffer, 10); // Necesitarás implementar ulltoa
        len = strlen(num_buffer);
        pad = width > len ? width - len : 0;

        if (!left_align) {
          for (int i = 0; i < pad && written < size - 1; i++) {
            str[written++] = zero_pad ? '0' : ' ';
          }
        }

        for (int i = 0; i < len && written < size - 1; i++) {
          str[written++] = num_buffer[i];
        }

        if (left_align) {
          for (int i = 0; i < pad && written < size - 1; i++) {
            str[written++] = ' ';
          }
        }
      } else { // %u
        unum = va_arg(args, unsigned int);
        uitoa(unum, num_buffer, 10);
        len = strlen(num_buffer);
        pad = width > len ? width - len : 0;

        if (!left_align) {
          for (int i = 0; i < pad && written < size - 1; i++) {
            str[written++] = zero_pad ? '0' : ' ';
          }
        }

        for (int i = 0; i < len && written < size - 1; i++) {
          str[written++] = num_buffer[i];
        }

        if (left_align) {
          for (int i = 0; i < pad && written < size - 1; i++) {
            str[written++] = ' ';
          }
        }
      }
      break;

    case 'x':
    case 'X':
      unum = va_arg(args, unsigned int);
      uitoa(unum, num_buffer, 16);
      len = strlen(num_buffer);
      pad = width > len ? width - len : 0;

      if (!left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = zero_pad ? '0' : ' ';
        }
      }

      for (int i = 0; i < len && written < size - 1; i++) {
        str[written++] = num_buffer[i];
      }

      if (left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }
      break;

    case 'p':
      unum = (unsigned int)(uintptr_t)va_arg(args, void *);
      str[written++] = '0';
      if (written < size - 1)
        str[written++] = 'x';
      uitoa(unum, num_buffer, 16);
      len = strlen(num_buffer);
      pad = width > len + 2 ? width - len - 2 : 0;

      if (!left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = zero_pad ? '0' : ' ';
        }
      }

      for (int i = 0; i < len && written < size - 1; i++) {
        str[written++] = num_buffer[i];
      }

      if (left_align) {
        for (int i = 0; i < pad && written < size - 1; i++) {
          str[written++] = ' ';
        }
      }
      break;

    case '%':
      if (written < size - 1)
        str[written++] = '%';
      break;

    default:
      // Especificador desconocido - copiar literalmente
      if (written < size - 1)
        str[written++] = '%';
      if (written < size - 1)
        str[written++] = *format;
      break;

    case 'f': {
      double dnum = va_arg(args, double);
      int int_part = (int)dnum;
      double fractional = dnum - int_part;
      if (fractional < 0)
        fractional = -fractional;

      int frac_part = (int)(fractional * 100 + 0.5); // Redondeo a 2 decimales

      // Parte entera
      char temp[32];
      int len = int_itoa(int_part, temp, 10);
      for (int i = 0; i < len && written < size - 1; i++) {
        str[written++] = temp[i];
      }

      // Punto decimal
      if (written < size - 1)
        str[written++] = '.';

      // Parte fraccional (2 dígitos)
      if (frac_part < 10 && written < size - 1) {
        str[written++] = '0';
      }
      len = int_itoa(frac_part, temp, 10);
      for (int i = 0; i < 2 && written < size - 1 && i < len; i++) {
        str[written++] = temp[i];
      }
      break;
    }
    }

    if (*format)
      format++;
  }

  if (written < size) {
    str[written] = '\0';
  } else {
    str[size - 1] = '\0';
  }

  return written;
}

char *strrchr(const char *str, int c) {
  char *last = NULL;

  // Recorremos la cadena hasta encontrar el carácter nulo '\0'
  while (*str != '\0') {
    if (*str == c) {
      last = (char *)str; // Guardamos la posición de la última coincidencia
    }
    str++;
  }

  // Verificamos si el carácter también está al final de la cadena
  if (c == '\0') {
    return (char *)str;
  }

  return last; // Devuelve la última aparición del carácter
}

// Implementación de strtok_r para entornos bare-metal
char *strtok_r(char *str, const char *delim, char **saveptr) {
  char *token;

  // Si str es NULL, se continúa desde el último token guardado
  if (str == NULL)
    str = *saveptr;

  // Saltar caracteres delimitadores al inicio
  while (*str && strchr(delim, *str))
    str++;

  if (*str == '\0') {
    *saveptr = str;
    return NULL;
  }

  token = str;
  // Avanza hasta encontrar un delimitador o el final de la cadena
  while (*str && !strchr(delim, *str))
    str++;

  if (*str) {
    *str = '\0';        // Termina el token actual
    *saveptr = str + 1; // Guarda el puntero para la próxima llamada
  } else {
    *saveptr = str;
  }
  return token;
}

int snprintf(char *str, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int ret = vsnprintf(str, size, format, args);
  va_end(args);
  return ret;
}

static void reverse(char *s, int len) {
  int i = 0, j = len - 1;
  char temp;
  while (i < j) {
    temp = s[i];
    s[i] = s[j];
    s[j] = temp;
    i++;
    j--;
  }
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
  if (d == 0) {
    // Manejo de división por cero: aquí podrías invocar una rutina
    // de pánico o simplemente entrar en un bucle infinito.
    while (1)
      ;
  }

  unsigned long long quotient = 0;
  int shift = 0;

  // Normaliza el divisor desplazándolo a la izquierda hasta que su bit más
  // significativo esté en la posición 63 o hasta que sea mayor que n.
  while (d <= n && (d & 0x8000000000000000ULL) == 0) {
    d <<= 1;
    shift++;
  }

  // Realiza la división bit a bit.
  while (shift >= 0) {
    if (n >= d) {
      n -= d;
      quotient |= (1ULL << shift);
    }
    d >>= 1;
    shift--;
  }

  return quotient;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

size_t strcspn(const char *str1, const char *str2) {
  const char *s1;
  const char *s2;

  // Recorremos la cadena `str1`
  for (s1 = str1; *s1 != '\0'; ++s1) {
    // Comprobamos si el carácter actual de `str1` está en `str2`
    for (s2 = str2; *s2 != '\0'; ++s2) {
      if (*s1 == *s2) {
        return s1 - str1; // Si encontramos una coincidencia, retornamos la
                          // longitud hasta el carácter coincidente
      }
    }
  }

  // Si no se encuentra ninguna coincidencia, retornamos la longitud completa de
  // `str1`
  return s1 - str1;
}

char *trim_whitespace(char *str) {
  char *start = str;
  char *end;

  // Recortar espacios iniciales
  while (isspace((unsigned char)*start))
    start++;

  // Si todo son espacios
  if (*start == '\0') {
    *str = '\0';
    return str;
  }

  // Recortar espacios finales
  end = start + strlen(start) - 1;
  while (end > start && isspace((unsigned char)*end))
    end--;

  // Terminar la cadena
  *(end + 1) = '\0';

  return start;
}

// strncat seguro para entornos bare-metal
char *strncat(char *dest, const char *src, size_t n) {
  char *ptr = dest;

  // Mover ptr al final de dest
  while (*ptr != '\0') {
    ptr++;
  }

  // Copiar src a dest hasta n caracteres o fin de src
  while (n-- && *src != '\0') {
    *ptr++ = *src++;
  }

  *ptr = '\0'; // Agregar terminador nulo

  return dest;
}

char *strtok(char *str, const char *delim) {
  static char *next_token = NULL;

  if (str != NULL) {
    next_token = str;
  }

  if (next_token == NULL || *next_token == '\0') {
    return NULL;
  }

  // Saltar delimitadores al inicio
  char *start = next_token;
  while (*start && strchr(delim, *start)) {
    start++;
  }

  if (*start == '\0') {
    next_token = NULL;
    return NULL;
  }

  // Buscar el siguiente delimitador
  char *end = start;
  while (*end && !strchr(delim, *end)) {
    end++;
  }

  if (*end) {
    *end = '\0';
    next_token = end + 1;
  } else {
    next_token = NULL;
  }

  return start;
}

char *strchr(const char *str, int c) {
  while (*str) {
    if (*str == (char)c) {
      return (char *)str;
    }
    str++;
  }
  return NULL;
}

void u64_to_str(uint64_t value, char *buffer) {
  char temp[21];
  int i = 0;
  do {
    temp[i++] = '0' + (value % 10);
    value /= 10;
  } while (value && i < 20);
  int j = 0;
  while (i > 0) {
    buffer[j++] = temp[--i];
  }
  buffer[j] = '\0';
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;

  for (; *haystack; haystack++) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && *h == *n) {
      h++;
      n++;
    }
    if (!*n)
      return (char *)haystack;
  }
  return NULL;
}

int toupper(int c) {
  if (c >= 'a' && c <= 'z') {
    return c - ('a' - 'A');
  }
  return c;
}

void strupper(char *s) {
  while (*s) {
    *s = toupper(*s);
    s++;
  }
}

/*
 * Función auxiliar para convertir cadena a entero con base arbitraria.
 */
static int strtoi_base(const char *str, char **endptr, int base) {
  int value = 0;
  int sign = 1;

  if (*str == '-') {
    sign = -1;
    str++;
  } else if (*str == '+') {
    str++;
  }

  while (*str) {
    int digit;
    if (*str >= '0' && *str <= '9')
      digit = *str - '0';
    else if (*str >= 'a' && *str <= 'z')
      digit = 10 + (*str - 'a');
    else if (*str >= 'A' && *str <= 'Z')
      digit = 10 + (*str - 'A');
    else
      break;

    if (digit >= base)
      break;

    value = value * base + digit;
    str++;
  }

  if (endptr)
    *endptr = (char *)str;
  return value * sign;
}

/*
 * vsscanf: parsea una cadena usando formato tipo scanf.
 */
int vsscanf(const char *str, const char *fmt, va_list ap) {
  int assigned = 0;

  while (*fmt && *str) {
    if (*fmt == '%') {
      fmt++;

      int width = 0;
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }

      if (*fmt == 's') {
        char *out = va_arg(ap, char *);
        int i = 0;
        while (*str && !isspace((unsigned char)*str) &&
               (width == 0 || i < width)) {
          out[i++] = *str++;
        }
        out[i] = '\0';
        assigned++;
        fmt++;
      } else if (*fmt == 'd') {
        char *end;
        int val = strtoi_base(str, &end, 10);
        int *out = va_arg(ap, int *);
        *out = val;
        if (end == str)
          break; // error
        str = end;
        assigned++;
        fmt++;
      } else if (*fmt == 'x' || *fmt == 'X') {
        char *end;
        int val = strtoi_base(str, &end, 16);
        int *out = va_arg(ap, int *);
        *out = val;
        if (end == str)
          break; // error
        str = end;
        assigned++;
        fmt++;
      } else if (*fmt == 'c') {
        char *out = va_arg(ap, char *);
        *out = *str++;
        assigned++;
        fmt++;
      } else {
        // especificador no soportado
        return assigned;
      }
    } else if (isspace((unsigned char)*fmt)) {
      while (isspace((unsigned char)*str))
        str++;
      fmt++;
    } else {
      if (*fmt != *str)
        break;
      fmt++;
      str++;
    }
  }

  return assigned;
}

/*
 * sscanf: interfaz pública
 */
int sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}
int tolower(int c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

int strcasecmp(const char *s1, const char *s2) {
  while (*s1 &&
         (tolower(*(unsigned char *)s1) == tolower(*(unsigned char *)s2))) {
    s1++;
    s2++;
  }
  return tolower(*(unsigned char *)s1) - tolower(*(unsigned char *)s2);
}
