#pragma once
#include <stdint.h>

/* Acces la porturile I/O x86. */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Citeste `count` cuvinte de 16 biti de pe `port` in `buf` (pentru ATA PIO). */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(count)
                     : "d"(port)
                     : "memory");
}

/* Scrie `count` cuvinte de 16 biti din `buf` pe `port`. */
static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    __asm__ volatile("rep outsw"
                     : "+S"(buf), "+c"(count)
                     : "d"(port)
                     : "memory");
}
