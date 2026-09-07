#pragma once
#include <stdint.h>

/* Decompresor DEFLATE (RFC 1951) — pentru PNG (zlib). Intoarce numarul de
 * octeti scrisi in `out` (max `outcap`), sau -1 la eroare. `in`/`inlen` = flux
 * DEFLATE brut (fara antetul zlib de 2 octeti). */
int inflate(const uint8_t *in, int inlen, uint8_t *out, int outcap);
