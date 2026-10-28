/*
 * Force-included into every sr25519-donna translation unit (-include).
 * Supplies the SR25519_CUSTOMHASH and SR25519_CUSTOMRANDOM hooks, so the
 * submodule's own placeholder headers stay untouched.
 */
#ifndef PDOT_SR25519_HOOKS_H
#define PDOT_SR25519_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/sha512.h>

typedef mbedtls_sha512_context sr25519_hash_context;

static inline void sr25519_hash_init(sr25519_hash_context *ctx)
{
    mbedtls_sha512_init(ctx);
    mbedtls_sha512_starts(ctx, 0);
}

static inline void sr25519_hash_update(sr25519_hash_context *ctx,
                                       const uint8_t *in, size_t inlen)
{
    mbedtls_sha512_update(ctx, in, inlen);
}

static inline void sr25519_hash_final(sr25519_hash_context *ctx, uint8_t *hash)
{
    mbedtls_sha512_finish(ctx, hash);
    mbedtls_sha512_free(ctx);
}

static inline void sr25519_hash(uint8_t *hash, const uint8_t *in, size_t inlen)
{
    mbedtls_sha512(in, inlen, hash, 0);
}

/*
 * Signing entropy. If the DRBG fails the buffer stays zeroed; schnorrkel
 * still derives the nonce from the secret nonce and the full transcript,
 * so it remains unique per (key, message). signer_sr25519.c checks the
 * DRBG is seeded before it signs.
 */
int pdot_rng_bytes(void *out, size_t len);

static inline void sr25519_randombytes(void *p, size_t len)
{
    size_t i;

    if (pdot_rng_bytes(p, len) != 0)
        for (i = 0; i < len; i++)
            ((uint8_t *)p)[i] = 0;
}

#endif
