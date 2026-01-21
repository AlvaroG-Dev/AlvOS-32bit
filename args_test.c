#include "lib_os.h"
#include <stdint.h>

static void print_ptr(uint32_t p) {
  char buf[12];
  buf[0] = '0';
  buf[1] = 'x';
  buf[10] = '\0';
  for (int i = 7; i >= 0; i--) {
    uint8_t nibble = p & 0xF;
    buf[i + 2] = (nibble < 10) ? (nibble + '0') : (nibble - 10 + 'A');
    p >>= 4;
  }
  print(buf);
}

static void print_num(int n) {
  if (n == 0) {
    print("0");
    return;
  }
  if (n < 0) {
    print("-");
    n = -n;
  }
  char buf[12];
  int i = 10;
  buf[11] = '\0';
  while (n > 0 && i >= 0) {
    buf[i--] = (n % 10) + '0';
    n /= 10;
  }
  print(&buf[i + 1]);
}

int main_entry(int argc, char *argv[]) {
  uint32_t esp;
  __asm__ volatile("mov %%esp, %0" : "=r"(esp));

  print("--- AlvOS Debug Mode ---\n");
  print("ESP in main: ");
  print_ptr(esp);
  print("\n");

  print("argc value: ");
  print_num(argc);
  print("\n");

  if (argc > 0) {
    for (int i = 0; i < argc; i++) {
      print("argv[");
      print_num(i);
      print("]: ");
      if (argv[i])
        print(argv[i]);
      else
        print("(null)");
      print("\n");
    }
  }

  exit(0);
  return 0;
}
