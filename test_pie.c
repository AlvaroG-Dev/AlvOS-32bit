#include <stdint.h>

void _start() {
  const char *msg = "Soy un programa PIE y cargo donde el kernel quiera!\n";

  // Syscall Write (FD 1, buf, len)
  __asm__ volatile("int $0x80" ::"a"(0x01), "b"(1), "c"(msg), "d"(52));

  // Syscall Exit (0)
  __asm__ volatile("int $0x80" ::"a"(0x00), "b"(0));
}