
#include <stdint.h>
#include <sys/fcntl.h>

// Syscall wrappers
extern int _open(const char *name, int flags, ...);
extern int _read(int fd, char *buf, int len);
extern int _write(int fd, const char *buf, int len);
extern int _close(int fd);

// Helper functions (avoiding stdio)
static int strlen_s(const char *s) {
  int i = 0;
  while (s && s[i])
    i++;
  return i;
}

static void print(const char *s) { _write(1, (char *)s, strlen_s(s)); }

int main(int argc, char **argv) {
  if (argc < 2) {
    print("Usage: cat <file>...\n");
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    int fd = _open(argv[i], O_RDONLY);
    if (fd < 0) {
      print("cat: ");
      print(argv[i]);
      print(": No such file or directory\n");
      continue;
    }

    char buf[1024]; // Increased buffer size
    int n;
    while ((n = _read(fd, buf, sizeof(buf))) > 0) {
      int written = 0;
      while (written < n) {
        int w = _write(1, buf + written, n - written);
        if (w <= 0) {
          print("cat: Write error\n");
          _close(fd);
          return 1;
        }
        written += w;
      }
    }

    if (n < 0) {
      print("cat: ");
      print(argv[i]);
      print(": Read error\n");
    }

    _close(fd);
  }

  return 0;
}
