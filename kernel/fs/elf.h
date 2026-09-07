#pragma once
#include <stdint.h>
#include "vmm.h"

/* Incarcator ELF64 minimal: accepta doar executabile statice (ET_EXEC)
 * pentru x86-64 si proceseaza segmentele PT_LOAD, mapandu-le cu bitul U
 * in spatiul de adrese dat. Zona dintre filesz si memsz (.bss) ramane 0.
 *
 * Intoarce 0 si scrie entry point-ul in *entry_out, sau -1 la orice
 * problema (format invalid, adrese in afara spatiului user, memorie). */
int elf_load(address_space_t space, const void *image, uint64_t size,
             uint64_t *entry_out);
