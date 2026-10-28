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

int main(void)
{
    test_blake2b();
    test_storage_keys();
    TEST_DONE();
}
