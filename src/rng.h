/* Process-wide CTR-DRBG, seeded once from the platform entropy source. */
#ifndef PDOT_RNG_H
#define PDOT_RNG_H

#include <stddef.h>

/* Seeds the DRBG on first use. Returns 0 or PDOT_E_RNG. Thread-safe. */
int pdot_rng_init(void);
int pdot_rng_bytes(void *out, size_t len);

/* mbedTLS f_rng signature, for blinding inside mbedTLS calls. */
int pdot_rng_mbedtls(void *unused, unsigned char *out, size_t len);

/* Zeroes memory in a way the compiler will not drop. */
void pdot_wipe(void *p, size_t len);

#endif
