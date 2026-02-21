#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>

// Definiciones de syscalls de AlvOS (deben coincidir con syscalls.h)
int errno;

#define SYSCALL_EXIT 0x00
#define SYSCALL_WRITE 0x01
#define SYSCALL_READ 0x02
#define SYSCALL_GETPID 0x03
#define SYSCALL_YIELD 0x04
#define SYSCALL_SLEEP 0x05
#define SYSCALL_GETTIME 0x06
#define SYSCALL_OPEN 0x07
#define SYSCALL_CLOSE 0x08
#define SYSCALL_LSEEK 0x37
#define SYSCALL_STAT 0x0B
#define SYSCALL_FSTAT 0x26
#define SYSCALL_UNLINK 0x16
#define SYSCALL_RMDIR 0x15
#define SYSCALL_RENAME 0x3B
#define SYSCALL_RTC_GET_DATETIME 0x46
#define SYSCALL_SBRK 0x22
#define SYSCALL_EXECVE 0x0D
#define SYSCALL_FORK 0x0C
#define SYSCALL_WAITPID 0x20
#define SYSCALL_TIMES 0x2E
#define SYSCALL_CHDIR 0x0A
#define SYSCALL_KILL 0x31 // Asumiendo, ajustar si es necesario

// Macro para invocar syscalls
static inline int _syscall3(int num, int a, int b, int c) {
  int ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"(num), "b"(a), "c"(b), "d"(c)
                   : "memory");
  return ret;
}

static inline int _syscall2(int num, int a, int b) {
  int ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"(num), "b"(a), "c"(b)
                   : "memory");
  return ret;
}

static inline int _syscall1(int num, int a) {
  int ret;
  __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
  return ret;
}

static inline int _syscall0(int num) {
  int ret;
  __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
  return ret;
}

// --- Stubs de Newlib ---

void _exit(int status) {
  _syscall1(SYSCALL_EXIT, status);
  while (1)
    ; // No debería llegar aquí
}

int _close(int file) { return _syscall1(SYSCALL_CLOSE, file); }

int _execve(const char *name, char *const argv[], char *const envp[]) {
  return _syscall3(SYSCALL_EXECVE, (int)(uintptr_t)name, (int)(uintptr_t)argv,
                   (int)(uintptr_t)envp);
}

int _fork(void) { return _syscall0(SYSCALL_FORK); }

int _waitpid(int pid, int *status, int options) {
  return _syscall3(SYSCALL_WAITPID, pid, (int)(uintptr_t)status, options);
}

int _chdir(const char *path) {
  return _syscall1(SYSCALL_CHDIR, (int)(uintptr_t)path);
}

int _fstat(int file, struct stat *st) {
  return _syscall2(SYSCALL_FSTAT, file, (int)(uintptr_t)st);
}

int _getpid(void) { return _syscall0(SYSCALL_GETPID); }

int _isatty(int file) {
  // En AlvOS, por ahora, los FDs 0, 1 y 2 son siempre terminal.
  // También podríamos preguntar al kernel en el futuro.
  if (file >= 0 && file <= 2)
    return 1;

  // Realizar un fstat para ver si es un dispositivo de caracteres
  struct stat st;
  if (_fstat(file, &st) == 0) {
    if (S_ISCHR(st.st_mode))
      return 1;
  }

  return 0;
}

int _kill(int pid, int sig) {
  (void)pid;
  (void)sig;
  errno = EINVAL;
  return -1;
}

int _link(char *old, char *new) {
  errno = EMLINK;
  return -1;
}

int _lseek(int file, int ptr, int dir) {
  return _syscall3(SYSCALL_LSEEK, file, ptr, dir);
}

int _open(const char *name, int flags, ...) {
  return _syscall2(SYSCALL_OPEN, (int)(uintptr_t)name, flags);
}

int _read(int file, char *ptr, int len) {
  return _syscall3(SYSCALL_READ, file, (int)(uintptr_t)ptr, len);
}

caddr_t _sbrk(int incr) {
  int res = _syscall1(SYSCALL_SBRK, incr);
  if (res < 0) {
    errno = ENOMEM;
    return (caddr_t)-1;
  }
  return (caddr_t)res;
}

int _stat(const char *file, struct stat *st) {
  return _syscall2(SYSCALL_STAT, (int)(uintptr_t)file, (int)(uintptr_t)st);
}

clock_t _times(struct tms *buf) {
  return (clock_t)_syscall1(SYSCALL_TIMES, (int)(uintptr_t)buf);
}

int _unlink(char *name) {
  return _syscall1(SYSCALL_UNLINK, (int)(uintptr_t)name);
}

int _rmdir(char *name) {
  return _syscall1(SYSCALL_RMDIR, (int)(uintptr_t)name);
}

int _rename(const char *oldpath, const char *newpath) {
  return _syscall2(SYSCALL_RENAME, (int)(uintptr_t)oldpath,
                   (int)(uintptr_t)newpath);
}

int _write(int file, char *ptr, int len) {
  return _syscall3(SYSCALL_WRITE, file, (int)(uintptr_t)ptr, len);
}

// gettimeofday es frecuentemente requerido
int _gettimeofday(struct timeval *tv, void *tz) {
  if (tv) {
    tv->tv_sec = _syscall0(SYSCALL_GETTIME) / 100; // Asumiendo ticks de 10ms
    tv->tv_usec = (_syscall0(SYSCALL_GETTIME) % 100) * 10000;
  }
  return 0;
}

