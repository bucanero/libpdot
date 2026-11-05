/*
 * libFuzzer target: calls and values encoded from hostile JSON with Polkadot
 * Asset Hub metadata (PDOT_FUZZ_ASSET_METADATA, default
 * tests/fixtures/metadata-v15-asset-hub-polkadot.scale).
 *
 * Input: selector byte | JSON text. The selector picks a call, or with its
 * top bit a type id taken from the next two bytes, to encode the JSON as.
 * Encoded calls are then built into an extrinsic with ChargeAssetTxPayment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "encode.h"
#include "metadata.h"
#include "tx.h"

static pdot_metadata *md;
static pdot_signer signer;

static const char *const calls[][2] = {
    { "Balances", "transfer_keep_alive" }, { "Balances", "transfer_all" },
    { "Assets", "transfer_keep_alive" },   { "Assets", "create" },
    { "Assets", "set_metadata" },          { "Nfts", "create" },
    { "Nfts", "mint" },                    { "Nfts", "transfer" },
    { "Nfts", "set_attribute" },           { "Proxy", "add_proxy" },
    { "Proxy", "remove_proxy" },           { "Utility", "batch_all" },
    { "System", "remark" },                { "ForeignAssets", "transfer_keep_alive" },
    { "PolkadotXcm", "transfer_assets" },  { "Proxy", "proxy" },
};

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *path = getenv("PDOT_FUZZ_ASSET_METADATA");
    static const uint8_t seed[32] = { 1 };
    FILE *f;
    uint8_t *blob;
    long n;

    (void)argc;
    (void)argv;
    f = fopen(path ? path : "tests/fixtures/metadata-v15-asset-hub-polkadot.scale", "rb");
    if (!f)
        abort();
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n ||
        pdot_metadata_parse(blob, (size_t)n, &md) || pdot_signer_sr25519_from_seed(&signer, seed))
        abort();
    free(blob);
    fclose(f);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char err[200], *text;
    pdot_buf b, xt;
    cJSON *v;
    size_t skip = 1;
    int rc;

    if (size < 1 || !(text = malloc(size + 1)))
        return 0;
    if ((data[0] & 0x80) && size >= 3)
        skip = 3;
    memcpy(text, data + skip, size - skip);
    text[size - skip] = '\0';
    v = cJSON_Parse(text);
    free(text);

    pdot_buf_init(&b);
    pdot_buf_init(&xt);
    if (skip == 3) {
        uint32_t ty = ((uint32_t)data[1] | (uint32_t)data[2] << 8) % md->ntypes;
        if (v)
            (void)pdot_encode_json(md, ty, v, &b, err, sizeof err);
    } else {
        size_t k = data[0] % (sizeof calls / sizeof calls[0]);
        rc = pdot_encode_call_json(md, calls[k][0], calls[k][1], v, &b, err, sizeof err);
        if (rc == 0) {
            pdot_tx_params p;
            memset(&p, 0, sizeof p);
            p.md = md;
            p.era_period = 64;
            p.tip.lo = data[0];
            (void)pdot_tx_build(NULL, &p, &signer, b.data, b.len, &xt);
        } else if (!memchr(err, '\0', sizeof err)) {
            abort();  /* messages are always terminated */
        }
    }
    pdot_buf_free(&b);
    pdot_buf_free(&xt);
    cJSON_Delete(v);
    return 0;
}
