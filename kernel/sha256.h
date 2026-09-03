#pragma once
#include <stdint.h>
#include <stddef.h>

/* SHA-256 + HMAC-SHA256 (pentru TLS 1.2: PRF, Finished). */

typedef struct {
    uint32_t h[8];
    uint64_t len;          /* total octeti procesati */
    uint8_t  buf[64];
    int      buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen, uint8_t out[32]);
