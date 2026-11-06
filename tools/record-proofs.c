/*
 * Records live proofs into a JSON fixture for tests/test_verify.c:
 *
 *   record_proofs RELAY_RPC PARA_RPC PARA_ID CID OUT.json
 *   record_proofs --add-tx FIXTURE.json PARA_RPC
 *
 * CID must be stored on the Bulletin chain. The "tx" section is a recent
 * transaction for proven results: a block a few below the proven parachain
 * head with a signed extrinsic, the headers back to it, its body and a proof
 * of its System.Events. --add-tx records only that section into a fixture,
 * for the head its relay header already proves (needs an archive node). The relay node is trusted only
 * here, to fetch the authority sets the fixture is checked against; the test
 * then verifies every proof offline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "client.h"
#include "grandpa.h"
#include "hash.h"
#include "hex.h"
#include "jsonrpc.h"
#include "scale.h"
#include "trie.h"

static void add_hex(cJSON *o, const char *k, const uint8_t *p, size_t n)
{
    char *h = malloc(2 * n + 3);

    pdot_hex_encode(p, n, h);
    cJSON_AddStringToObject(o, k, h);
    free(h);
}

/* Keeps a FinalityProof's justification; its unknown_headers are never used. */
static void add_justification(cJSON *o, const uint8_t *proof, size_t len)
{
    pdot_reader r;
    const uint8_t *j;
    size_t jl;

    pdot_reader_init(&r, proof, len);
    if (pdot_read_skip(&r, 32) == 0 && pdot_read_bytes(&r, &j, &jl) == 0)
        add_hex(o, "justification", j, jl);
}

static void add_proof(cJSON *o, const char *k, const pdot_trie_proof *pf)
{
    cJSON *a = cJSON_AddArrayToObject(o, k);
    size_t i;

    for (i = 0; i < pf->n; i++) {
        char *h = malloc(2 * pf->nodes[i].len + 3);
        pdot_hex_encode(pf->nodes[i].data, pf->nodes[i].len, h);
        cJSON_AddItemToArray(a, cJSON_CreateString(h));
        free(h);
    }
}

#define TRY(x) do { if ((rc = (x)) != 0) { fprintf(stderr, "%s: %s (%s)\n", #x, pdot_strerror(rc), pdot_last_error(relay)); return 1; } } while (0)

/* Set id and authorities at a relay block. */
static int set_at(pdot_client *c, const uint8_t at[32], cJSON *o)
{
    uint8_t *raw;
    size_t len;
    pdot_reader r;
    uint64_t id;

    if (pdot_rpc_state_call_at(c, "GrandpaApi_current_set_id", NULL, 0, at, &raw, &len))
        return -1;
    pdot_reader_init(&r, raw, len);
    pdot_read_u64(&r, &id);
    free(raw);
    cJSON_AddNumberToObject(o, "set_id", (double)id);
    if (pdot_rpc_state_call_at(c, "GrandpaApi_grandpa_authorities", NULL, 0, at, &raw, &len))
        return -1;
    add_hex(o, "authorities", raw, len);
    free(raw);
    return 0;
}

/* Encoded header of block `hash`, as a hex string in arr. */
static int add_header(pdot_client *para, const uint8_t hash[32], cJSON *arr, pdot_header *h)
{
    pdot_buf enc;
    char *hx;

    pdot_buf_init(&enc);
    if (pdot_rpc_header(para, hash, &enc) || pdot_header_decode(enc.data, enc.len, h)) {
        pdot_buf_free(&enc);
        return -1;
    }
    hx = malloc(2 * enc.len + 3);
    pdot_hex_encode(enc.data, enc.len, hx);
    cJSON_AddItemToArray(arr, cJSON_CreateString(hx));
    free(hx);
    pdot_buf_free(&enc);
    return 0;
}

/*
 * The "tx" section: of the 32 blocks below head, the one with the smallest
 * body that has a signed extrinsic past the inherents; its last extrinsic.
 */
