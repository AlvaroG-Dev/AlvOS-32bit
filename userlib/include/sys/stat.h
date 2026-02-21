#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>
#include <sys/types.h>

struct stat {
  uint32_t st_dev;
  uint32_t st_ino;
  uint16_t st_mode;
  uint16_t st_nlink;
  uint16_t st_uid;
  uint16_t st_gid;
  uint32_t st_rdev;
  uint32_t st_size;
  uint32_t st_blksize;
  uint32_t st_blocks;
  uint32_t st_atime;
  uint32_t st_mtime;
  uint32_t st_ctime;
};

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_IFIFO 0010000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#endif
