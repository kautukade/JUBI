/* io.h — low-level CPU/IO helpers: port I/O, MMIO, TSC, byte swapping. */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* ---- x86 port I/O ---- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ---- Memory-mapped I/O (32-bit) ---- */
static inline void mmio_write32(volatile void *addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_read32(volatile void *addr) {
    return *(volatile uint32_t *)addr;
}

/* ---- Timestamp counter ---- */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Byte order (network = big endian, x86 = little endian) ---- */
static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }

#endif /* IO_H */
