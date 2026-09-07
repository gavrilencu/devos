#pragma once
#include <stdint.h>

/* Decodor PNG (8 biti/canal; tipuri 0 gri, 2 RGB, 3 paleta, 4 gri+alfa, 6 RGBA;
 * fara interlace). Scrie pixeli 0x00RRGGBB (alfa compus peste alb) in `pix`.
 * `scratch` = buffer temporar pentru datele decomprimate. 0 = succes. */
int png_decode(const uint8_t *in, int inlen, uint32_t *pix, int maxpix,
               int *w, int *h, uint8_t *scratch, int scratchcap);
