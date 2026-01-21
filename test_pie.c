#include "lib_os.h"
#include <stdint.h>

int main_entry(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *msg = "Soy un programa PIE y cargo donde el kernel quiera!\n";
  print(msg);
  exit(0);
  return 0;
}