#include "lapic.h"
#include "acpi.h"
#include "vmm.h"
#include "pit.h"

#define LAPIC_ID    0x020    /* ID Register */
#define LAPIC_EOI   0x0B0    /* End Of Interrupt */
#define LAPIC_SVR   0x0F0    /* Spurious Interrupt Vector Register */
#define SVR_ENABLE  0x100    /* bit 8: APIC Software Enable */
#define LAPIC_ICRLO 0x300    /* Interrupt Command Register (low) */
#define LAPIC_ICRHI 0x310    /* Interrupt Command Register (high) */
#define ICR_INIT    0x00004500   /* Delivery mode INIT, assert, edge */
#define ICR_STARTUP 0x00004600   /* Delivery mode Startup (SIPI) */
#define ICR_PENDING 0x00001000   /* bit 12: Delivery Status = pending */

static volatile uint32_t *lapic;    /* baza MMIO mapata */

static inline uint32_t lapic_read(uint32_t reg)  { return lapic[reg / 4]; }
static inline void lapic_write(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }

int lapic_present(void) { return lapic != 0; }

int lapic_init(void)
{
    uint64_t base = acpi_lapic_base();
    if (base == 0)
        base = 0xFEE00000;                 /* implicit arhitectural */

    /* MMIO-ul Local APIC nu e in RAM-ul identity-mapped — il mapam noi. */
    if (vmm_translate(base) == VMM_NOT_MAPPED)
        vmm_map(base, base, VMM_W);
    lapic = (volatile uint32_t *)base;

    /* Activare software: setam bitul 8 + un vector spurios (0xFF). */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | SVR_ENABLE | 0xFF);
    return 1;
}

uint32_t lapic_id(void)
{
    return lapic ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

void lapic_eoi(void)
{
    if (lapic)
        lapic_write(LAPIC_EOI, 0);
}

static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICRLO) & ICR_PENDING)
        __asm__ volatile("pause");
}

void lapic_send_init(uint32_t apic_id)
{
    lapic_write(LAPIC_ICRHI, apic_id << 24);
    lapic_write(LAPIC_ICRLO, ICR_INIT);
    lapic_wait_icr();
}

void lapic_send_startup(uint32_t apic_id, uint8_t vector)
{
    lapic_write(LAPIC_ICRHI, apic_id << 24);
    lapic_write(LAPIC_ICRLO, ICR_STARTUP | vector);
    lapic_wait_icr();
}

/* --- Timer Local APIC (per-nucleu) --- */
#define LVT_TIMER    0x320
#define TIMER_INIT   0x380
#define TIMER_CUR    0x390
#define TIMER_DIV    0x3E0
#define LVT_MASKED   0x10000
#define LVT_PERIODIC 0x20000

static volatile uint64_t timer_ticks;

void lapic_timer_start(uint8_t vector, uint32_t hz)
{
    if (!lapic)
        return;
    lapic_write(TIMER_DIV, 0x3);            /* divizor 16 */
    /* Calibrare: numaram tick-uri LAPIC intr-un interval PIT cunoscut. */
    lapic_write(LVT_TIMER, LVT_MASKED);
    uint64_t t = pit_ticks();
    while (pit_ticks() == t)                /* aliniere la marginea unui tick */
        __asm__ volatile("pause");
    lapic_write(TIMER_INIT, 0xFFFFFFFF);
    uint64_t s = pit_ticks();
    while (pit_ticks() - s < 2)             /* 2 tick-uri PIT = 20 ms */
        __asm__ volatile("pause");
    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(TIMER_CUR);
    uint32_t count = (uint32_t)((uint64_t)elapsed * 50 / hz);   /* /s = elapsed*50 */
    if (count == 0)
        count = 1;
    lapic_write(TIMER_INIT, count);
    lapic_write(LVT_TIMER, vector | LVT_PERIODIC);
}

void lapic_timer_tick(void)      { timer_ticks++; }
uint64_t lapic_timer_count(void) { return timer_ticks; }