// --- Minimal libc support ---

void exit(int status) { _exit(status); }

int _getdents(const char *path, void *buf, unsigned int buf_size) {
  return _syscall3(0x25, (int)(uintptr_t)path, (int)(uintptr_t)buf,
                   (int)buf_size);
}

int _getcwd(char *buf, unsigned int size) {
  return _syscall2(0x09, (int)(uintptr_t)buf, (int)size);
}

void *malloc(size_t size) {
  void *res = (void *)_sbrk(size);
  if (res == (void *)-1)
    return NULL;
  return res;
}

void free(void *ptr) {
  // Minimal: do nothing
  (void)ptr;
}

int putchar(int c) {
  char ch = (char)c;
  _write(1, &ch, 1);
  return c;
}

int puts(const char *s) {
  int i = 0;
  while (s[i]) {
    putchar(s[i++]);
  }
  putchar('\n');
  return i;
}

static void print_uint(unsigned int n, int base) {
  char buf[32];
  int i = 0;
  if (n == 0) {
    putchar('0');
    return;
  }
  while (n > 0) {
    int rem = n % base;
    buf[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
    n /= base;
  }
  while (i > 0) {
    putchar(buf[--i]);
  }
}

static void print_int(int n) {
  if (n < 0) {
    putchar('-');
    n = -n;
  }
  print_uint((unsigned int)n, 10);
}

int printf(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  int ret = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  if (ret > 0) {
    _write(1, buf, ret);
  }
  return ret;
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
  size_t written = 0;
  for (const char *p = format; *p != '\0' && written < size - 1; p++) {
    if (*p == '%') {
      p++;
      if (*p == 's') {
        const char *s = va_arg(args, const char *);
        if (!s)
          s = "(null)";
        while (*s && written < size - 1) {
          str[written++] = *s++;
        }
      } else if (*p == 'd' || *p == 'u' || *p == 'x' || *p == 'p') {
        char buf[32];
        int i = 0;
        if (*p == 'd') {
          int val = va_arg(args, int);
          if (val == 0)
            buf[i++] = '0';
          else {
            unsigned int uval = (val < 0) ? -val : val;
            if (val < 0 && written < size - 1)
              str[written++] = '-';
            while (uval > 0) {
              buf[i++] = '0' + (uval % 10);
              uval /= 10;
            }
          }
        } else if (*p == 'u') {
          unsigned int uval = va_arg(args, unsigned int);
          if (uval == 0)
            buf[i++] = '0';
          else {
            while (uval > 0) {
              buf[i++] = '0' + (uval % 10);
              uval /= 10;
            }
          }
        } else if (*p == 'x' || *p == 'p') {
          unsigned int uval = (*p == 'x') ? va_arg(args, unsigned int)
                                          : (uintptr_t)va_arg(args, void *);
          if (uval == 0)
            buf[i++] = '0';
          else {
            while (uval > 0) {
              int rem = uval % 16;
              buf[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
              uval /= 16;
            }
          }
          if (*p == 'p') {
            // Hex prefix already handling in loop? No, usually hex doesn't
            // have 0x unless specified. But %p does.
            // Simplified: just the hex.
          }
        }
        for (int j = i - 1; j >= 0 && written < size - 1; j--) {
          str[written++] = buf[j];
        }
      } else if (*p == 'c') {
        str[written++] = (char)va_arg(args, int);
      } else if (*p == '%') {
        str[written++] = '%';
      } else {
        str[written++] = '%';
        if (written < size - 1)
          str[written++] = *p;
      }
    } else {
      str[written++] = *p;
    }
  }
  str[written] = '\0';
  return (int)written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int ret = vsnprintf(str, size, format, args);
  va_end(args);
  return ret;
}

// Implementación de funciones de string.h
void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;
  while (n--)
    *d++ = *s++;
  return dest;
}

size_t strlen(const char *s) {
  size_t len = 0;
  while (s && s[len])
    len++;
  return len;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  char *d = dest;
  while (n && (*d++ = *src++))
    n--;
  while (n--)
    *--d = '\0';
  return dest;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  return NULL;
}

static char *strtok_ptr = NULL;
char *strtok(char *str, const char *delim) {
  if (str)
    strtok_ptr = str;
  if (!strtok_ptr)
    return NULL;

  // Saltar delimitadores iniciales
  while (*strtok_ptr) {
    bool is_delim = false;
    for (const char *d = delim; *d; d++) {
      if (*strtok_ptr == *d) {
        is_delim = true;
        break;
      }
    }
    if (!is_delim)
      break;
    strtok_ptr++;
  }

  if (!*strtok_ptr) {
    strtok_ptr = NULL;
    return NULL;
  }

  char *start = strtok_ptr;
  // Buscar fin del token
  while (*strtok_ptr) {
    bool is_delim = false;
    for (const char *d = delim; *d; d++) {
      if (*strtok_ptr == *d) {
        is_delim = true;
        break;
      }
    }
    if (is_delim) {
      *strtok_ptr++ = '\0';
      return start;
    }
    strtok_ptr++;
  }

  strtok_ptr = NULL;
  return start;
}
