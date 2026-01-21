#include "lib_os.h"

int main_entry(int argc, char **argv) {
  (void)argc;
  (void)argv;
  rtc_time_t time;
  if (get_rtc(&time) == 0) {
    print("RTC Time: ");

    // Very basic int to string
    char buf[16];
    int n;

    // Hour
    n = time.hour;
    buf[0] = (n / 10) + '0';
    buf[1] = (n % 10) + '0';
    buf[2] = ':';

    // Minute
    n = time.minute;
    buf[3] = (n / 10) + '0';
    buf[4] = (n % 10) + '0';
    buf[5] = ':';

    // Second
    n = time.second;
    buf[6] = (n / 10) + '0';
    buf[7] = (n % 10) + '0';
    buf[8] = '\r';
    buf[9] = '\n';
    buf[10] = '\0';

    print(buf);
  } else {
    print("Failed to get RTC\n");
  }

  exit(0);
  return 0;
}
