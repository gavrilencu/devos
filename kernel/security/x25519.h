#pragma once
#include <stdint.h>

/* X25519 (Curve25519 ECDH) — schimbul de chei pentru TLS.
 * out = scalar * point (32 octeti fiecare). */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* out = scalar * basepoint (cheia publica dintr-o cheie privata) */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);
