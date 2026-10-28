/*
 * Signer tests against sp-core vectors (tests/fixtures/signer_vectors.h).
 *
 * With PDOT_SIGLOG=<file> set, every signature libpdot makes is written
 * there as "<scheme> <public> <message> <signature>" in hex, so sp-core can
 * check the other direction: tools/fixtures-gen verify < <file>.
 */
#include <stdlib.h>

#include "pdot.h"
#include "hash.h"
#include "signer.h"
#include "test.h"
#include "fixtures/signer_vectors.h"

static FILE *siglog;

static void log_hex(const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        fprintf(siglog, "%02x", p[i]);
}

static void log_sig(const char *scheme, const pdot_signer *s,
                    const uint8_t *msg, size_t len, const uint8_t *sig)
{
    if (!siglog)
        return;
    fprintf(siglog, "%s ", scheme);
    log_hex(s->public_key, s->public_key_len);
    fputc(' ', siglog);
    if (len == 0)
        fputc('-', siglog);  /* keeps four fields; fixtures-gen reads it as "" */
    log_hex(msg, len);
    fputc(' ', siglog);
    log_hex(sig, s->sig_len);
    fputc('\n', siglog);
}

static void test_sr25519(void)
{
    size_t v, m;

    for (v = 0; v < sizeof sr25519_vectors / sizeof sr25519_vectors[0]; v++) {
        const struct sr25519_vector *tv = &sr25519_vectors[v];
        pdot_signer s;
        uint8_t sig[64], sig2[64];

        CHECK(pdot_signer_sr25519_from_seed(&s, tv->seed) == 0);
        CHECK(s.scheme == PDOT_SCHEME_SR25519 && s.sig_len == 64);
        CHECK(memcmp(s.public_key, tv->public_key, 32) == 0);
        CHECK(memcmp(s.account_id, tv->public_key, 32) == 0);

        for (m = 0; m < VEC_NMSGS; m++) {
            const uint8_t *msg = vec_msgs[m].p;
            size_t len = vec_msgs[m].len;

            /* sp-core signature verifies here. */
            CHECK(pdot_sr25519_verify(tv->public_key, msg, len, tv->sigs[m]) == 1);

            /* Our signature verifies here; sp-core checks it via PDOT_SIGLOG. */
            CHECK(pdot_signer_sign(&s, msg, len, sig) == 0);
            CHECK(pdot_sr25519_verify(s.public_key, msg, len, sig) == 1);
            log_sig("sr25519", &s, msg, len, sig);

            /* Signing is randomized. */
            CHECK(pdot_signer_sign(&s, msg, len, sig2) == 0);
            CHECK(memcmp(sig, sig2, 64) != 0);

            /* Tampering fails. */
            sig[5] ^= 1;
            CHECK(pdot_sr25519_verify(s.public_key, msg, len, sig) == 0);
            if (len > 0)
                CHECK(pdot_sr25519_verify(s.public_key, msg, len - 1, tv->sigs[m]) == 0);
        }
        pdot_signer_wipe(&s);
        CHECK(s.sign == NULL && s.secret[0] == 0);
    }
}

static void test_ecdsa(void)
{
    static const uint8_t zero[32];
    /* The secp256k1 group order n: not a valid secret. */
    static const uint8_t order[32] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48,
        0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41,
    };
    size_t v, m;
    pdot_signer s;

    for (v = 0; v < sizeof ecdsa_vectors / sizeof ecdsa_vectors[0]; v++) {
        const struct ecdsa_vector *tv = &ecdsa_vectors[v];
        uint8_t sig[65], account[32];

        CHECK(pdot_signer_ecdsa_from_seed(&s, tv->seed) == 0);
        CHECK(s.scheme == PDOT_SCHEME_ECDSA && s.sig_len == 65);
        CHECK(s.public_key_len == 33);
        CHECK(memcmp(s.public_key, tv->public_key, 33) == 0);

        /* sp-core: AccountId32 = blake2_256(compressed public key). */
        pdot_blake2b_256(account, tv->public_key, 33);
        CHECK(memcmp(s.account_id, account, 32) == 0);

        /* RFC 6979 on both sides: byte-for-byte identical, including v. */
        for (m = 0; m < VEC_NMSGS; m++) {
            CHECK(pdot_signer_sign(&s, vec_msgs[m].p, vec_msgs[m].len, sig) == 0);
            if (memcmp(sig, tv->sigs[m], 65) != 0) {
                fprintf(stderr, "  ecdsa %s msg %u differs\n", tv->uri, (unsigned)m);
                CHECK_HEX(sig, 65, "");
                test_failures++;
            }
            log_sig("ecdsa", &s, vec_msgs[m].p, vec_msgs[m].len, sig);
        }
        pdot_signer_wipe(&s);
    }

    CHECK(pdot_signer_ecdsa_from_seed(&s, zero) == PDOT_E_CRYPTO);
    CHECK(pdot_signer_ecdsa_from_seed(&s, order) == PDOT_E_CRYPTO);
}

static void test_alice_addresses(void)
{
    char addr[PDOT_SS58_MAX_LEN];
    pdot_signer s;

    CHECK(pdot_signer_sr25519_from_seed(&s, sr25519_vectors[0].seed) == 0);
    CHECK(pdot_ss58_encode(s.account_id, 42, addr, sizeof addr) == 0);
    CHECK(strcmp(addr, "5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY") == 0);

    CHECK(pdot_signer_ecdsa_from_seed(&s, ecdsa_vectors[0].seed) == 0);
    CHECK(pdot_ss58_encode(s.account_id, 42, addr, sizeof addr) == 0);
    CHECK(strcmp(addr, "5C7C2Z5sWbytvHpuLTvzKunnnRwQxft1jiqrLD5rhucQ5S9X") == 0);
    pdot_signer_wipe(&s);
}

int main(void)
{
    const char *path = getenv("PDOT_SIGLOG");

    if (path && !(siglog = fopen(path, "w"))) {
        perror(path);
        return 1;
    }
    test_sr25519();
    test_ecdsa();
    test_alice_addresses();
    if (siglog)
        fclose(siglog);
    TEST_DONE();
}
