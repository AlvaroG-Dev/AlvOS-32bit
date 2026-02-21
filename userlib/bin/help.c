#include <stdio.h>

int main(int argc, char **argv) {
  printf("\033[1;36mAlvOS Help System\033[0m\n");
  printf("Welcome to the AlvOS user shell. The following commands are "
         "available:\n\n");
  printf("  \033[1mls\033[0m      - List directory contents\n");
  printf("  \033[1mcat\033[0m     - Display file contents\n");
  printf("  \033[1mecho\033[0m    - Print text to the terminal\n");
  printf("  \033[1mclear\033[0m   - Clear the terminal screen\n");
  printf("  \033[1mguess\033[0m   - Play a number guessing game\n");
  printf("  \033[1mhelp\033[0m    - Show this help message\n\n");
  printf("Use these commands to explore the system.\n");
  return 0;
}
