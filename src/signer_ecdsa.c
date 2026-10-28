/*
 * ECDSA secp256k1 through mbedTLS, matching sp_core::ecdsa:
 *   signature = r (32) | s (32) | recovery id (1), s in the low half,
 *   message hash = Blake2b-256(msg), nonce per RFC 6979,
 *   account id = Blake2b-256(compressed public key).
 */
#include <string.h>

#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>

#include "pdot.h"
#include "hash.h"
#include "rng.h"

/* secret holds the 32-byte private scalar. */

static int load_group(mbedtls_ecp_group *grp)
{
    mbedtls_ecp_group_init(grp);
    return mbedtls_ecp_group_load(grp, MBEDTLS_ECP_DP_SECP256K1);
}

static int write_compressed(const mbedtls_ecp_group *grp,
                            const mbedtls_ecp_point *pt, uint8_t out[33])
{
    size_t olen;

    return mbedtls_ecp_point_write_binary(grp, pt, MBEDTLS_ECP_PF_COMPRESSED,
                                          &olen, out, 33);
}

/*
 * Finds v such that recovering from (r, s, v) gives the signer's key.
 * mbedTLS has no recovery API, so this recomputes
 *   Q = r^-1 (s R - e G)  with R = (r, parity v)
 * for both parities. r >= n - p (v = 2, 3) has probability ~2^-127 and is
 * not handled.
 */
static int recovery_id(const mbedtls_ecp_group *grp, const mbedtls_mpi *r,
                       const mbedtls_mpi *s, const mbedtls_mpi *e,
                       const uint8_t pub[33], uint8_t *v)
{
    mbedtls_mpi rinv, u1, u2;
    mbedtls_ecp_point R, Q;
    uint8_t enc[33];
    int rc, parity;

    mbedtls_mpi_init(&rinv);
    mbedtls_mpi_init(&u1);
    mbedtls_mpi_init(&u2);
    mbedtls_ecp_point_init(&R);
    mbedtls_ecp_point_init(&Q);

    rc = mbedtls_mpi_inv_mod(&rinv, r, &grp->N);
    if (!rc) rc = mbedtls_mpi_mul_mpi(&u1, e, &rinv);           /* e / r */
    if (!rc) rc = mbedtls_mpi_mod_mpi(&u1, &u1, &grp->N);
    if (!rc && mbedtls_mpi_cmp_int(&u1, 0) != 0)
        rc = mbedtls_mpi_sub_mpi(&u1, &grp->N, &u1);           /* -e / r */
    if (!rc) rc = mbedtls_mpi_mul_mpi(&u2, s, &rinv);           /* s / r */
    if (!rc) rc = mbedtls_mpi_mod_mpi(&u2, &u2, &grp->N);

    for (parity = 0; !rc && parity < 2; parity++) {
        enc[0] = (uint8_t)(0x02 | parity);
        if ((rc = mbedtls_mpi_write_binary(r, enc + 1, 32)))
            break;
        if (mbedtls_ecp_point_read_binary(grp, &R, enc, 33) ||
            mbedtls_ecp_check_pubkey(grp, &R))
            continue;
        if ((rc = mbedtls_ecp_muladd((mbedtls_ecp_group *)grp, &Q, &u2, &R,
                                     &u1, &grp->G)))
            break;
        if (write_compressed(grp, &Q, enc) == 0 && memcmp(enc, pub, 33) == 0) {
            *v = (uint8_t)parity;
            goto out;
        }
    }
    if (!rc)
        rc = -1;
out:
    mbedtls_mpi_free(&rinv);
    mbedtls_mpi_free(&u1);
    mbedtls_mpi_free(&u2);
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_point_free(&Q);
    return rc;
}

static int ecdsa_sign_cb(const pdot_signer *s, const uint8_t *msg,
                         size_t len, uint8_t *sig_out)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, sv, e, half;
    uint8_t hash[32];
    int rc;

    pdot_blake2b_256(hash, msg, len);

    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&sv);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&half);

    rc = load_group(&grp);
    if (!rc) rc = mbedtls_mpi_read_binary(&d, s->secret, 32);
    /* RFC 6979 with HMAC-SHA256, as libsecp256k1 does. */
    if (!rc) rc = mbedtls_ecdsa_sign_det_ext(&grp, &r, &sv, &d, hash, 32,
                                             MBEDTLS_MD_SHA256,
                                             pdot_rng_mbedtls, NULL);
    /* Low-s normalization: s > n/2 becomes n - s. */
    if (!rc) rc = mbedtls_mpi_copy(&half, &grp.N);
    if (!rc) rc = mbedtls_mpi_shift_r(&half, 1);
    if (!rc && mbedtls_mpi_cmp_mpi(&sv, &half) > 0)
        rc = mbedtls_mpi_sub_mpi(&sv, &grp.N, &sv);
    if (!rc) rc = mbedtls_mpi_read_binary(&e, hash, 32);
    if (!rc) rc = recovery_id(&grp, &r, &sv, &e, s->public_key, &sig_out[64]);
    if (!rc) rc = mbedtls_mpi_write_binary(&r, sig_out, 32);
    if (!rc) rc = mbedtls_mpi_write_binary(&sv, sig_out + 32, 32);

    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&sv);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&half);
    mbedtls_ecp_group_free(&grp);
    if (rc)
        memset(sig_out, 0, 65);
    return rc ? PDOT_E_CRYPTO : 0;
}

int pdot_signer_ecdsa_from_seed(pdot_signer *s, const uint8_t seed[32])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    int rc;

    if (!s || !seed)
        return PDOT_E_INVALID;
    memset(s, 0, sizeof *s);

    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    rc = load_group(&grp);
    if (!rc) rc = mbedtls_mpi_read_binary(&d, seed, 32);
    /* The seed must be a valid scalar: 0 < d < n. */
    if (!rc) rc = mbedtls_ecp_check_privkey(&grp, &d);
    if (!rc) rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, pdot_rng_mbedtls, NULL);
    if (!rc) rc = write_compressed(&grp, &Q, s->public_key);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    if (rc) {
        pdot_wipe(s, sizeof *s);
        return PDOT_E_CRYPTO;
    }

    memcpy(s->secret, seed, 32);
    s->scheme = PDOT_SCHEME_ECDSA;
    s->public_key_len = 33;
    pdot_blake2b_256(s->account_id, s->public_key, 33);
    s->sig_len = 65;
    s->sign = ecdsa_sign_cb;
    return 0;
}
