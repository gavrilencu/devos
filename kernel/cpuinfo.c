#include "cpuinfo.h"
#include "pit.h"

static inline void cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cpu_vendor(char *s)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    ((uint32_t *)s)[0] = b;
    ((uint32_t *)s)[1] = d;
    ((uint32_t *)s)[2] = c;
    s[12] = 0;
}

int cpu_brand(char *s)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) { s[0] = 0; return 0; }
    uint32_t *p = (uint32_t *)s;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid(leaf, 0, &a, &b, &c, &d);
        *p++ = a; *p++ = b; *p++ = c; *p++ = d;
    }
    s[48] = 0;
    /* taie spatiile din fata (brand string-ul are adesea padding) */
    return 1;
}

int cpu_logical(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    int n = (int)((b >> 16) & 0xFF);
    return n ? n : 1;
}

void cpu_features(char *s, int max)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    int p = 0;
    #define ADD(str) do { const char *q = str; \
        if (p && p < max - 1) s[p++] = ' '; \
        while (*q && p < max - 1) s[p++] = *q++; } while (0)
    if (d & (1u << 25)) ADD("SSE");
    if (d & (1u << 26)) ADD("SSE2");
    if (c & (1u << 0))  ADD("SSE3");
    if (c & (1u << 19)) ADD("SSE4.1");
    if (c & (1u << 20)) ADD("SSE4.2");
    if (c & (1u << 28)) ADD("AVX");
    if (d & (1u << 4))  ADD("TSC");
    if (d & (1u << 5))  ADD("MSR");
    #undef ADD
    s[p] = 0;
}

static int mhz_cached;

void cpu_info_init(void)
{
    /* masuram frecventa: cate cicluri TSC intr-un interval PIT de 100 ms.
     * PIT ruleaza la 100 Hz (10 ms/tick). Se cheama la boot, cu IF=1. */
    uint64_t s = pit_ticks();
    while (pit_ticks() == s)
        ;                              /* aliniere la marginea unui tick */
    uint64_t t0 = rdtsc();
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 10)
        ;                              /* 10 tick-uri = 100 ms */
    uint64_t t1 = rdtsc();
    uint64_t ms = (pit_ticks() - start) * 10;
    if (ms)
        mhz_cached = (int)((t1 - t0) / (ms * 1000));
}

int cpu_mhz(void)
{
    return mhz_cached;
}
