/* Internal signer helpers, used by tests and later by tx.c. */
#ifndef PDOT_SIGNER_H
#define PDOT_SIGNER_H

#include <stddef.h>
#include <stdint.h>

/* 1 if sig is a valid sr25519 signature (context "substrate"), else 0. */
int pdot_sr25519_verify(const uint8_t public_key[32], const uint8_t *msg,
                        size_t len, const uint8_t sig[64]);

#endif
