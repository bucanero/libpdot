/* sr25519 through sr25519-donna, with SHA-512 and randomness from mbedTLS. */
#include <string.h>

#include "sr25519.h"

#include "pdot.h"
#include "rng.h"
#include "signer.h"

/* secret holds the sr25519-donna keypair: key (32) | nonce (32) | public (32). */

static int sr25519_sign_cb(const pdot_signer *s, const uint8_t *msg,
                           size_t len, uint8_t *sig_out)
{
    if (pdot_rng_init())
        return PDOT_E_RNG;
    sr25519_sign(sig_out, s->secret + 64, s->secret, msg, (unsigned long)len);
    return 0;
}

int pdot_signer_sr25519_from_seed(pdot_signer *s, const uint8_t seed[32])
{
    if (!s || !seed)
        return PDOT_E_INVALID;
    memset(s, 0, sizeof *s);
    sr25519_keypair_from_seed(s->secret, seed);
    s->scheme = PDOT_SCHEME_SR25519;
    memcpy(s->public_key, s->secret + 64, 32);
    s->public_key_len = 32;
    memcpy(s->account_id, s->public_key, 32);
    s->sig_len = 64;
    s->sign = sr25519_sign_cb;
    return 0;
}

int pdot_sr25519_verify(const uint8_t public_key[32], const uint8_t *msg,
                        size_t len, const uint8_t sig[64])
{
    return sr25519_verify(sig, msg, (unsigned long)len, public_key) ? 1 : 0;
}
