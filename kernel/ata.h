#pragma once
#include <stdint.h>

/* Driver ATA PIO (LBA28, polling) pentru discul primar master —
 * acelasi disc de pe care am bootat. */

/* Citeste `count` sectoare de 512 bytes incepand de la `lba` in `buf`.
 * Intoarce 0 la succes, -1 la eroare raportata de disc. */
int ata_read(uint32_t lba, uint32_t count, void *buf);

/* Scrie `count` sectoare de 512 bytes pe disc (cu flush de cache). */
int ata_write(uint32_t lba, uint32_t count, const void *buf);
