#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <stdint.h>
#include <sys/types.h>

struct timeval {
  uint32_t tv_sec;
  uint32_t tv_usec;
};

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

#endif
