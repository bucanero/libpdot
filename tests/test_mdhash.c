/*
 * RFC-0078 metadata hash against hashes computed by the Rust
 * merkleized-metadata crate:
 *  - Westend and Paseo Bulletin: the hash each runtime embeds at build time
 *    (substrate-wasm-builder), confirmed live with validate_transaction;
 *  - Acala: a fixture of the crate's own test suite, with its ExtraInfo.
 * Then any change to an input must change the hash.
 */
#include <stdlib.h>

#include "pdot.h"
#include "mdhash.h"
#include "test.h"

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static pdot_metadata *parse(const char *path)
{
    size_t len;
    uint8_t *blob = load(path, &len);
    pdot_metadata *md = NULL;

    CHECK(blob != NULL);
    if (blob)
        CHECK(pdot_metadata_parse(blob, len, &md) == 0);
    free(blob);
    return md;
}

static void check_hash(const char *path, pdot_mdhash_info info, int ss58_from_metadata,
                       const char *expected)
{
    pdot_metadata *md = parse(path);
    uint8_t h[32];

    printf("%s\n", path);
    if (!md)
        return;
    if (ss58_from_metadata)
        CHECK(pdot_md_ss58_prefix(md, &info.ss58_prefix) == 0);
    CHECK(pdot_metadata_hash(md, &info, h) == 0);
    CHECK_HEX(h, 32, expected);
    pdot_metadata_free(md);
}

/* d7071996...53d1, the hash the Westend Bulletin runtime embeds. */
static const uint8_t westend_hash[32] = {
    0xd7, 0x07, 0x19, 0x96, 0xb0, 0x1f, 0x0c, 0x49, 0x75, 0x49, 0x63, 0xbb, 0xb3, 0x9a, 0x44, 0x34,
    0x34, 0x84, 0xd7, 0x6a, 0xb4, 0x06, 0x60, 0x61, 0x6f, 0x64, 0x32, 0x9a, 0x73, 0x30, 0x53, 0xd1
};

int main(void)
{
    const pdot_mdhash_info westend = { 1000026, "bulletin-westend", 0, 12, "WND" };
    const pdot_mdhash_info paseo = { 1000026, "bulletin-paseo", 0, 10, "PAS" };
    const pdot_mdhash_info nice = { 1, "nice", 1, 1, "lol" };

    check_hash("metadata-v15-westend.scale", westend, 1,
               "d7071996b01f0c49754963bbb39a44343484d76ab40660616f64329a733053d1");
    check_hash("metadata-v15-paseo.scale", paseo, 1,
               "c3d6e6d784cfa6d9a68fee36fe9394d424b557a828c02f92481350531592e72a");
    check_hash("metadata-v15-acala.scale", nice, 0,
               "bd64dee496517c5288c47014fe0f57c2e12e42a7d627caeafa95e9f992e7e774");

    /* Each digest input changes the hash. */
    {
        pdot_metadata *md = parse("metadata-v15-westend.scale");
        pdot_mdhash_info info = westend;
        uint8_t base[32], h[32];
        uint16_t ss58 = 0;

        CHECK(pdot_md_ss58_prefix(md, &ss58) == 0 && ss58 == 42);
        info.ss58_prefix = ss58;
        CHECK(pdot_metadata_hash(md, &info, base) == 0 && memcmp(base, westend_hash, 32) == 0);

        info.token_symbol = "UNIT";
        CHECK(pdot_metadata_hash(md, &info, h) == 0 && memcmp(h, base, 32) != 0);
        info = westend;
        info.ss58_prefix = ss58;
        info.decimals = 10;
        CHECK(pdot_metadata_hash(md, &info, h) == 0 && memcmp(h, base, 32) != 0);
        info.decimals = 12;
        info.spec_version++;
        CHECK(pdot_metadata_hash(md, &info, h) == 0 && memcmp(h, base, 32) != 0);
        info.spec_version--;
        info.spec_name = "bulletin-paseo";
        CHECK(pdot_metadata_hash(md, &info, h) == 0 && memcmp(h, base, 32) != 0);
        info.spec_name = westend.spec_name;
        CHECK(pdot_metadata_hash(md, &info, h) == 0 && memcmp(h, base, 32) == 0);
        pdot_metadata_free(md);
    }

    /*
     * Forged metadata: the call authorize_account renamed, same length. Docs
     * are not hashed, so rename every length-prefixed name (17 << 2 = 0x44),
     * which includes the call variant.
     */
    {
        size_t len, i, renamed = 0;
        uint8_t *blob = load("metadata-v15-westend.scale", &len), h[32];
        pdot_metadata *md;
        pdot_mdhash_info info = westend;
        static const char from[] = "\x44" "authorize_account";

        info.ss58_prefix = 42;
        CHECK(blob != NULL);
        for (i = 0; blob && i + sizeof from - 1 <= len; i++)
            if (memcmp(blob + i, from, sizeof from - 1) == 0) {
                blob[i + sizeof from - 2] = 'x';
                renamed++;
            }
        CHECK(renamed > 0);
        CHECK(pdot_metadata_parse(blob, len, &md) == 0);
        CHECK(pdot_metadata_hash(md, &info, h) == 0);
        CHECK(memcmp(h, westend_hash, 32) != 0);
        pdot_metadata_free(md);
        free(blob);
    }

    /* v14 metadata has no extrinsic types: the digest cannot be built. */
    {
        pdot_metadata *md = parse("metadata-v14-westend.scale");
        uint8_t h[32];

        if (md)
            CHECK(pdot_metadata_hash(md, &westend, h) == PDOT_E_UNSUPPORTED);
        pdot_metadata_free(md);
    }
    TEST_DONE();
}
