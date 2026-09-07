#include "kheap.h"
#include "pmm.h"
#include "kprintf.h"

/* 256 KiB de heap, alocati ca un bloc fizic contiguu la boot (memoria e
 * inca nefragmentata atunci). Cand vom avea manager de memorie virtuala,
 * heap-ul va putea creste dinamic din pagini oarecare. */
#define HEAP_PAGES 64

/* Fiecare bloc (liber sau alocat) incepe cu un header de 16 bytes care ii
 * tine dimensiunea totala; blocurile libere folosesc si campul next.
 * Payload-ul alocat incepe la block + 16, deci ramane aliniat la 16. */
struct free_block {
    size_t size;                /* dimensiunea totala, inclusiv headerul */
    struct free_block *next;
};

#define HDR_SIZE   16
#define MIN_SPLIT  (HDR_SIZE + 16)
#define ALIGN16(x) (((x) + 15) & ~(size_t)15)

static uint8_t *heap_base;
static size_t heap_size;
static struct free_block *free_list;

void kheap_init(void)
{
    uint64_t phys = pmm_alloc_contig(HEAP_PAGES);
    if (!phys) {
        kprintf("EROARE: nu am gasit %u pagini fizice contigue pentru heap\n",
                (unsigned)HEAP_PAGES);
        return;
    }
    heap_base = (uint8_t *)phys;    /* primul 1 GiB e identity-mapped */
    heap_size = HEAP_PAGES * PMM_FRAME_SIZE;

    free_list = (struct free_block *)heap_base;
    free_list->size = heap_size;
    free_list->next = NULL;
}

void *kmalloc(size_t size)
{
    if (size == 0 || heap_base == NULL)
        return NULL;

    size_t need = ALIGN16(size) + HDR_SIZE;

    struct free_block **pp = &free_list;
    for (struct free_block *b = free_list; b; pp = &b->next, b = b->next) {
        if (b->size < need)
            continue;

        if (b->size >= need + MIN_SPLIT) {
            /* spargem blocul: restul ramane in lista de blocuri libere */
            struct free_block *rest = (struct free_block *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            *pp = rest;
            b->size = need;
        } else {
            *pp = b->next;
        }

        *(size_t *)b = b->size;     /* headerul blocului alocat */
        return (uint8_t *)b + HDR_SIZE;
    }
    return NULL;                    /* heap plin */
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    struct free_block *b = (struct free_block *)((uint8_t *)ptr - HDR_SIZE);
    b->size = *(size_t *)b;

    /* inseram ordonat dupa adresa, ca sa putem lipi blocurile vecine */
    struct free_block *prev = NULL, *cur = free_list;
    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    b->next = cur;
    if (prev)
        prev->next = b;
    else
        free_list = b;

    /* coalescenta cu blocul urmator, apoi cu cel precedent */
    if (cur && (uint8_t *)b + b->size == (uint8_t *)cur) {
        b->size += cur->size;
        b->next = cur->next;
    }
    if (prev && (uint8_t *)prev + prev->size == (uint8_t *)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
}

size_t kheap_free_bytes(void)
{
    size_t total = 0;
    for (struct free_block *b = free_list; b; b = b->next)
        total += b->size;
    return total;
}

size_t kheap_total_bytes(void)
{
    return heap_size;
}
