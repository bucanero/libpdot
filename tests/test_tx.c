/*
 * Extrinsic construction against subxt-core goldens (tests/fixtures/tx_golden.h).
 * ECDSA signing is deterministic, so whole extrinsics compare byte for byte.
 * The fixed inputs match tools/fixtures-gen/src/main.rs.
 */
#include <stdlib.h>

#include "pdot.h"
#include "bulletin.h"
#include "metadata.h"
#include "tx.h"
#include "test.h"
#include "fixtures/signer_vectors.h"
#include "fixtures/tx_golden.h"

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void fixed_params(pdot_tx_params *p, const pdot_metadata *md)
{
    memset(p, 0, sizeof *p);
    p->md = md;
    memset(p->genesis, 0x11, 32);
    p->spec_version = 1000026;
    p->tx_version = 1;
    p->nonce = 7;
    p->era_period = 64;
    p->era_block = 1234;
    memset(p->era_block_hash, 0x22, 32);
}

static void test_era(void)
{
    /* Values from sp-runtime's Era::mortal tests. */
    CHECK(pdot_era_mortal(64, 42) == 0x02a5);       /* encodes as a5 02 */
    CHECK(pdot_era_mortal(64, 1234) == (5 | (18 << 4)));
    CHECK(pdot_era_mortal(32768, 20000) == 0x9c4e); /* quantized phase: 4e 9c */
    CHECK(pdot_era_mortal(4, 0) == 0x01);
    CHECK(pdot_era_mortal(100000, 0) == 0x0f);      /* clamped to 65536 */
}

static void test_golden(const pdot_metadata *md)
{
    pdot_signer s;
    pdot_tx_params p;
    pdot_buf call, xt;
    uint8_t data[300];
    size_t i;

    CHECK(pdot_signer_ecdsa_from_seed(&s, ecdsa_vectors[0].seed) == 0);
    fixed_params(&p, md);

    for (i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 13);
    pdot_buf_init(&call);
    pdot_buf_init(&xt);
    CHECK(pdot_bulletin_store_call(NULL, md, data, sizeof data, &call) == 0);
    CHECK(pdot_tx_build(NULL, &p, &s, call.data, call.len, &xt) == 0);
    CHECK_HEX(xt.data, xt.len, golden_store);
    pdot_buf_free(&call);
    pdot_buf_free(&xt);

    pdot_buf_init(&call);
    pdot_buf_init(&xt);
    memset(data, 0x33, 32);
    CHECK(pdot_bulletin_renew_call(NULL, md, data, &call) == 0);
    CHECK_HEX(call.data, call.len,
              "2a01" "01" "3333333333333333333333333333333333333333333333333333333333333333");
    CHECK(pdot_tx_build(NULL, &p, &s, call.data, call.len, &xt) == 0);
    CHECK_HEX(xt.data, xt.len, golden_force_renew);
    pdot_buf_free(&call);
    pdot_buf_free(&xt);
    pdot_signer_wipe(&s);
}

static void test_auth_key(const pdot_metadata *md)
{
    pdot_buf key;

    pdot_buf_init(&key);
    CHECK(pdot_bulletin_auth_key(NULL, md, sr25519_vectors[0].public_key, &key) == 0);
    CHECK_HEX(key.data, key.len, golden_auth_key_alice);
    pdot_buf_free(&key);
}

/* sr25519 signatures are randomized: check structure, not bytes. */
static void test_sr25519_shape(const pdot_metadata *md)
{
    pdot_signer s;
    pdot_tx_params p;
    pdot_buf call, xt;
    static const uint8_t small[3] = { 1, 2, 3 };

    CHECK(pdot_signer_sr25519_from_seed(&s, sr25519_vectors[0].seed) == 0);
    fixed_params(&p, md);
    pdot_buf_init(&call);
    pdot_buf_init(&xt);
    CHECK(pdot_bulletin_store_call(NULL, md, small, sizeof small, &call) == 0);
    CHECK(pdot_tx_build(NULL, &p, &s, call.data, call.len, &xt) == 0);
    /* compact len | 84 | 00 account | 01 sig(64) | era nonce tip mode | call */
    CHECK(xt.len == 2 + 1 + 33 + 65 + 2 + 1 + 1 + 1 + call.len);
    CHECK(xt.data[2] == 0x84 && xt.data[3] == 0x00);
    CHECK(memcmp(xt.data + 4, sr25519_vectors[0].public_key, 32) == 0);
    CHECK(xt.data[36] == 0x01);
    CHECK(memcmp(xt.data + xt.len - call.len, call.data, call.len) == 0);
    pdot_buf_free(&call);
    pdot_buf_free(&xt);
    pdot_signer_wipe(&s);
}

int main(void)
{
    static const char *files[] = {
        "metadata-v15-paseo.scale", "metadata-v14-paseo.scale",
    };
    size_t i;

    test_era();
    /* v14 must give the same bytes as v15: it lacks the v15 address and signature types. */
    for (i = 0; i < 2; i++) {
        pdot_metadata *md = NULL;
        size_t len;
        uint8_t *blob = load(files[i], &len);

        CHECK(blob && pdot_metadata_parse(blob, len, &md) == 0);
        free(blob);
        if (!md)
            continue;
        test_golden(md);
        test_auth_key(md);
        test_sr25519_shape(md);
        pdot_metadata_free(md);
    }
    TEST_DONE();
}
