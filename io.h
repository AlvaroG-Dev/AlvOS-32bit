#ifndef IO_H
#define IO_H

#include <stdint.h>

// Lee un byte del puerto especificado
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Envía un byte al puerto especificado
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Lee una palabra (2 bytes) del puerto especificado
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Envía una palabra (2 bytes) al puerto especificado
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

// Lee un long (4 bytes) del puerto especificado
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Envía un long (4 bytes) al puerto especificado
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

// Lee 'count' palabras (16-bit) desde el puerto a la memoria
static inline void insw(uint16_t port, volatile void* addr, uint32_t count) {
    __asm__ volatile (
        "cld; rep insw"
        : "=D"(addr)
        : "d"(port), "c"(count), "0"(addr)
        : "memory"
    );
}

// Escribe 'count' palabras (16-bit) desde la memoria al puerto
static inline void outsw(uint16_t port, const volatile void* addr, uint32_t count) {
    __asm__ volatile (
        "cld; rep outsw"
        : "=S"(addr)
        : "d"(port), "c"(count), "0"(addr)
        : "memory"
    );
}

// Funciones para Memory-Mapped I/O (MMIO) - NUEVAS
static inline uint8_t readb(uint32_t addr) {
    return *(volatile uint8_t*)addr;
}

static inline void writeb(uint32_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

static inline uint16_t readw(uint32_t addr) {
    return *(volatile uint16_t*)addr;
}

static inline void writew(uint32_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

static inline uint32_t readl(uint32_t addr) {
    return *(volatile uint32_t*)addr;
}

static inline void writel(uint32_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

#endif // IO_H