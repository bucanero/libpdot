#include "pdot.h"
#include "hash.h"
#include "test.h"

static const uint8_t alice[32] = {
    0xd4, 0x35, 0x93, 0xc7, 0x15, 0xfd, 0xd3, 0x1c, 0x61, 0x14, 0x1a,
    0xbd, 0x04, 0xa9, 0x9f, 0xd6, 0x82, 0x2c, 0x85, 0x58, 0x85, 0x4c,
    0xcd, 0xe3, 0x9a, 0x56, 0x84, 0xe7, 0xa5, 0x6d, 0xa2, 0x7d,
};

static void test_blake2b(void)
{
    uint8_t out[64];
    pdot_blake2b_ctx ctx;

    /* RFC 7693 Appendix A. */
    pdot_blake2b(out, 64, "abc", 3);
    CHECK_HEX(out, 64,
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
        "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");

    pdot_blake2b_256(out, "", 0);
    CHECK_HEX(out, 32,
        "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");

    /* Streaming matches one-shot. */
    CHECK(pdot_blake2b_init(&ctx, 64) == 0);
    pdot_blake2b_update(&ctx, "a", 1);
    pdot_blake2b_update(&ctx, "bc", 2);
    pdot_blake2b_final(&ctx, out);
    CHECK_HEX(out, 4, "ba80a53f");

    CHECK(pdot_blake2b_init(&ctx, 0) == PDOT_E_INVALID);
    CHECK(pdot_blake2b_init(&ctx, 65) == PDOT_E_INVALID);
}

static void test_storage_keys(void)
{
    uint8_t out[48];

    pdot_twox128(out, "System", 6);
    CHECK_HEX(out, 16, "26aa394eea5630e07c48ae0c9558cef7");
    pdot_twox128(out, "Account", 7);
    CHECK_HEX(out, 16, "b99d880ec681799c0cf30e8886371da9");
    pdot_twox128(out, "Timestamp", 9);
    CHECK_HEX(out, 16, "f0c365c3cf59d671eb72da0e7a4113c4");

    /* System.Account key suffix for //Alice. */
    pdot_blake2_128_concat(out, alice, 32);
    CHECK_HEX(out, 48,
        "de1e86a9a8c739864cf3cc5ec2bea59f"
        "d43593c715fdd31c61141abd04a99fd6822c8558854ccde39a5684e7a56da27d");
}

/* BLAKE3 test_vectors.json: input byte i is i % 251; lengths around chunk edges. */
static void test_blake3(void)
{
    static const struct {
        size_t      len;
        const char *hash;
    } v[] = {
        { 0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262" },
        { 1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213" },
        { 1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7" },
        { 1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444" },
        { 8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b" },
    };
    static uint8_t in[8193];
    uint8_t out[32];
    size_t i;

    for (i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i % 251);
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        pdot_blake3(out, in, v[i].len);
        CHECK_HEX(out, 32, v[i].hash);
    }
}

int main(void)
{
    test_blake2b();
    test_blake3();
    test_storage_keys();
    TEST_DONE();
}
