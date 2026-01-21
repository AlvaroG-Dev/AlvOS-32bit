#ifndef LIB_OS_H
#define LIB_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Syscall numbers from syscalls.h
#define SYS_EXIT 0x00
#define SYS_WRITE 0x01
#define SYS_READ 0x02
#define SYS_GETPID 0x03
#define SYS_YIELD 0x04
#define SYS_SLEEP 0x05
#define SYS_GETTIME 0x06
#define SYS_OPEN 0x07
#define SYS_CLOSE 0x08
#define SYS_GETCWD 0x09
#define SYS_CHDIR 0x0A
#define SYS_STAT 0x0B
#define SYS_FORK 0x0C
#define SYS_EXECVE 0x0D
#define SYS_READKEY 0x0E
#define SYS_KEY_AVAILABLE 0x0F
#define SYS_GETC 0x10
#define SYS_GETS 0x11
#define SYS_KBHIT 0x12
#define SYS_KBFLUSH 0x13
#define SYS_MKDIR 0x14
#define SYS_RMDIR 0x15
#define SYS_UNLINK 0x16
#define SYS_SEEK 0x17
#define SYS_TELL 0x18
#define SYS_IOCTL 0x19
#define SYS_UNAME 0x2F
#define SYS_RTC_GET_DATETIME 0x46

// Keyboard keys (match keyboard.h)
#define KEY_UP (-1)
#define KEY_DOWN (-2)
#define KEY_LEFT (-3)
#define KEY_RIGHT (-4)
#define KEY_HOME (-5)
#define KEY_END (-6)
#define KEY_PGUP (-7)
#define KEY_PGDOWN (-8)
#define KEY_INSERT (-9)
#define KEY_DELETE (-10)

// ANSI codes
#define ANSI_CLEAR "\x1b[2J\x1b[H"
#define ANSI_RESET "\x1b[0m"
#define ANSI_REVERSE "\x1b[7m"

// VFS flags
#define O_RDONLY 0x1
#define O_WRONLY 0x2
#define O_RDWR 0x4
#define O_CREAT 0x8
#define O_TRUNC 0x10

typedef struct {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint32_t year;
} rtc_time_t;

// Syscall wrappers
static inline int syscall0(int num) {
  int res;
  __asm__ volatile("int $0x80" : "=a"(res) : "a"(num));
  return res;
}

static inline int syscall1(int num, uint32_t a) {
  int res;
  __asm__ volatile("int $0x80" : "=a"(res) : "a"(num), "b"(a));
  return res;
}

static inline int syscall2(int num, uint32_t a, uint32_t b) {
  int res;
  __asm__ volatile("int $0x80" : "=a"(res) : "a"(num), "b"(a), "c"(b));
  return res;
}

static inline int syscall3(int num, uint32_t a, uint32_t b, uint32_t c) {
  int res;
  __asm__ volatile("int $0x80" : "=a"(res) : "a"(num), "b"(a), "c"(b), "d"(c));
  return res;
}

// User-space API
static inline void exit(int code) {
  syscall1(SYS_EXIT, code);
  while (1)
    ;
}
static inline int write(int fd, const void *buf, size_t count) {
  return syscall3(SYS_WRITE, fd, (uintptr_t)buf, count);
}
static inline int read(int fd, void *buf, size_t count) {
  return syscall3(SYS_READ, fd, (uintptr_t)buf, count);
}
static inline int open(const char *path, int flags) {
  return syscall2(SYS_OPEN, (uintptr_t)path, flags);
}
static inline int close(int fd) { return syscall1(SYS_CLOSE, fd); }
static inline void sleep(uint32_t ms) { syscall1(SYS_SLEEP, ms); }
static inline int readkey() { return syscall0(SYS_READKEY); }
static inline bool kbhit() { return syscall0(SYS_KBHIT) > 0; }
static inline int get_rtc(rtc_time_t *time) {
  return syscall1(SYS_RTC_GET_DATETIME, (uintptr_t)time);
}

// String utils (basic)
static inline size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

static inline void print(const char *s) { write(1, s, strlen(s)); }

static inline void putchar(char c) { write(1, &c, 1); }

#endif
