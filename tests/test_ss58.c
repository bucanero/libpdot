#include "pdot.h"
#include "test.h"

static const uint8_t alice[32] = {
    0xd4, 0x35, 0x93, 0xc7, 0x15, 0xfd, 0xd3, 0x1c, 0x61, 0x14, 0x1a,
    0xbd, 0x04, 0xa9, 0x9f, 0xd6, 0x82, 0x2c, 0x85, 0x58, 0x85, 0x4c,
    0xcd, 0xe3, 0x9a, 0x56, 0x84, 0xe7, 0xa5, 0x6d, 0xa2, 0x7d,
};

static void test_known_addresses(void)
{
    char addr[PDOT_SS58_MAX_LEN];

    CHECK(pdot_ss58_encode(alice, 42, addr, sizeof addr) == 0);
    CHECK(strcmp(addr, "5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY") == 0);
    CHECK(pdot_ss58_encode(alice, 0, addr, sizeof addr) == 0);
    CHECK(strcmp(addr, "15oF4uVJwmo4TdGW7VfQxNLavjCXviqxT9S1MgbjMNHr6Sp5") == 0);
    CHECK(pdot_ss58_encode(alice, 2, addr, sizeof addr) == 0);
    CHECK(strcmp(addr, "HNZata7iMYWmk5RvZRTiAsSDhV8366zq2YGb3tLH5Upf74F") == 0);
}

static void test_roundtrip(void)
{
    static const uint16_t prefixes[] = { 0, 42, 63, 64, 255, 1284, 16383 };
    char addr[PDOT_SS58_MAX_LEN];
    uint8_t id[32];
    uint16_t prefix;
    size_t i;

    for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        CHECK(pdot_ss58_encode(alice, prefixes[i], addr, sizeof addr) == 0);
        CHECK(pdot_ss58_decode(addr, id, &prefix) == 0);
        CHECK(prefix == prefixes[i]);
        CHECK(memcmp(id, alice, 32) == 0);
    }
}

static void test_errors(void)
{
    char addr[PDOT_SS58_MAX_LEN];
    uint8_t id[32];

    CHECK(pdot_ss58_encode(alice, 16384, addr, sizeof addr) == PDOT_E_INVALID);
    CHECK(pdot_ss58_encode(alice, 42, addr, 10) == PDOT_E_BUFFER);
    /* Last character changed: checksum fails. */
    CHECK(pdot_ss58_decode("5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQZ",
                           id, NULL) == PDOT_E_CHECKSUM);
    CHECK(pdot_ss58_decode("5Grwva0F", id, NULL) == PDOT_E_DECODE);
    CHECK(pdot_ss58_decode("", id, NULL) == PDOT_E_DECODE);
}

int main(void)
{
    test_known_addresses();
    test_roundtrip();
    test_errors();
    TEST_DONE();
}
