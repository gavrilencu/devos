#include "tss.h"
#include "string.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;              /* stiva de kernel pentru intreruperi din ring 3 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} __attribute__((packed));

static struct tss tss;

extern uint8_t gdt64_tss_desc[];   /* din gdt.asm, intrarea 0x28 */

void tss_init(uint64_t rsp0)
{
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = rsp0;
    tss.iopb = sizeof(tss);        /* fara bitmap de porturi I/O */

    /* Descriptor de sistem pe 16 bytes: TSS pe 64 de biti, disponibil. */
    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(tss) - 1;
    uint8_t *d = gdt64_tss_desc;

    d[0] = (uint8_t)(limit & 0xFF);
    d[1] = (uint8_t)(limit >> 8);
    d[2] = (uint8_t)(base & 0xFF);
    d[3] = (uint8_t)(base >> 8);
    d[4] = (uint8_t)(base >> 16);
    d[5] = 0x89;                   /* present | DPL0 | tip 9 = TSS 64-bit */
    d[6] = (uint8_t)((limit >> 16) & 0x0F);
    d[7] = (uint8_t)(base >> 24);
    *(uint32_t *)(d + 8)  = (uint32_t)(base >> 32);
    *(uint32_t *)(d + 12) = 0;

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)0x28));
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
