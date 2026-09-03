#pragma once
#include <stdint.h>

/* Managerul de memorie virtuala: kernelul isi construieste propriile tabele
 * de paginare (inlocuind pe cele statice din stage2) si poate mapa/demapa
 * pagini de 4 KiB la orice adresa virtuala. */

/* Flags pentru vmm_map (peste PTE_P, care e implicit). */
#define VMM_W 0x002   /* scriere permisa */
#define VMM_U 0x004   /* accesibil din user mode (ring 3) */

/* Un spatiu de adrese = un PML4 (cadru fizic, identity-mapped). Kernelul
 * sta in PML4[0] si e partajat de toate procesele; spatiul user al unui
 * proces sta in PML4[1] (adrese de la 512 GiB) si e privat. */
typedef uint64_t *address_space_t;
#define VMM_USER_SLOT 1

void vmm_init(void);

address_space_t vmm_kernel_space(void);

/* Creeaza un spatiu de adrese nou care partajeaza kernelul (intrarile
 * PML4 copiate pointeaza spre ACELEASI tabele) si are user space gol. */
address_space_t vmm_create_space(void);

/* Elibereaza tot subtree-ul user (tabele + cadrele mapate) si PML4-ul.
 * Nu se cheama niciodata pe spatiul aflat in CR3. */
void vmm_destroy_space(address_space_t space);

/* Mapeaza pagina virtuala `virt` la cadrul fizic `phys` (ambele aliniate
 * la 4 KiB). Intoarce 0 la succes, -1 daca nu mai sunt cadre pentru tabele. */
int vmm_map_in(address_space_t space, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_in(address_space_t space, uint64_t virt);
uint64_t vmm_translate_in(address_space_t space, uint64_t virt);

/* Variantele fara spatiu lucreaza pe spatiul kernelului. */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);

/* Intoarce adresa fizica pe care e mapata `virt`, sau VMM_NOT_MAPPED. */
#define VMM_NOT_MAPPED (~0ull)
uint64_t vmm_translate(uint64_t virt);
