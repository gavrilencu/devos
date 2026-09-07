#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"

#define PTE_P 0x001
#define PTE_W 0x002
#define PTE_U 0x004
#define PTE_NX (1ull << 63)   /* No-eXecute (necesita EFER.NXE) */

#define PTE_ADDR(e)   ((e) & 0x000FFFFFFFFFF000ull)
#define PML4_IDX(v)   (((v) >> 39) & 511)
#define PDPT_IDX(v)   (((v) >> 30) & 511)
#define PD_IDX(v)     (((v) >> 21) & 511)
#define PT_IDX(v)     (((v) >> 12) & 511)

/* Toate cadrele fizice sunt identity-mapped, deci putem folosi adresa
 * fizica a unei tabele direct ca pointer. */
static uint64_t *kernel_pml4;

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Intoarce tabela de sub intrarea `idx` din `parent`; daca nu exista si
 * create=1, aloca un cadru nou, il curata si il leaga. */
static uint64_t *get_table(uint64_t *parent, int idx, int create)
{
    if (!(parent[idx] & PTE_P)) {
        if (!create)
            return NULL;
        uint64_t frame = pmm_alloc();
        if (frame == 0)
            return NULL;
        memset((void *)frame, 0, PMM_FRAME_SIZE);
        /* Nivelurile intermediare primesc W+U; protectia reala o decide
         * intrarea finala (accesul user cere U pe TOATE nivelurile). */
        parent[idx] = frame | PTE_P | PTE_W | PTE_U;
    }
    return (uint64_t *)PTE_ADDR(parent[idx]);
}

int vmm_map_in(address_space_t space, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = get_table(space, PML4_IDX(virt), 1);
    if (!pdpt)
        return -1;
    uint64_t *pd = get_table(pdpt, PDPT_IDX(virt), 1);
    if (!pd)
        return -1;
    uint64_t *pt = get_table(pd, PD_IDX(virt), 1);
    if (!pt)
        return -1;

    uint64_t pte = (phys & ~0xFFFull) | PTE_P | (flags & (PTE_W | PTE_U));
    if (flags & VMM_NX)
        pte |= PTE_NX;                        /* pagina ne-executabila (W^X) */
    pt[PT_IDX(virt)] = pte;
    invlpg(virt);   /* inofensiv si daca `space` nu e cel din CR3 */
    return 0;
}

void vmm_unmap_in(address_space_t space, uint64_t virt)
{
    uint64_t *pdpt = get_table(space, PML4_IDX(virt), 0);
    if (!pdpt)
        return;
    uint64_t *pd = get_table(pdpt, PDPT_IDX(virt), 0);
    if (!pd)
        return;
    uint64_t *pt = get_table(pd, PD_IDX(virt), 0);
    if (!pt)
        return;

    pt[PT_IDX(virt)] = 0;
    invlpg(virt);
}

uint64_t vmm_translate_in(address_space_t space, uint64_t virt)
{
    uint64_t *pdpt = get_table(space, PML4_IDX(virt), 0);
    if (!pdpt)
        return VMM_NOT_MAPPED;
    uint64_t *pd = get_table(pdpt, PDPT_IDX(virt), 0);
    if (!pd)
        return VMM_NOT_MAPPED;
    uint64_t *pt = get_table(pd, PD_IDX(virt), 0);
    if (!pt)
        return VMM_NOT_MAPPED;

    uint64_t e = pt[PT_IDX(virt)];
    if (!(e & PTE_P))
        return VMM_NOT_MAPPED;
    return PTE_ADDR(e) | (virt & 0xFFF);
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return vmm_map_in(kernel_pml4, virt, phys, flags);
}

void vmm_unmap(uint64_t virt)
{
    vmm_unmap_in(kernel_pml4, virt);
}

uint64_t vmm_translate(uint64_t virt)
{
    return vmm_translate_in(kernel_pml4, virt);
}

address_space_t vmm_kernel_space(void)
{
    return kernel_pml4;
}

address_space_t vmm_create_space(void)
{
    uint64_t frame = pmm_alloc();
    if (frame == 0)
        return 0;
    uint64_t *space = (uint64_t *)frame;

    /* Kernelul e partajat: intrarile PML4 copiate pointeaza spre aceleasi
     * tabele, deci orice mapare noua a kernelului se vede in toate
     * procesele. Slotul user ramane gol — privat. */
    memcpy(space, kernel_pml4, PMM_FRAME_SIZE);
    space[VMM_USER_SLOT] = 0;
    return space;
}

