/* Blake2b and XXH64-based twox hashes used by Substrate. */
#ifndef PDOT_HASH_H
#define PDOT_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "blake2.h"

typedef struct {
    blake2b_state state;
    size_t        outlen;
} pdot_blake2b_ctx;

int  pdot_blake2b_init(pdot_blake2b_ctx *ctx, size_t outlen);  /* 1..64 */
void pdot_blake2b_update(pdot_blake2b_ctx *ctx, const void *in, size_t len);
void pdot_blake2b_final(pdot_blake2b_ctx *ctx, uint8_t *out);

void pdot_blake2b(uint8_t *out, size_t outlen, const void *in, size_t len);
void pdot_blake2b_256(uint8_t out[32], const void *in, size_t len);
void pdot_blake2b_128(uint8_t out[16], const void *in, size_t len);

/* twox64/128/256: XXH64 with seeds 0..n-1, each output little-endian. */
void pdot_twox(uint8_t *out, size_t outlen, const void *in, size_t len);
void pdot_twox128(uint8_t out[16], const void *in, size_t len);

/* Storage map key hashers that append the key: hash(key) || key. */
void pdot_blake2_128_concat(uint8_t *out, const void *key, size_t len);
void pdot_twox64_concat(uint8_t *out, const void *key, size_t len);

#endif
