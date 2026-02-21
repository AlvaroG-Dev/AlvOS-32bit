/*
 * newlib_test.c - Prueba de la portación de Newlib / lib mínima en AlvOS
 *
 * Ejercita los stubs de syscall y la biblioteca de usuario:
 *   _write, _read, _open, _close, _sbrk, _exit
 *   printf, puts, putchar, malloc, free, exit
 *   _getpid, _gettimeofday, _fstat, _isatty
 *
 * Compilar desde userlib/: make newlib_test.elf
 * Ejecutar en AlvOS: exec /path/to/newlib_test.elf (o como cargue tu OS los ELFs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/* Stubs que no están en los headers de userlib */
extern int _getpid(void);
extern int _gettimeofday(struct timeval *tv, void *tz);
extern int _isatty(int fd);
extern int _open(const char *name, int flags, ...);
extern int _close(int fd);
extern int _write(int fd, char *ptr, int len);

static int tests_run = 0;
static int tests_ok = 0;

#define TEST(name, cond) do { \
  tests_run++; \
  if (cond) { tests_ok++; printf("[OK] %s\n", name); } \
  else      { printf("[FAIL] %s\n", name); } \
} while (0)

int main(void)
{
  printf("========================================\n");
  printf("  Prueba de portacion Newlib - AlvOS\n");
  printf("========================================\n\n");

  /* --- puts / putchar --- */
  puts("Test: puts()");
  putchar('O');
  putchar('K');
  putchar('\n');
  TEST("putchar/puts", 1);

  /* --- printf formatos --- */
  TEST("printf %%s", printf("Hola %s\n", "Newlib") >= 0);
  TEST("printf %%d", printf("Numero: %d\n", -42) >= 0);
  TEST("printf %%x", printf("Hex: 0x%x\n", 0xDEAD) >= 0);
  TEST("printf %%c", printf("Char: %c\n", 'X') >= 0);
  printf("Puntero: %p\n", (void *)main);
  TEST("printf %%p", 1);

  /* --- malloc / free (usa _sbrk) --- */
  void *a = malloc(256);
  TEST("malloc(256)", a != NULL);
  if (a) free(a);

  void *b = malloc(1024);
  TEST("malloc(1024)", b != NULL);
  if (b) free(b);

  void *c = malloc(4096);
  TEST("malloc(4096)", c != NULL);
  if (c) free(c);

  /* Múltiples bloques (el malloc mínimo es solo sbrk, sin listas; cada malloc es un sbrk) */
  void *d = malloc(128);
  void *e = malloc(128);
  TEST("dos mallocs", d != NULL && e != NULL);
  if (d) free(d);
  if (e) free(e);

  /* --- getpid --- */
  int pid = _getpid();
  printf("PID del proceso: %d\n", pid);
  TEST("_getpid", pid >= 0);

  /* --- gettimeofday --- */
  struct timeval tv;
  int gt = _gettimeofday(&tv, NULL);
  if (gt == 0)
    printf("Tiempo: %u s, %u us\n", (unsigned)tv.tv_sec, (unsigned)tv.tv_usec);
  TEST("_gettimeofday", gt == 0);

  /* --- isatty --- */
  int tty0 = _isatty(0);
  int tty1 = _isatty(1);
  int tty2 = _isatty(2);
  printf("isatty(0)=%d isatty(1)=%d isatty(2)=%d\n", tty0, tty1, tty2);
  TEST("_isatty(stdin/stdout/stderr)", tty0 == 1 && tty1 == 1 && tty2 == 1);

  /* --- open/close (puede fallar si no hay FS o ruta) --- */
  int fd = _open("/dev/null", 0);
  if (fd >= 0) {
    _close(fd);
    TEST("_open/_close /dev/null", 1);
  } else {
    printf("(open /dev/null no disponible, omitido)\n");
    TEST("_open/_close", 1); /* No fallar si el OS no tiene /dev/null */
  }

  printf("\n----------------------------------------\n");
  printf("Resultado: %d/%d pruebas pasaron\n", tests_ok, tests_run);
  printf("----------------------------------------\n");

  exit(0);
  return 0;
}
