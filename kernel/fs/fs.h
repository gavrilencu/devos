#pragma once
#include <stdint.h>

/* MyFS — sistem de fisiere read-only, minimal:
 *   LBA 2048 (offset 1 MiB in imagine): superbloc
 *     "MYFS" (4 bytes) + numarul de fisiere (u32)
 *     apoi tabelul: intrari de 32 de bytes (nume, LBA relativ, dimensiune)
 *   datele fisierelor urmeaza, aliniate la sector.
 * Imaginea o construieste unealta scripts/mkfs.c la build. */

struct fs_file {
    char name[24];
    uint32_t lba;      /* relativ la inceputul zonei FS */
    uint32_t size;     /* in bytes */
} __attribute__((packed));

void fs_init(void);
int fs_count(void);
const struct fs_file *fs_get(int idx);
const struct fs_file *fs_find(const char *name);

/* Citeste fisierul intreg intr-un buffer kmalloc (apelantul face kfree).
 * Intoarce NULL daca fisierul nu exista sau citirea esueaza. */
void *fs_read_file(const char *name, uint32_t *size_out);

/* Citeste fisierul direct intr-un buffer dat de apelant (pentru fisiere
 * mari, ex. imagini de fundal — heap-ul kernelului e mic). Intoarce
 * numarul de bytes sau -1. */
int64_t fs_read_into(const char *name, void *dst, uint32_t maxlen);

/* Creeaza sau suprascrie un fisier (persistent pe disc).
 * Intoarce 0 la succes, -1 eroare, -2 disc plin, -3 tabel plin. */
int fs_save(const char *name, const void *data, uint32_t size);

/* Sterge un fisier din tabel (sectoarele lui raman nefolosite — MyFS nu
 * are inca lista de sectoare libere). 0 la succes, -1 daca nu exista. */
int fs_delete(const char *name);

/* Redenumeste un fisier (doar intrarea din tabel — instant, indiferent de
 * marime). 0 la succes, -1 nu exista, -2 numele nou e invalid/ocupat,
 * -3 fisier de sistem protejat. */
int fs_rename(const char *oldname, const char *newname);

/* 1 daca fisierul e de sistem (program sau livrat de build) si nu poate fi
 * sters/redenumit. */
int fs_is_protected(const char *name);