static int record_tx(pdot_client *para, const pdot_header *head, cJSON *out)
{
    cJSON *tx, *headers, *body = NULL, *best = NULL;
    uint32_t n, best_n = 0;
    size_t best_size = (size_t)-1;
    uint8_t h[32], key[32];
    pdot_header hd = *head;
    pdot_trie_proof pf;

    for (n = head->number - 1; n + 32 >= head->number && n > 0; n--) {
        cJSON *item;
        size_t size = 0;
        if (pdot_rpc_block_hash(para, n, h) || pdot_rpc_block_extrinsics(para, h, &body))
            return -1;
        cJSON_ArrayForEach(item, body)
            size += strlen(item->valuestring);
        if (cJSON_GetArraySize(body) > 2 && size < best_size) {
            cJSON_Delete(best);
            best = body;
            best_n = n;
            best_size = size;
        } else {
            cJSON_Delete(body);
        }
    }
    if (!best)
        return -1;
    tx = cJSON_AddObjectToObject(out, "tx");
    cJSON_AddNumberToObject(tx, "number", best_n);
    cJSON_AddNumberToObject(tx, "index", cJSON_GetArraySize(best) - 1);
    headers = cJSON_AddArrayToObject(tx, "headers");  /* head down to the block */
    if (add_header(para, head->hash, headers, &hd))
        return -1;
    while (hd.number > best_n)
        if (add_header(para, hd.parent, headers, &hd))
            return -1;
    cJSON_AddItemToObject(tx, "body", best);
    pdot_twox128(key, "System", 6);
    pdot_twox128(key + 16, "Events", 6);
    if (pdot_rpc_read_proof(para, key, sizeof key, hd.hash, &pf)) {
        fprintf(stderr, "events proof: %s\n", pdot_last_error(para));
        return -1;
    }
    add_proof(tx, "events_proof", &pf);
    pdot_trie_proof_free(&pf);
    printf("tx: Bulletin #%u extrinsic %d, %d headers from #%u\n", best_n,
           cJSON_GetArraySize(best) - 1, cJSON_GetArraySize(headers), head->number);
    return 0;
}

static int write_json(const cJSON *out, const char *path)
{
    char *s = cJSON_Print(out);
    FILE *f = fopen(path, "w");

    if (!f || fputs(s, f) < 0) {
        free(s);
        if (f)
            fclose(f);
        return 1;
    }
    fputc('\n', f);
    fclose(f);
    free(s);
    printf("wrote %s\n", path);
    return 0;
}

/* The parachain head proven by a fixture's relay header and Paras::Heads proof. */
static int fixture_head(const cJSON *j, pdot_header *ph)
{
    const cJSON *rh_hex = cJSON_GetObjectItem(j, "relay_header"), *item;
    uint8_t *rh_raw, key[44];
    size_t rh_len, i = 0;
    pdot_header rh;
    pdot_trie_proof pf;
    const uint8_t *val, *hd;
    size_t vlen, hdlen;
    pdot_reader r;
    uint32_t id = (uint32_t)cJSON_GetObjectItem(j, "para_id")->valuedouble;
    uint8_t le[4] = { (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24) };
    int rc;

    if (pdot_hex_decode_alloc(rh_hex->valuestring, &rh_raw, &rh_len) ||
        pdot_header_decode(rh_raw, rh_len, &rh))
        return -1;
    free(rh_raw);
    pf.n = (size_t)cJSON_GetArraySize(cJSON_GetObjectItem(j, "paras_heads_proof"));
    pf.nodes = calloc(pf.n, sizeof *pf.nodes);
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(j, "paras_heads_proof")) {
        uint8_t *d;
        pdot_hex_decode_alloc(item->valuestring, &d, &pf.nodes[i].len);
        pf.nodes[i++].data = d;
    }
    pdot_trie_hash_nodes(pf.nodes, pf.n);
    pdot_twox128(key, "Paras", 5);
    pdot_twox128(key + 16, "Heads", 5);
    pdot_twox64_concat(key + 32, le, 4);
    rc = pdot_trie_read(rh.state_root, pf.nodes, pf.n, key, sizeof key, &val, &vlen);
    if (!rc) {
        pdot_reader_init(&r, val, vlen);
        rc = pdot_read_bytes(&r, &hd, &hdlen) || pdot_header_decode(hd, hdlen, ph);
    }
    pdot_trie_proof_free(&pf);
    return rc ? -1 : 0;
}

