#pragma once
#include <stdint.h>
#include <stddef.h>

/* AES-128/256 (doar criptare, suficient pentru GCM) + AES-GCM (AEAD). */

typedef struct {
    uint8_t rk[240];   /* chei de rundă expandate */
    int nr;            /* numărul de runde (10 pentru 128, 14 pentru 256) */
} aes_ctx;

void aes_init(aes_ctx *c, const uint8_t *key, int keybits);   /* 128 sau 256 */
void aes_encrypt(const aes_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* AES-GCM. iv are 12 octeti (96 biti, cazul TLS). tag are 16 octeti.
 * decrypt întoarce 0 dacă tag-ul e valid, -1 altfel. */
void aes_gcm_encrypt(const uint8_t *key, int keybits, const uint8_t iv[12],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *plain, size_t plen,
                     uint8_t *cipher, uint8_t tag[16]);
int  aes_gcm_decrypt(const uint8_t *key, int keybits, const uint8_t iv[12],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *cipher, size_t clen,
                     const uint8_t tag[16], uint8_t *plain);
