/*
 * Records Polkadot Asset Hub state for tests/test_assets.c:
 *
 *   record_assets ASSET_HUB_RPC OUT.json METADATA_OUT.scale
 *
 * Saves the runtime metadata, the newest final header, and one read proof
 * covering every key of the cases in tests/assets_cases.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "assets.h"
#include "client.h"
#include "hex.h"
#include "jsonrpc.h"
#include "trie.h"
#include "../tests/assets_cases.h"

static cJSON *hex_item(const uint8_t *p, size_t n)
{
    char *s = malloc(2 * n + 3);
    cJSON *j;

    pdot_hex_encode(p, n, s);
    j = cJSON_CreateString(s);
    free(s);
    return j;
}

int main(int argc, char **argv)
{
    pdot_client *c;
    pdot_query cases[NCASES];
    pdot_buf keys[NCASES][PDOT_Q_MAX_KEYS], header;
    const uint8_t *kp[NCASES * PDOT_Q_MAX_KEYS];
    size_t kl[NCASES * PDOT_Q_MAX_KEYS], nkeys = 0, n, i, k;
    uint8_t hash[32], owner[32];
    pdot_trie_proof proof;
    cJSON *root, *arr;
    char *text;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: %s ASSET_HUB_RPC OUT.json METADATA_OUT.scale\n", argv[0]);
        return 2;
    }
    if (!(c = pdot_client_new(argv[1], NULL)) || pdot_chain_sync(c) ||
        pdot_ss58_decode(nft_owner_ss58, owner, NULL)) {
        fprintf(stderr, "error: %s\n", c ? pdot_last_error(c) : "no client");
        return 1;
    }
    assets_cases(cases, owner);
    for (i = 0; i < NCASES; i++) {
        for (k = 0; k < PDOT_Q_MAX_KEYS; k++)
            pdot_buf_init(&keys[i][k]);
        if (pdot_query_keys(c, &cases[i], keys[i], &n)) {
            fprintf(stderr, "case %zu: %s\n", i, pdot_last_error(c));
            return 1;
        }
        for (k = 0; k < n; k++) {
            kp[nkeys] = keys[i][k].data;
            kl[nkeys++] = keys[i][k].len;
        }
    }
    pdot_buf_init(&header);
    if (pdot_rpc_finalized_head(c, hash) || pdot_rpc_header(c, hash, &header) ||
        pdot_rpc_read_proof_keys(c, kp, kl, nkeys, hash, &proof)) {
        fprintf(stderr, "error: %s\n", pdot_last_error(c));
        return 1;
    }

    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "block_hash", hex_item(hash, 32));
    cJSON_AddItemToObject(root, "header", hex_item(header.data, header.len));
    cJSON_AddNumberToObject(root, "spec_version", c->spec_version);
    arr = cJSON_AddArrayToObject(root, "proof");
    for (i = 0; i < proof.n; i++)
        cJSON_AddItemToArray(arr, hex_item(proof.nodes[i].data, proof.nodes[i].len));
    text = cJSON_Print(root);
    if (!(f = fopen(argv[2], "w")) || fputs(text, f) < 0 || fclose(f)) {
        perror(argv[2]);
        return 1;
    }
    if (!(f = fopen(argv[3], "wb")) || fwrite(c->md->blob, 1, c->md->len, f) != c->md->len ||
        fclose(f)) {
        perror(argv[3]);
        return 1;
    }
    printf("recorded %zu keys, %zu proof nodes, spec %u\n", nkeys, proof.n, c->spec_version);
    cJSON_free(text);
    cJSON_Delete(root);
    pdot_trie_proof_free(&proof);
    pdot_buf_free(&header);
    pdot_client_free(c);
    return 0;
}
