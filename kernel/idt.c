#include <stdint.h>
#include "interrupts.h"

/* O intrare in IDT pe x86-64 are 16 bytes; adresa handlerului e imprastiata
 * in trei campuri, din motive istorice. */
struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t reserved;
} __attribute__((packed));

static struct idt_entry idt[256];

extern uint64_t isr_stub_table[49];   /* din isr.asm */
extern char isr128[];                 /* stub-ul de syscall */

static void set_gate(int v, uint64_t handler, uint8_t flags)
{
    idt[v].off_lo   = handler & 0xFFFF;
    idt[v].sel      = 0x08;           /* selectorul de cod din GDT-ul kernelului */
    idt[v].ist      = 0;
    idt[v].flags    = flags;
    idt[v].off_mid  = (handler >> 16) & 0xFFFF;
    idt[v].off_hi   = (uint32_t)(handler >> 32);
    idt[v].reserved = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 49; i++)
        set_gate(i, isr_stub_table[i], 0x8E);   /* present | DPL0 | int gate */

    /* Syscall: DPL=3, ca ring 3 sa aiba voie sa faca "int 0x80". */
    set_gate(128, (uint64_t)isr128, 0xEE);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = { sizeof(idt) - 1, (uint64_t)idt };

    __asm__ volatile("lidt %0" : : "m"(idtr));
}
