#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint32_t year;
} rtc_time_t;

void rtc_get_time(rtc_time_t *time);
void rtc_print_time(rtc_time_t *time);

// Helper for FAT32 timestamps
uint16_t rtc_get_fat_time(rtc_time_t *time);
uint16_t rtc_get_fat_date(rtc_time_t *time);

#endif // RTC_H
