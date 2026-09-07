#include "lapic.h"
#include "acpi.h"
#include "vmm.h"

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