/* --add-tx FIXTURE PARA_RPC */
static int add_tx(const char *path, const char *para_rpc)
{
    FILE *f = fopen(path, "rb");
    pdot_client *para = pdot_client_new(para_rpc, NULL);
    cJSON *j = NULL;
    pdot_header ph;
    char *text;
    long n;
    int rc = 1;

    if (!f || !para)
        return 1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = calloc(1, (size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n)
        j = cJSON_Parse(text);
    fclose(f);
    free(text);
    if (j && fixture_head(j, &ph) == 0) {
        cJSON_DeleteItemFromObject(j, "tx");
        if (record_tx(para, &ph, j) == 0)
            rc = write_json(j, path);
    } else {
        fprintf(stderr, "%s: not a fixture with a provable parachain head\n", path);
    }
    cJSON_Delete(j);
    pdot_client_free(para);
    return rc;
}

int main(int argc, char **argv)
{
    pdot_client *relay, *para;
    uint8_t fin[32], target[32], key[44], *proof, *raw;
    size_t plen, len;
    uint32_t number;
    pdot_buf enc;
    pdot_header rh, ph;
    pdot_trie_proof pf;
    pdot_cid cid;
    const uint8_t *val, *hd, *digest;
    size_t vlen, hdlen, dlen;
    uint64_t code;
    pdot_reader r;
    cJSON *out, *ho;
    int rc;

    if (argc == 4 && strcmp(argv[1], "--add-tx") == 0)
        return add_tx(argv[2], argv[3]);
    if (argc != 6) {
        fprintf(stderr, "usage: %s RELAY_RPC PARA_RPC PARA_ID CID OUT.json\n"
                        "       %s --add-tx FIXTURE.json PARA_RPC\n", argv[0], argv[0]);
        return 2;
    }
    relay = pdot_client_new(argv[1], NULL);
    para = pdot_client_new(argv[2], NULL);
    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "para_id", atoi(argv[3]));
    cJSON_AddStringToObject(out, "cid", argv[4]);
    if (pdot_cid_from_string(&cid, argv[4]) || pdot_cid_digest(&cid, &code, &digest, &dlen))
        return 2;

    /* Finality of the newest relay block, and the set that signs it. */
    TRY(pdot_rpc_finalized_head(relay, fin));
    TRY(pdot_rpc_block_number(relay, fin, &number));
    if (set_at(relay, fin, out))
        return 1;
    TRY(pdot_rpc_prove_finality(relay, number, &proof, &plen));
    add_justification(out, proof, plen);
    memcpy(target, proof, 32);
    free(proof);

    pdot_buf_init(&enc);
    TRY(pdot_rpc_header(relay, target, &enc));
    TRY(pdot_header_decode(enc.data, enc.len, &rh));
    add_hex(out, "relay_header", enc.data, enc.len);
    pdot_buf_free(&enc);

    {
        uint32_t id = (uint32_t)atoi(argv[3]);
        uint8_t le[4] = { (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24) };
        pdot_twox128(key, "Paras", 5);
        pdot_twox128(key + 16, "Heads", 5);
        pdot_twox64_concat(key + 32, le, 4);
    }
    TRY(pdot_rpc_read_proof(relay, key, sizeof key, target, &pf));
    TRY(pdot_trie_read(rh.state_root, pf.nodes, pf.n, key, sizeof key, &val, &vlen));
    add_proof(out, "paras_heads_proof", &pf);
    pdot_reader_init(&r, val, vlen);
    pdot_read_bytes(&r, &hd, &hdlen);
    TRY(pdot_header_decode(hd, hdlen, &ph));
    pdot_trie_proof_free(&pf);

    /* Bulletin state at that head: where CID is stored, and the retention period. */
    {
        uint8_t k[16 + 16 + 16 + 32], h[16];
        pdot_twox128(k, "TransactionStorage", 18);
        pdot_twox128(k + 16, "TransactionByContentHash", 24);
        pdot_blake2_128_concat(k + 32, digest, 32);
        if ((rc = pdot_rpc_read_proof(para, k, sizeof k, ph.hash, &pf)) != 0) {
            fprintf(stderr, "para read proof: %s\n", pdot_last_error(para));
            return 1;
        }
        add_proof(out, "stored_proof", &pf);
        pdot_trie_proof_free(&pf);
        pdot_twox128(h, "RetentionPeriod", 15);
        memcpy(k + 16, h, 16);
        if (pdot_rpc_read_proof(para, k, 32, ph.hash, &pf))
            return 1;
        add_proof(out, "retention_proof", &pf);
        pdot_trie_proof_free(&pf);
    }

    /* A handover: the previous set's final justification and its header. */
    ho = cJSON_AddObjectToObject(out, "handover");
    {
        uint64_t cur = (uint64_t)cJSON_GetObjectItem(out, "set_id")->valuedouble, sid = cur;
        uint32_t at = number;
        uint8_t h[32];
        do {  /* step back to a block in the previous set */
            at -= 600;
            TRY(pdot_rpc_block_hash(relay, at, h));
            TRY(pdot_rpc_state_call_at(relay, "GrandpaApi_current_set_id", NULL, 0, h, &raw, &len));
            pdot_reader_init(&r, raw, len);
            pdot_read_u64(&r, &sid);
            free(raw);
        } while (sid == cur && number - at < 40000);
        if (sid + 1 != cur || set_at(relay, h, ho))
            return 1;
        /* Blocks after `at` are final under that set; its last justification ends it. */
        TRY(pdot_rpc_prove_finality(relay, at + 1, &proof, &plen));
        add_justification(ho, proof, plen);
        memcpy(target, proof, 32);
        free(proof);
        pdot_buf_init(&enc);
        TRY(pdot_rpc_header(relay, target, &enc));
        add_hex(ho, "header", enc.data, enc.len);
        pdot_buf_free(&enc);
    }

    if (record_tx(para, &ph, out))
        return 1;
    rc = write_json(out, argv[5]);
    cJSON_Delete(out);
    pdot_client_free(relay);
    pdot_client_free(para);
    return rc;
}
