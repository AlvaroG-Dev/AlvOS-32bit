/*
 * ls.c - Lista archivos de un directorio con colores y mejor formato
 * Programa de usuario para AlvOS
 */

#include <stdbool.h>
#include <stdint.h>

// Syscall wrappers
extern int _getdents(const char *path, void *buf, unsigned int buf_size);
extern int _getcwd(char *buf, unsigned int size);
extern int _write(int fd, char *ptr, int len);

// Node types (must match VFS_NODE_* in vfs.h)
#define NODE_DIR 1
#define NODE_FILE 2
#define NODE_SYMLINK 3
#define NODE_CHRDEV 4
#define NODE_BLKDEV 5
#define NODE_SOCKET 6

// ANSI Colors
#define RESET "\033[0m"
#define BOLD "\033[1m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

static int strlen_s(const char *s) {
  int i = 0;
  while (s && s[i])
    i++;
  return i;
}

static void print(const char *s) { _write(1, (char *)s, strlen_s(s)); }
static void print_char(char c) { _write(1, &c, 1); }

// Simple number to string
static void print_uint(unsigned int n) {
  char buf[12];
  int i = 0;
  if (n == 0) {
    print("0");
    return;
  }
  while (n > 0) {
    buf[i++] = '0' + (n % 10);
    n /= 10;
  }
  while (i > 0) {
    print_char(buf[--i]);
  }
}

// Right-align a number
static void print_uint_padded(unsigned int n, int width) {
  int digits = 0;
  unsigned int tmp = n;
  if (tmp == 0)
    digits = 1;
  while (tmp > 0) {
    digits++;
    tmp /= 10;
  }
  for (int i = 0; i < width - digits; i++)
    print_char(' ');
  print_uint(n);
}

static bool is_executable(const char *name) {
  int len = strlen_s(name);
  if (len < 4)
    return false;
  // Simple check for binaries
  return true;
}

static void print_name_with_color(uint8_t type, const char *name) {
  switch (type) {
  case NODE_DIR:
    print(BLUE BOLD);
    print(name);
    print("/" RESET);
    break;
  case NODE_SYMLINK:
    print(CYAN);
    print(name);
    print("@" RESET);
    break;
  case NODE_CHRDEV:
  case NODE_BLKDEV:
    print(YELLOW);
    print(name);
    print(RESET);
    break;
  case NODE_FILE:
    if (is_executable(name)) {
      print(GREEN BOLD);
    } else {
      print(WHITE);
    }
    print(name);
    print(RESET);
    break;
  default:
    print(name);
    break;
  }
}

static void print_entry(uint8_t type, const char *name, uint32_t size,
                        bool long_mode) {
  if (long_mode) {
    print(" ");
    // Type indicator
    switch (type) {
    case NODE_DIR:
      print(BLUE BOLD "d" RESET);
      break;
    case NODE_FILE:
      print(WHITE "f" RESET);
      break;
    case NODE_SYMLINK:
      print(CYAN "l" RESET);
      break;
    case NODE_CHRDEV:
    case NODE_BLKDEV:
      print(MAGENTA "c" RESET);
      break;
    default:
      print("?");
      break;
    }

    // Permissions stub
    print(" rwxr-xr-x ");

    // Size
    if (type == NODE_FILE) {
      print_uint_padded(size, 8);
    } else {
      print("       -");
    }

    print("  ");
    print_name_with_color(type, name);
    print("\n");
  } else {
    print_name_with_color(type, name);
    print("    "); // 4 spaces separation
  }
}

int main(int argc, char **argv) {
  char path[256];
  path[0] = '\0';
  bool show_all = false;
  bool long_mode = false;

  // Simple flag parsing
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      for (int j = 1; argv[i][j]; j++) {
        if (argv[i][j] == 'a')
          show_all = true;
        else if (argv[i][j] == 'l')
          long_mode = true;
      }
    } else {
      int len = strlen_s(argv[i]);
      if (len >= 256)
        len = 255;
      for (int k = 0; k < len; k++)
        path[k] = argv[i][k];
      path[len] = '\0';
    }
  }

  if (path[0] == '\0') {
    path[0] = '.';
    path[1] = '\0';
  }

  uint8_t buf[8192];
  int result = _getdents(path, buf, sizeof(buf));

  if (result < 0) {
    print(RED "ls: cannot access '" RESET);
    print(path);
    print(RED "': No such file or directory\n" RESET);
    return 1;
  }

  uint32_t count = (result >= 4) ? *(uint32_t *)buf : 0;

  if (long_mode) {
    print(BOLD "Contents of " CYAN);
    print(path);
    print(RESET " (" YELLOW);
    print_uint(count);
    print(RESET " entries):\n");
  }

  // Synthesize "." and ".." if applicable
  bool has_dot = false;
  bool has_dotdot = false;

  uint32_t temp_offset = 4;
  for (uint32_t i = 0; i < count && temp_offset < (uint32_t)result; i++) {
    temp_offset++; // type
    const char *name = (const char *)(buf + temp_offset);
    if (name[0] == '.' && name[1] == '\0')
      has_dot = true;
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0')
      has_dotdot = true;
    temp_offset += strlen_s(name) + 1 + 4; // name + null + size
  }

  if (show_all) {
    if (!has_dot)
      print_entry(NODE_DIR, ".", 0, long_mode);
    if (!has_dotdot)
      print_entry(NODE_DIR, "..", 0, long_mode);
  }

  // Print entries from kernel
  int printed_count = 0;
  uint32_t offset = 4;
  for (uint32_t i = 0; i < count && offset < (uint32_t)result; i++) {
    uint8_t type = buf[offset++];
    const char *name = (const char *)(buf + offset);

    // Skip hidden files if not show_all
    bool is_hidden = (name[0] == '.');

    int name_len = strlen_s(name);
    offset += name_len + 1;

    uint32_t fsize = 0;
    if (offset + 4 <= (uint32_t)result) {
      fsize = *(uint32_t *)(buf + offset);
      offset += 4;
    }

    if (show_all || !is_hidden) {
      print_entry(type, name, fsize, long_mode);
      printed_count++;
    }
  }

  if (printed_count == 0 && !show_all) {
    print(WHITE "(Empty directory)\n" RESET);
  } else if (!long_mode) {
    print("\n");
  }

  return 0;
}
