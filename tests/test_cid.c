#include "pdot.h"
#include "test.h"

static void test_compute(void)
{
    static const uint8_t data[] = "hello bulletin";
    pdot_cid cid, back;
    char str[128];

    CHECK(pdot_cid_compute(&cid, data, sizeof data - 1) == 0);
    CHECK(cid.len == 38);
    /* CIDv1, raw, blake2b-256 (varint 0xb220), 32-byte digest. */
    CHECK_HEX(cid.bytes, 6, "0155a0e40220");
    CHECK(pdot_cid_verify(&cid, data, sizeof data - 1) == 0);
    CHECK(pdot_cid_verify(&cid, data, sizeof data - 2) == PDOT_E_MISMATCH);

    CHECK(pdot_cid_to_string(&cid, str, sizeof str) == 0);
    CHECK(strncmp(str, "bafk2bzace", 10) == 0);
    CHECK(strlen(str) == 1 + (38 * 8 + 4) / 5);
    CHECK(pdot_cid_to_string(&cid, str, 20) == PDOT_E_BUFFER);

    CHECK(pdot_cid_from_string(&back, str) == 0);
    CHECK(back.len == cid.len && memcmp(back.bytes, cid.bytes, cid.len) == 0);
}

static void test_empty(void)
{
    pdot_cid cid;

    CHECK(pdot_cid_compute(&cid, NULL, 0) == 0);
    CHECK_HEX(cid.bytes + 6, 32,
        "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
}

static void test_parse_errors(void)
{
    pdot_cid cid;

    /* CIDv0 (base58btc "Qm...") and other multibases are not handled. */
    CHECK(pdot_cid_from_string(&cid,
        "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG") == PDOT_E_UNSUPPORTED);
    CHECK(pdot_cid_from_string(&cid, "bafk2bzace!") == PDOT_E_DECODE);
    CHECK(pdot_cid_from_string(&cid, "bafk2bzace") == PDOT_E_DECODE);

    /* sha2-256 CIDv1 parses but cannot be verified without mbedTLS yet. */
    CHECK(pdot_cid_from_string(&cid,
        "bafkreidgvpkjawlxz6sffxzwgooowe5yt7i6wsyg236mfoks77nywkptdq") == 0);
    CHECK_HEX(cid.bytes, 4, "01551220");
    CHECK(pdot_cid_verify(&cid, (const uint8_t *)"", 0) == PDOT_E_UNSUPPORTED);
}

int main(void)
{
    test_compute();
    test_empty();
    test_parse_errors();
    TEST_DONE();
}