void vmm_destroy_space(address_space_t space)
{
    uint64_t e1 = space[VMM_USER_SLOT];
    if (e1 & PTE_P) {
        uint64_t *pdpt = (uint64_t *)PTE_ADDR(e1);
        for (int i = 0; i < 512; i++) {
            if (!(pdpt[i] & PTE_P))
                continue;
            uint64_t *pd = (uint64_t *)PTE_ADDR(pdpt[i]);
            for (int j = 0; j < 512; j++) {
                if (!(pd[j] & PTE_P))
                    continue;
                uint64_t *pt = (uint64_t *)PTE_ADDR(pd[j]);
                for (int k = 0; k < 512; k++)
                    if (pt[k] & PTE_P)
                        pmm_free(PTE_ADDR(pt[k]));   /* cadrele user */
                pmm_free((uint64_t)pt);
            }
            pmm_free((uint64_t)pd);
        }
        pmm_free((uint64_t)pdpt);
    }
    pmm_free((uint64_t)space);
}

/* Copiaza intreg spatiul user (subtree-ul PML4[1]) din `src` in `dst`, cadru
 * cu cadru: pentru fiecare pagina user mapata aloca un cadru nou, ii copiaza
 * continutul (toate cadrele fizice sunt identity-mapped) si o mapeaza in `dst`
 * cu ACELEASI permisiuni (W/U/NX). Folosit de fork(). Copiere completa (eager),
 * fara copy-on-write. Intoarce 0 la succes, -1 daca ramane fara memorie (caz in
 * care apelantul distruge `dst`, eliberand ce s-a alocat pana atunci). */
int vmm_fork_user(address_space_t dst, address_space_t src)
{
    uint64_t e1 = src[VMM_USER_SLOT];
    if (!(e1 & PTE_P))
        return 0;                       /* procesul nu are nimic user mapat */
    uint64_t *pdpt = (uint64_t *)PTE_ADDR(e1);
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & PTE_P))
            continue;
        uint64_t *pd = (uint64_t *)PTE_ADDR(pdpt[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_P))
                continue;
            uint64_t *pt = (uint64_t *)PTE_ADDR(pd[j]);
            for (int k = 0; k < 512; k++) {
                uint64_t e = pt[k];
                if (!(e & PTE_P))
                    continue;
                uint64_t va = ((uint64_t)VMM_USER_SLOT << 39) |
                              ((uint64_t)i << 30) | ((uint64_t)j << 21) |
                              ((uint64_t)k << 12);
                uint64_t flags = 0;
                if (e & PTE_W)  flags |= VMM_W;
                if (e & PTE_U)  flags |= VMM_U;
                if (e & PTE_NX) flags |= VMM_NX;
                uint64_t nf = pmm_alloc();
                if (nf == 0)
                    return -1;
                memcpy((void *)nf, (const void *)PTE_ADDR(e), PMM_FRAME_SIZE);
                if (vmm_map_in(dst, va, nf, flags) < 0) {
                    pmm_free(nf);
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* Activeaza bitul NXE din EFER (MSR 0xC0000080) ca bitul NX din PTE-uri sa
 * fie luat in seama. Paginile existente au NX=0 (executabile), deci e sigur. */
static void enable_nxe(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 11);                        /* NXE */
    __asm__ volatile("wrmsr" : : "c"(0xC0000080), "a"(lo), "d"(hi));
}

void vmm_init(void)
{
    enable_nxe();

    kernel_pml4 = (uint64_t *)pmm_alloc();
    if (!kernel_pml4) {
        kprintf("EROARE: nu am cadru pentru PML4\n");
        return;
    }
    memset(kernel_pml4, 0, PMM_FRAME_SIZE);

    /* Identity map pentru toata memoria gestionata, cu pagini de 4 KiB.
     * Cadrele consumate aici pentru tabele sunt si ele sub limita, deci
     * bucla le mapeaza automat. */
    uint64_t limit = pmm_managed_bytes();
    for (uint64_t addr = 0; addr < limit; addr += PMM_FRAME_SIZE) {
        if (vmm_map(addr, addr, VMM_W) < 0) {
            kprintf("EROARE: VMM a ramas fara cadre la %p\n", (void *)addr);
            return;
        }
    }

    /* Comutam pe tabelele noastre; cele din stage2 (0x1000-0x3FFF) raman
     * doar istorie. Incarcarea CR3 goleste si TLB-ul. */
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)kernel_pml4) : "memory");

    kprintf("[ok] VMM: tabele de paginare proprii, %llu MiB identity-mapped "
            "cu pagini de 4 KiB\n",
            (unsigned long long)(limit / (1024 * 1024)));
}
