/*
 * Light-client verification against proofs recorded from Paseo and Polkadot
 * (tests/fixtures/verify-*.json, made by tools/record-proofs.c): GRANDPA
 * finality, relay header, Paras::Heads, Bulletin storage, and a handover to
 * the next authority set. Then the same proofs, tampered, must fail.
 */
#include <stdlib.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "grandpa.h"
#include "hash.h"
#include "hex.h"
#include "scale.h"
#include "trie.h"
#include "test.h"

typedef struct {
    uint8_t *p;
    size_t   len;
} blob;

static blob hex_field(const cJSON *o, const char *k)
{
    blob b = { NULL, 0 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    if (cJSON_IsString(v))
        pdot_hex_decode_alloc(v->valuestring, &b.p, &b.len);
    return b;
}

static pdot_trie_proof proof_field(const cJSON *o, const char *k)
{
    pdot_trie_proof pf = { NULL, 0 };
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k), *item;

    pf.nodes = calloc((size_t)cJSON_GetArraySize(a) + 1, sizeof *pf.nodes);
    cJSON_ArrayForEach(item, a) {
        uint8_t *d;
        size_t n;
        if (pdot_hex_decode_alloc(item->valuestring, &d, &n) == 0) {
            pf.nodes[pf.n].data = d;
            pf.nodes[pf.n].len = n;
            pf.n++;
        }
    }
    pdot_trie_hash_nodes(pf.nodes, pf.n);
    return pf;
}

