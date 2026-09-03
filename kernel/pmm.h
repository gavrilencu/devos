#pragma once
#include <stdint.h>

/* Alocatorul de memorie fizica: imparte RAM-ul in cadre de 4 KiB si tine
 * evidenta lor intr-un bitmap, pe baza hartii E820 primite de la BIOS. */

#define PMM_FRAME_SIZE 4096ull

void pmm_init(void);

/* Intoarce adresa fizica a unui cadru liber de 4 KiB (si il marcheaza
 * ocupat), sau 0 daca nu mai e memorie. */
uint64_t pmm_alloc(void);

/* Aloca `count` cadre fizice consecutive; intoarce adresa primului
 * sau 0 daca nu exista un interval contiguu suficient de mare. */
uint64_t pmm_alloc_contig(uint64_t count);

void pmm_free(uint64_t frame_addr);

uint64_t pmm_free_bytes(void);

/* Cata memorie fizica gestioneaza PMM-ul (de la 0 la ultimul cadru). */
uint64_t pmm_managed_bytes(void);
