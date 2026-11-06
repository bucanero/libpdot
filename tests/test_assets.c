/*
 * Balances, pallet-assets and pallet-nfts reads against Polkadot Asset Hub
 * state recorded by tools/record-assets.c: keys derived from the metadata
 * (two compared with keys the node itself listed), looked up in one read
 * proof checked against the header's state root, then decoded. Also
 * 128-bit amount formatting.
 */
#include <stdlib.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "assets.h"
#include "client.h"
#include "grandpa.h"
#include "hex.h"
#include "trie.h"
#include "test.h"
#include "assets_cases.h"

/* Keys as the node listed them (state_getKeysPaged), not as libpdot builds them. */
static const char usdt_holding_key[] =
    "682a59d51ab9e48a8c8cc418ff9708d2b99d880ec681799c0cf30e8886371da9"
    "a319d0e87221ca1ee751c1529f201522c007000000064a5b7db2b0c8271fe85bce78da1e"
    "08622ff9ded7476d40178685d0a0a9d28cd396472dba36eb735f48ca54a0c7d7";
static const char nft_item_key[] =
    "e8d49389c2e23e152fdd6364daadd2cce6636c7091b616c035068763633f33e2"
    "0009f36281d5596534363f86d1162cc1f0020000"
    "0380353fe0c824bfe4038d9b490d395538000000";

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (p = malloc((size_t)n + 1)) && fread(p, 1, (size_t)n, f) == (size_t)n) {
        p[n] = 0;
        *len = (size_t)n;
    } else {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

static void test_format(void)
{
    pdot_u128 max = { UINT64_MAX, UINT64_MAX }, v = { 12345, 0 };
    char out[64];

    CHECK(pdot_u128_format(v, 3, out, sizeof out) == 0 && strcmp(out, "12.345") == 0);
    v.lo = 1000;
    CHECK(pdot_u128_format(v, 3, out, sizeof out) == 0 && strcmp(out, "1") == 0);
    v.lo = 5;
    CHECK(pdot_u128_format(v, 3, out, sizeof out) == 0 && strcmp(out, "0.005") == 0);
    v.lo = 0;
    CHECK(pdot_u128_format(v, 10, out, sizeof out) == 0 && strcmp(out, "0") == 0);
    v.lo = 1200;
    CHECK(pdot_u128_format(v, 0, out, sizeof out) == 0 && strcmp(out, "1200") == 0);
    CHECK(pdot_u128_format(max, 0, out, sizeof out) == 0 &&
          strcmp(out, "340282366920938463463374607431768211455") == 0);
    CHECK(pdot_u128_format(max, 18, out, sizeof out) == 0 &&
          strcmp(out, "340282366920938463463.374607431768211455") == 0);
    CHECK(pdot_u128_format(max, 0, out, 39) == PDOT_E_BUFFER);
    CHECK(pdot_u128_format(max, 0, out, 40) == 0);
    v.lo = 15;
    v.hi = 0;
    CHECK(pdot_u128_to_double(v, 1) == 1.5);
}

typedef struct {
    pdot_client     *c;
    pdot_header      header;
    pdot_trie_proof  proof;
    pdot_final_block at;
} fixture;

static int load(fixture *fx)
{
    size_t len, i;
    uint8_t *text = read_file("assets-polkadot.json", &len), *md_blob, *hdr;
    size_t md_len, hdr_len;
    cJSON *j, *item;
    const cJSON *proof;
    uint8_t hash[32];
    size_t hash_len;

    if (!text || !(md_blob = read_file("metadata-v15-asset-hub-polkadot.scale", &md_len)))
        return -1;
    j = cJSON_Parse((const char *)text);
    free(text);
    fx->c = pdot_client_new("http://127.0.0.1:1", NULL);
    if (!j || !fx->c || pdot_metadata_parse(md_blob, md_len, &fx->c->md)) {
        free(md_blob);
        return -1;
    }
    free(md_blob);
    fx->c->spec_version = (uint32_t)cJSON_GetObjectItem(j, "spec_version")->valuedouble;
    CHECK(pdot_hex_decode_alloc(cJSON_GetObjectItem(j, "header")->valuestring, &hdr, &hdr_len) == 0);
    CHECK(pdot_hex_decode(cJSON_GetObjectItem(j, "block_hash")->valuestring, hash, 32,
                          &hash_len) == 0);
    CHECK(pdot_header_decode(hdr, hdr_len, &fx->header) == 0);
    CHECK(memcmp(fx->header.hash, hash, 32) == 0);  /* the header is that block's */
    free(hdr);

    proof = cJSON_GetObjectItem(j, "proof");
    fx->proof.nodes = calloc((size_t)cJSON_GetArraySize(proof) + 1, sizeof *fx->proof.nodes);
    i = 0;
    cJSON_ArrayForEach(item, proof) {
        uint8_t *d;
        size_t n;
        if (pdot_hex_decode_alloc(item->valuestring, &d, &n) == 0) {
            fx->proof.nodes[i].data = d;
            fx->proof.nodes[i++].len = n;
        }
    }
    fx->proof.n = i;
    pdot_trie_hash_nodes(fx->proof.nodes, fx->proof.n);
    cJSON_Delete(j);

    memset(&fx->at, 0, sizeof fx->at);
    fx->at.number = fx->header.number;
    memcpy(fx->at.hash, fx->header.hash, 32);
    fx->at.verified = 1;
    return 0;
}

/* Builds q's keys, reads them through the proof, and decodes into out. */
static int run(fixture *fx, const pdot_query *q, pdot_buf keys[PDOT_Q_MAX_KEYS], void *out)
{
    pdot_value values[PDOT_Q_MAX_KEYS];
    size_t n, i;
    int rc;

    memset(values, 0, sizeof values);
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_init(&keys[i]);
    if ((rc = pdot_query_keys(fx->c, q, keys, &n)))
        return rc;
    for (i = 0; i < n; i++) {
        const uint8_t *v;
        size_t vl;
        rc = pdot_trie_read(fx->header.state_root, fx->proof.nodes, fx->proof.n,
                            keys[i].data, keys[i].len, &v, &vl);
        if (rc == 0) {
            values[i].data = malloc(vl + 1);
            memcpy(values[i].data, v, vl);
            values[i].len = vl;
            values[i].present = 1;
        } else if (rc != PDOT_E_NOTFOUND) {
            pdot_values_free(values, n);
            return rc;
        }
    }
    rc = pdot_query_decode(fx->c, q, values, &fx->at, out);
    pdot_values_free(values, n);
    return rc;
}

static void free_keys(pdot_buf keys[PDOT_Q_MAX_KEYS])
{
    size_t i;

    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_free(&keys[i]);
}

static void test_reads(fixture *fx)
{
    pdot_query cases[NCASES];
    pdot_buf keys[PDOT_Q_MAX_KEYS];
    uint8_t owner[32];
    pdot_asset_info info;
    pdot_asset_balance hold;
    pdot_balance bal;
    pdot_nft_item nft;
    pdot_nft_attribute attr;
    char text[64];

    CHECK(pdot_ss58_decode(nft_owner_ss58, owner, NULL) == 0);
    assets_cases(cases, owner);

    CHECK(run(fx, &cases[CASE_USDT_INFO], keys, &info) == 0);
    free_keys(keys);
    CHECK(info.exists && info.live && info.sufficient);
    CHECK(strcmp(info.name, "Tether USD") == 0 && strcmp(info.symbol, "USDt") == 0);
    CHECK(info.decimals == 6 && info.accounts > 10000);
    CHECK(info.supply.hi == 0 && info.supply.lo > 1000000000000ull);
    CHECK(info.at.number == fx->header.number && info.at.verified);

    CHECK(run(fx, &cases[CASE_USDT_HOLDING], keys, &hold) == 0);
    CHECK_HEX(keys[0].data, keys[0].len, usdt_holding_key);
    free_keys(keys);
    CHECK(hold.exists && !hold.frozen && hold.balance.lo > 0);

    CHECK(run(fx, &cases[CASE_OWNER_BALANCE], keys, &bal) == 0);
    free_keys(keys);
    CHECK(bal.exists && bal.free.lo > 0);
    CHECK(pdot_u128_format(bal.reserved, 10, text, sizeof text) == 0 &&
          strcmp(text, "30001") == 0);

    CHECK(run(fx, &cases[CASE_NFT], keys, &nft) == 0);
    CHECK_HEX(keys[0].data, keys[0].len, nft_item_key);
    free_keys(keys);
    CHECK(nft.exists && memcmp(nft.owner, owner, 32) == 0 && nft.has_metadata);
    CHECK(nft.metadata_len == 53 &&
          memcmp(nft.metadata, "ipfs://QmbeECLdAtSTmewULSH5fDEVGARnm4kAXgaZdv3W1krRFU", 53) == 0);

    CHECK(run(fx, &cases[CASE_ATTR_ITEM], keys, &attr) == 0);
    free_keys(keys);
    CHECK(attr.exists && attr.len == 3 && memcmp(attr.value, "UZH", 3) == 0);
    CHECK(run(fx, &cases[CASE_ATTR_COLLECTION], keys, &attr) == 0);
    free_keys(keys);
    CHECK(attr.exists && attr.len == 1 && attr.value[0] == '5');

    /* Absence is proven too. */
    CHECK(run(fx, &cases[CASE_NO_ASSET], keys, &info) == 0 && !info.exists);
    free_keys(keys);
    CHECK(run(fx, &cases[CASE_NO_NFT], keys, &nft) == 0 && !nft.exists);
    free_keys(keys);
    CHECK(run(fx, &cases[CASE_NO_HOLDING], keys, &hold) == 0 && !hold.exists);
    free_keys(keys);
}

/* A proof node changed by one byte no longer hashes into the state root. */
static void test_tampered(fixture *fx)
{
    pdot_query cases[NCASES];
    pdot_buf keys[PDOT_Q_MAX_KEYS];
    pdot_asset_info info;
    uint8_t owner[32] = { 0 };
    size_t i;
    int failed = 0;

    assets_cases(cases, owner);
    for (i = 0; i < fx->proof.n; i++) {
        uint8_t *p = (uint8_t *)fx->proof.nodes[i].data;
        size_t at = fx->proof.nodes[i].len - 1;
        int rc;

        p[at] ^= 1;
        rc = run(fx, &cases[CASE_USDT_INFO], keys, &info);
        free_keys(keys);
        p[at] ^= 1;
        if (rc == 0 && info.exists && strcmp(info.symbol, "USDt") == 0)
            continue;  /* that node is not on this key's path */
        failed++;
        CHECK(rc != 0);  /* never a wrong value, only an error */
    }
    CHECK(failed > 0);
}

int main(void)
{
    fixture fx;

    test_format();
    memset(&fx, 0, sizeof fx);
    if (load(&fx) != 0) {
        fprintf(stderr, "cannot load the Asset Hub fixture\n");
        return 1;
    }
    test_reads(&fx);
    test_tampered(&fx);
    pdot_trie_proof_free(&fx.proof);
    pdot_client_free(fx.c);
    TEST_DONE();
}