static cJSON *load_json(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    cJSON *j;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    buf[n] = '\0';
    fclose(f);
    j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static int set_from(const cJSON *o, pdot_grandpa_set *set)
{
    blob a = hex_field(o, "authorities");
    int rc = pdot_grandpa_set_decode(a.p, a.len,
                                     (uint64_t)cJSON_GetObjectItem(o, "set_id")->valuedouble, set);
    free(a.p);
    return rc;
}

static void run(const char *path, int expect_stored, uint32_t expect_block, uint32_t expect_index)
{
    cJSON *j = load_json(path), *ho;
    pdot_grandpa_set set, prev, next;
    pdot_grandpa_stats st;
    blob just, rhdr, hj, hhdr;
    pdot_header rh, ph, hh;
    pdot_trie_proof heads, stored, retention;
    uint8_t target[32], key[80];
    uint32_t tnum, para_id, delay;
    const uint8_t *v, *hd;
    size_t vl, hdl, i;
    pdot_reader r;
    pdot_cid cid;
    uint64_t code;
    const uint8_t *digest;
    size_t dlen;

    printf("%s\n", path);
    CHECK(j != NULL);
    if (!j)
        return;
    para_id = (uint32_t)cJSON_GetObjectItem(j, "para_id")->valuedouble;

    /* 1. GRANDPA finality under the recorded set. */
    CHECK(set_from(j, &set) == 0);
    just = hex_field(j, "justification");
    CHECK(pdot_grandpa_verify(&set, just.p, just.len, target, &tnum, &st) == 0);
    CHECK(st.weight >= st.threshold && st.signers >= st.threshold);
    printf("  %zu authorities, %zu signed\n", set.nauthorities, st.signers);

    /* 2. Relay header hashes to the final block. */
    rhdr = hex_field(j, "relay_header");
    CHECK(pdot_header_decode(rhdr.p, rhdr.len, &rh) == 0);
    CHECK(memcmp(rh.hash, target, 32) == 0 && rh.number == tnum && rh.encoded_len == rhdr.len);

    /* 3. Paras::Heads(para_id) against the relay state root. */
    {
        uint8_t le[4] = { (uint8_t)para_id, (uint8_t)(para_id >> 8),
                          (uint8_t)(para_id >> 16), (uint8_t)(para_id >> 24) };
        pdot_twox128(key, "Paras", 5);
        pdot_twox128(key + 16, "Heads", 5);
        pdot_twox64_concat(key + 32, le, 4);
    }
    heads = proof_field(j, "paras_heads_proof");
    CHECK(pdot_trie_read(rh.state_root, heads.nodes, heads.n, key, 44, &v, &vl) == 0);
    pdot_reader_init(&r, v, vl);
    memset(&ph, 0, sizeof ph);  /* stays zero if the check below fails */
    CHECK(pdot_read_bytes(&r, &hd, &hdl) == 0 && pdot_header_decode(hd, hdl, &ph) == 0);
    printf("  Bulletin #%u proven final\n", ph.number);

    /* 4. Bulletin storage against the proven parachain state root. */
    pdot_cid_from_string(&cid, cJSON_GetObjectItem(j, "cid")->valuestring);
    pdot_cid_digest(&cid, &code, &digest, &dlen);
    pdot_twox128(key, "TransactionStorage", 18);
    pdot_twox128(key + 16, "TransactionByContentHash", 24);
    pdot_blake2_128_concat(key + 32, digest, 32);
    stored = proof_field(j, "stored_proof");
    if (expect_stored) {
        uint32_t block = 0, index = 0;
        CHECK(pdot_trie_read(ph.state_root, stored.nodes, stored.n, key, 80, &v, &vl) == 0 && vl == 8);
        pdot_reader_init(&r, v, vl);
        pdot_read_u32(&r, &block);
        pdot_read_u32(&r, &index);
        CHECK(block == expect_block && index == expect_index);
        printf("  CID proven stored at #%u, entry %u\n", block, index);
    } else {
        CHECK(pdot_trie_read(ph.state_root, stored.nodes, stored.n, key, 80, &v, &vl) == PDOT_E_NOTFOUND);
        printf("  CID proven absent\n");
    }
    pdot_twox128(key + 16, "RetentionPeriod", 15);
    retention = proof_field(j, "retention_proof");
    if (pdot_trie_read(ph.state_root, retention.nodes, retention.n, key, 32, &v, &vl) == 0) {
        uint32_t period = 0;
        pdot_reader_init(&r, v, vl);
        CHECK(vl == 4 && pdot_read_u32(&r, &period) == 0 && period > 0);
        printf("  retention period %u blocks\n", period);
    }

    /* 5. Handover: the previous set's final justification names this set. */
    ho = cJSON_GetObjectItem(j, "handover");
    CHECK(set_from(ho, &prev) == 0 && prev.set_id + 1 == set.set_id);
    hj = hex_field(ho, "justification");
    hhdr = hex_field(ho, "header");
    CHECK(pdot_grandpa_verify(&prev, hj.p, hj.len, target, &tnum, &st) == 0);
    CHECK(pdot_header_decode(hhdr.p, hhdr.len, &hh) == 0 && memcmp(hh.hash, target, 32) == 0);
    CHECK(pdot_grandpa_scheduled_change(hhdr.p, hhdr.len, &next, &delay) == 0 && delay == 0);
    CHECK(next.nauthorities == set.nauthorities);
    for (i = 0; i < next.nauthorities && i < set.nauthorities; i++)
        CHECK(memcmp(next.authorities[i].id, set.authorities[i].id, 32) == 0);
    pdot_grandpa_set_free(&next);
    printf("  handover %llu -> %llu proven\n", (unsigned long long)prev.set_id,
           (unsigned long long)set.set_id);

    /* ---- Tampering ---- */
    {
        uint8_t *bad = malloc(just.len), root[32];
        pdot_grandpa_set wrong = set;
        pdot_reader cr;
        uint64_t n;
        size_t sig_off;

        pdot_reader_init(&cr, just.p + 44, just.len - 44);
        pdot_read_compact(&cr, &n);
        sig_off = 44 + cr.pos + 36;
        memcpy(bad, just.p, just.len);
        bad[sig_off] ^= 1;
        CHECK(pdot_grandpa_verify(&set, bad, just.len, target, &tnum, &st) == PDOT_E_CRYPTO);
        wrong.set_id++;
        CHECK(pdot_grandpa_verify(&wrong, just.p, just.len, target, &tnum, &st) == PDOT_E_CRYPTO);
        /* The previous set cannot finalize blocks of this one. */
        CHECK(pdot_grandpa_verify(&prev, just.p, just.len, target, &tnum, &st) != 0);
        for (i = 1; i < just.len; i += just.len / 97 + 1)  /* truncations */
            CHECK(pdot_grandpa_verify(&set, just.p, i, target, &tnum, &st) != 0);
        free(bad);

        memcpy(root, rh.state_root, 32);
        root[31] ^= 1;
        CHECK(pdot_trie_read(root, heads.nodes, heads.n, key, 44, &v, &vl) == PDOT_E_MISMATCH);
        /* Every node of a one-key proof is on the path: tampering any must fail. */
        {
            uint8_t k2[44];
            uint8_t le[4] = { (uint8_t)para_id, (uint8_t)(para_id >> 8),
                              (uint8_t)(para_id >> 16), (uint8_t)(para_id >> 24) };
            pdot_twox128(k2, "Paras", 5);
            pdot_twox128(k2 + 16, "Heads", 5);
            pdot_twox64_concat(k2 + 32, le, 4);
            for (i = 0; i < heads.n; i++) {
                uint8_t *d = (uint8_t *)heads.nodes[i].data;
                d[heads.nodes[i].len / 2] ^= 0x10;
                pdot_trie_hash_nodes(heads.nodes, heads.n);
                CHECK(pdot_trie_read(rh.state_root, heads.nodes, heads.n, k2, 44, &v, &vl) != 0);
                d[heads.nodes[i].len / 2] ^= 0x10;
                pdot_trie_hash_nodes(heads.nodes, heads.n);
            }
            /* And with the node untouched again, the value is back. */
            CHECK(pdot_trie_read(rh.state_root, heads.nodes, heads.n, k2, 44, &v, &vl) == 0);
        }
    }

    pdot_trie_proof_free(&heads);
    pdot_trie_proof_free(&stored);
    pdot_trie_proof_free(&retention);
    free(just.p);
    free(rhdr.p);
    free(hj.p);
    free(hhdr.p);
    pdot_grandpa_set_free(&set);
    pdot_grandpa_set_free(&prev);
    cJSON_Delete(j);
}

int main(void)
{
    /* Paseo: the CID stored on Oct 28 and renewed at #2511088 (entry 2). */
    run("verify-paseo.json", 1, 2511088, 2);
    /* Polkadot Bulletin stores nothing yet: the same CID is proven absent. */
    run("verify-polkadot.json", 0, 0, 0);
    TEST_DONE();
}
