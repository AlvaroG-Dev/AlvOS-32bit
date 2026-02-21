// stdio.h removed

extern int _write(int fd, const char *buf, int len);

int main(int argc, char **argv) {
  // ANSI escape sequence to clear screen and home cursor
  // \033[2J clears the entire screen
  // \033[H moves cursor to top-left (1,1)
  const char *clear_cmd = "\033[2J\033[H";
  _write(1, clear_cmd, 7);
  return 0;
}
