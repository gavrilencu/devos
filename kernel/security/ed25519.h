/* Ed25519 (EdDSA) — semnare + derivarea cheii publice din seed. */
#ifndef ED25519_H
#define ED25519_H
#include <stdint.h>

/* Deriva cheia publica (32 octeti) din seed-ul privat (32 octeti). */
void ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]);

/* Semneaza mesajul m (n octeti) cu seed-ul privat si cheia publica pk;
 * produce o semnatura detasata de 64 octeti (R||S). */
void ed25519_sign(uint8_t sig[64], const uint8_t *m, unsigned long long n,
                  const uint8_t seed[32], const uint8_t pk[32]);

#endif
