#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>
#include <stddef.h>

int printf(const char *format, ...);
int putchar(int c);
int puts(const char *s);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list args);

#endif
