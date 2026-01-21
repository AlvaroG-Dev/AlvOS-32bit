#ifndef STRING_H
#define STRING_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// Avoid redefining standard functions in kernel code to prevent conflicts
#ifndef KERNEL_BUILD
#define itoa kitoa
#define snprintf ksnprintf
#define strlcpy kstrlcpy
#endif

void __udivmoddi4(uint64_t dividend, uint64_t divisor, uint64_t *quotient,
                  uint64_t *remainder);
void to_hex(uint64_t val, char *buf);
void to_decimal(uint32_t val, char *buf);
int sprintf(char *buf, const char *fmt, ...);
static void reverse(char *s, int len);
size_t strlen(const char *str);
unsigned long long __udivdi3(unsigned long long n, unsigned long long d);
char *kitoa(int value, char *str, int base);
int int_itoa(int value, char *str, int base);
int snprintf(char *str, size_t size, const char *format, ...);
size_t kstrlcpy(char *dst, const char *src, size_t size);
char *uitoa(uint32_t value, char *str, int base);
void ulltoa(unsigned long long value, char *buf, int base);
char *strcat(char *dest, const char *src);
int strncmp(const char *s1, const char *s2, unsigned int n);
int strcmp(const char *s1, const char *s2);
char *trim_whitespace(char *str);
int isspace(int c);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
unsigned long strtoul(const char *str, char **endptr, int base);
int atoi(const char *str);
long strtol(const char *nptr, char **endptr, int base);
uint64_t strtoull(const char *str, char **endptr, int base);
int memcmp(const void *ptr1, const void *ptr2, size_t num);
char *strncat(char *dest, const char *src, size_t n);
char *strtok(char *str, const char *delim);
char *strchr(const char *str, int c);
char *strrchr(const char *str, int c);
char *strtok_r(char *str, const char *delim, char **saveptr);
size_t strcspn(const char *str1, const char *str2);
size_t strnlen(const char *s, size_t maxlen);
void u64_to_str(uint64_t value, char *buffer);
char *strstr(const char *haystack, const char *needle);
int vsnprintf(char *str, size_t size, const char *format, va_list args);
int toupper(int c);
int tolower(int c);
void strupper(char *s);
int strcasecmp(const char *s1, const char *s2);
int sscanf(const char *str, const char *fmt, ...);

#endif