#include <stdio.h>
#include <stdlib.h>

int main() {
  printf("Hola desde Newlib en AlvOS!\n");
  void *m = malloc(1024);
  if (m) {
    printf("Memoria asignada en: %p\n", m);
    free(m);
  }
  return 0;
}