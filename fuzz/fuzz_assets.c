/*
 * libFuzzer target: balance, asset and NFT value decoding over hostile
 * storage values, with Polkadot Asset Hub metadata (PDOT_FUZZ_ASSET_METADATA,
 * default tests/fixtures/metadata-v15-asset-hub-polkadot.scale).
 *
 * Input: kind | present bits | first value length (u16 LE) | values. Also
 * formats the first 16 bytes as a 128-bit amount.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "assets.h"
#include "client.h"

static pdot_client *c;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *path = getenv("PDOT_FUZZ_ASSET_METADATA");
    FILE *f;
    uint8_t *blob;
    long n;

    (void)argc;
    (void)argv;
    f = fopen(path ? path : "tests/fixtures/metadata-v15-asset-hub-polkadot.scale", "rb");
    if (!f || !(c = pdot_client_new("http://127.0.0.1:1", NULL)))
        abort();
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n ||
        pdot_metadata_parse(blob, (size_t)n, &c->md))
        abort();
    free(blob);
    fclose(f);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    union {
        pdot_storage_value sv;
        pdot_balance       b;
        pdot_asset_balance ab;
        pdot_asset_info    ai;
        pdot_nft_item      n;
        pdot_nft_attribute na;
    } out;
    pdot_value v[PDOT_Q_MAX_KEYS];
    pdot_final_block at;
    pdot_query q;
    pdot_u128 amount;
    char text[300];
    size_t n0, i;

    if (size < 4)
        return 0;
    memset(&q, 0, sizeof q);
    memset(&at, 0, sizeof at);
    memset(v, 0, sizeof v);
    q.kind = data[0] % (PDOT_Q_NFT_ATTRIBUTE + 1);
    n0 = (size_t)data[2] | (size_t)data[3] << 8;
    if (n0 > size - 4)
        n0 = size - 4;
    /* Fresh copies, so reads past a value's end are caught. */
    v[0].len = n0;
    v[1].len = size - 4 - n0;
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++) {
        v[i].present = data[1] >> i & 1;
        v[i].data = malloc(v[i].len ? v[i].len : 1);
        if (!v[i].data)
            abort();
        memcpy(v[i].data, data + 4 + (i ? n0 : 0), v[i].len);
    }
    if (pdot_query_decode(c, &q, v, &at, &out) == 0 && q.kind == PDOT_Q_STORAGE)
        free(out.sv.data);
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        free(v[i].data);

    if (size >= 20) {
        memcpy(&amount, data + 4, sizeof amount);
        pdot_u128_format(amount, data[1], text, (size_t)data[0] % sizeof text);
        pdot_u128_format(amount, data[1], text, sizeof text);
    }
    return 0;
}
