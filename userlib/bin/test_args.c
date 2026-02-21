#include <stdint.h>

extern int _write(int fd, const char *buf, int len);

static int strlen_s(const char *s) {
  int i = 0;
  while (s && s[i])
    i++;
  return i;
}

static void print(const char *s) { _write(1, (char *)s, strlen_s(s)); }

static void print_int(int n) {
  char buf[12];
  int i = 0;
  if (n == 0) {
    print("0");
    return;
  }
  int neg = 0;
  if (n < 0) {
    neg = 1;
    n = -n;
  }
  while (n > 0) {
    buf[i++] = '0' + (n % 10);
    n /= 10;
  }
  if (neg)
    buf[i++] = '-';
  while (i > 0) {
    char c = buf[--i];
    _write(1, &c, 1);
  }
}

int main(int argc, char **argv) {
  print("argc = ");
  print_int(argc);
  print("\n");

  for (int i = 0; i < argc; i++) {
    print("argv[");
    print_int(i);
    print("] = ");
    print(argv[i]);
    print("\n");
  }

  print("Test complete\n");
  return 0;
}
