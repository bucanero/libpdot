#include <string.h>

#include "pdot.h"
#include "hash.h"

#define XXH_INLINE_ALL
#define XXH_NO_XXH3
#define XXH_NO_STREAM
#include "xxhash.h"

int pdot_blake2b_init(pdot_blake2b_ctx *ctx, size_t outlen)
{
    if (outlen == 0 || outlen > BLAKE2B_OUTBYTES)
        return PDOT_E_INVALID;
    ctx->outlen = outlen;
    return blake2b_init(&ctx->state, outlen) ? PDOT_E_INVALID : 0;
}

void pdot_blake2b_update(pdot_blake2b_ctx *ctx, const void *in, size_t len)
{
    blake2b_update(&ctx->state, in, len);
}

void pdot_blake2b_final(pdot_blake2b_ctx *ctx, uint8_t *out)
{
    blake2b_final(&ctx->state, out, ctx->outlen);
}

void pdot_blake2b(uint8_t *out, size_t outlen, const void *in, size_t len)
{
    blake2b(out, outlen, in, len, NULL, 0);
}

void pdot_blake2b_256(uint8_t out[32], const void *in, size_t len)
{
    pdot_blake2b(out, 32, in, len);
}

void pdot_blake2b_128(uint8_t out[16], const void *in, size_t len)
{
    pdot_blake2b(out, 16, in, len);
}

void pdot_twox(uint8_t *out, size_t outlen, const void *in, size_t len)
{
    size_t seed, i;

    for (seed = 0; seed < outlen / 8; seed++) {
        XXH64_hash_t h = XXH64(in, len, (XXH64_hash_t)seed);
        for (i = 0; i < 8; i++)
            out[seed * 8 + i] = (uint8_t)(h >> (8 * i));
    }
}

void pdot_twox128(uint8_t out[16], const void *in, size_t len)
{
    pdot_twox(out, 16, in, len);
}

void pdot_blake2_128_concat(uint8_t *out, const void *key, size_t len)
{
    pdot_blake2b_128(out, key, len);
    memcpy(out + 16, key, len);
}

void pdot_twox64_concat(uint8_t *out, const void *key, size_t len)
{
    pdot_twox(out, 8, key, len);
    memcpy(out + 8, key, len);
}
