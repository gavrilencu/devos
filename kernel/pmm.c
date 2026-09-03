#include "pmm.h"
#include "kprintf.h"
#include "string.h"

/* Adresele la care stage2 a depus harta E820 (vezi boot/stage2.asm). */
#define E820_COUNT_ADDR 0x5000
#define E820_TABLE_ADDR 0x5008

struct e820_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed));

#define E820_USABLE 1

/* stage2 mapeaza (identity) doar primul 1 GiB, deci atat gestionam
 * deocamdata; limita creste cand vom avea manager de memorie virtuala. */
#define MAX_MEM    (1024ull * 1024 * 1024)
#define MAX_FRAMES (MAX_MEM / PMM_FRAME_SIZE)

static uint8_t bitmap[MAX_FRAMES / 8];   /* 32 KiB in .bss; bit setat = ocupat */
static uint64_t total_frames;            /* cate cadre gestionam efectiv */
static uint64_t free_frames;

extern char __kernel_end[];              /* din linker.ld */

static inline void set_used(uint64_t f) { bitmap[f >> 3] |= (uint8_t)(1 << (f & 7)); }
static inline void set_free(uint64_t f) { bitmap[f >> 3] &= (uint8_t)~(1 << (f & 7)); }
static inline int  is_used(uint64_t f)  { return bitmap[f >> 3] & (1 << (f & 7)); }

static const char *e820_type_name(uint32_t t)
{
    switch (t) {
    case 1:  return "utilizabil";
    case 2:  return "rezervat";
    case 3:  return "ACPI (recuperabil)";
    case 4:  return "ACPI NVS";
    case 5:  return "defect";
    default: return "necunoscut";
    }
}

void pmm_init(void)
{
    uint32_t n = *(volatile uint32_t *)E820_COUNT_ADDR;
    const struct e820_entry *e = (const struct e820_entry *)E820_TABLE_ADDR;

    if (n == 0) {
        kprintf("EROARE: BIOS-ul nu a furnizat harta E820!\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    kprintf("\nHarta memoriei (E820, %u regiuni):\n", n);

    /* Pornim cu totul "ocupat"; eliberam doar ce declara BIOS-ul utilizabil. */
    memset(bitmap, 0xFF, sizeof(bitmap));

    uint64_t limit = 0;
    for (uint32_t i = 0; i < n; i++) {
        kprintf("  %p - %p  %s\n",
                (void *)e[i].base, (void *)(e[i].base + e[i].len - 1),
                e820_type_name(e[i].type));

        if (e[i].type != E820_USABLE)
            continue;

        /* Aliniem conservator: inceputul in sus, sfarsitul in jos. */
        uint64_t start = (e[i].base + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
        uint64_t end   = (e[i].base + e[i].len) & ~(PMM_FRAME_SIZE - 1);
        if (end > MAX_MEM)
            end = MAX_MEM;
        if (start >= end)
            continue;

        for (uint64_t f = start / PMM_FRAME_SIZE; f < end / PMM_FRAME_SIZE; f++)
            set_free(f);
        if (end > limit)
            limit = end;
    }
    total_frames = limit / PMM_FRAME_SIZE;

    /* Rezervam tot ce e sub sfarsitul kernelului: IVT + zona BIOS, tabelele
     * de paginare (0x1000), harta E820 (0x5000), stage1/2, kernel + .bss. */
    uint64_t kend = ((uint64_t)__kernel_end + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
    for (uint64_t f = 0; f < kend / PMM_FRAME_SIZE; f++)
        set_used(f);

    free_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++)
        if (!is_used(f))
            free_frames++;

    kprintf("RAM gestionata: %llu MiB; libera: %llu MiB "
            "(%llu cadre de 4 KiB); kernelul ocupa pana la %p\n",
            (unsigned long long)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
            (unsigned long long)(free_frames * PMM_FRAME_SIZE / (1024 * 1024)),
            (unsigned long long)free_frames, (void *)kend);
}

uint64_t pmm_alloc(void)
{
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!is_used(f)) {
            set_used(f);
            free_frames--;
            return f * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_contig(uint64_t count)
{
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (is_used(f)) {
            run = 0;
            continue;
        }
        if (run == 0)
            start = f;
        if (++run == count) {
            for (uint64_t i = start; i <= f; i++)
                set_used(i);
            free_frames -= count;
            return start * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t frame_addr)
{
    uint64_t f = frame_addr / PMM_FRAME_SIZE;
    if (f >= total_frames || !is_used(f)) {
        kprintf("PMM: free invalid pentru %p!\n", (void *)frame_addr);
        return;
    }
    set_free(f);
    free_frames++;
}

uint64_t pmm_free_bytes(void)
{
    return free_frames * PMM_FRAME_SIZE;
}

uint64_t pmm_managed_bytes(void)
{
    return total_frames * PMM_FRAME_SIZE;
}
