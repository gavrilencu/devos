#include "pit.h"
#include "io.h"
#include "interrupts.h"
#include "fb.h"

#define PIT_BASE_HZ 1193182u    /* frecventa cristalului PIT */

static volatile uint64_t ticks;
static uint32_t pit_hz;
static void (*second_cb)(uint64_t seconds);
static void (*fast_cb)(void);

static void pit_irq(struct int_frame *f)
{
    (void)f;
    ticks++;
    /* prezentam back buffer-ul pe ecran de ~50 de ori pe secunda (la 100 Hz,
     * la fiecare al doilea tick) — o singura copie rapida, fara tearing. */
    if (fb_active() && (ticks & 1))
        fb_flush();
    /* redesenari rapide (ex. browserul cand se incarca); ieftin daca
     * nu s-a schimbat nimic. */
    if (fast_cb && (ticks % 8 == 0))
        fast_cb();
    if (second_cb && pit_hz && ticks % pit_hz == 0)
        second_cb(ticks / pit_hz);
}

void pit_set_second_callback(void (*fn)(uint64_t seconds))
{
    second_cb = fn;
}

void pit_set_fast_callback(void (*fn)(void))
{
    fast_cb = fn;
}

void pit_init(uint32_t hz)
{
    pit_hz = hz;
    uint32_t div = PIT_BASE_HZ / hz;
    outb(0x43, 0x36);                    /* canal 0, acces lo/hi, mod 3 */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)(div >> 8));
    irq_install(0, pit_irq);
}

uint64_t pit_ticks(void)
{
    return ticks;
}
