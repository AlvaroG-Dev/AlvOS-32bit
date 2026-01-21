#include "rtc.h"
#include "io.h"
#include "terminal.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

int get_update_in_progress_flag() {
  outb(CMOS_ADDRESS, 0x0A);
  return (inb(CMOS_DATA) & 0x80);
}

uint8_t get_rtc_register(int reg) {
  outb(CMOS_ADDRESS, reg);
  return inb(CMOS_DATA);
}

void rtc_get_time(rtc_time_t *time) {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint8_t year;
  uint8_t century = 0;
  uint8_t last_second;
  uint8_t last_minute;
  uint8_t last_hour;
  uint8_t last_day;
  uint8_t last_month;
  uint8_t last_year;
  uint8_t last_century;
  uint8_t registerB;

  // Ensure we don't read during an update
  while (get_update_in_progress_flag())
    ;
  second = get_rtc_register(0x00);
  minute = get_rtc_register(0x02);
  hour = get_rtc_register(0x04);
  day = get_rtc_register(0x07);
  month = get_rtc_register(0x08);
  year = get_rtc_register(0x09);
  // Century register (might not exist on all machines, but common on ACPI)
  // We can try to read it if we know the offset from ACPI FADT,
  // but 0x32 is a common default.
  // century = get_rtc_register(0x32);

  do {
    last_second = second;
    last_minute = minute;
    last_hour = hour;
    last_day = day;
    last_month = month;
    last_year = year;
    last_century = century;

    while (get_update_in_progress_flag())
      ;
    second = get_rtc_register(0x00);
    minute = get_rtc_register(0x02);
    hour = get_rtc_register(0x04);
    day = get_rtc_register(0x07);
    month = get_rtc_register(0x08);
    year = get_rtc_register(0x09);
    // century = get_rtc_register(0x32);
  } while ((last_second != second) || (last_minute != minute) ||
           (last_hour != hour) || (last_day != day) || (last_month != month) ||
           (last_year != year) || (last_century != century));

  registerB = get_rtc_register(0x0B);

  // Convert BCD to binary if necessary
  if (!(registerB & 0x04)) {
    second = (second & 0x0F) + ((second / 16) * 10);
    minute = (minute & 0x0F) + ((minute / 16) * 10);
    hour = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
    day = (day & 0x0F) + ((day / 16) * 10);
    month = (month & 0x0F) + ((month / 16) * 10);
    year = (year & 0x0F) + ((year / 16) * 10);
    // century = (century & 0x0F) + ((century / 16) * 10);
  }

  // Convert 12 hour clock to 24 hour clock if necessary
  if (!(registerB & 0x02) && (hour & 0x80)) {
    hour = ((hour & 0x7F) + 12) % 24;
  }

  time->second = second;
  time->minute = minute;
  time->hour = hour;
  time->day = day;
  time->month = month;
  time->year = 2000 + year; // Assumes 21st century if century reg missing
}

uint16_t rtc_get_fat_time(rtc_time_t *time) {
  // Bits 0-4: Seconds / 2
  // Bits 5-10: Minutes
  // Bits 11-15: Hours
  return ((uint16_t)time->hour << 11) | ((uint16_t)time->minute << 5) |
         (time->second / 2);
}

uint16_t rtc_get_fat_date(rtc_time_t *time) {
  // Bits 0-4: Day
  // Bits 5-8: Month
  // Bits 9-15: Year offset from 1980
  uint32_t year_offset = (time->year >= 1980) ? (time->year - 1980) : 0;
  return ((uint16_t)(year_offset & 0x7F) << 9) | ((uint16_t)time->month << 5) |
         (time->day & 0x1F);
}

extern Terminal main_terminal;
void rtc_print_time(rtc_time_t *time) {
  terminal_printf(&main_terminal, "%02u/%02u/%04u %02u:%02u:%02u\r\n",
                  time->day, time->month, time->year, time->hour, time->minute,
                  time->second);
}
