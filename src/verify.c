#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pdot.h"
#include "hash.h"
#include "rpc.h"
#include "scale.h"
#include "trie.h"
#include "verify.h"

/* Checkpoint blob: "PDCK" | version | genesis | set_id | anchor number | anchor hash | set. */
#define CK_MAGIC   "PDCK"
#define CK_VERSION 1
/* Headers searched backwards for a change signalled with a delay. */
#define MAX_DELAY_WALK 256
/* Authority-set changes followed in one call before giving up. */
#define MAX_SET_WALK 20000
/* How long to wait for a new enough final block. */
#define VERIFY_TIMEOUT_S 300

void pdot_verifier_free(pdot_verifier *v)
{
    if (!v)
        return;
    pdot_client_free(v->relay);
    pdot_grandpa_set_free(&v->set);
    free(v);
}

/* ---- Checkpoints --------------------------------------------------------- */

static int ck_encode(const uint8_t genesis[32], const pdot_grandpa_set *set, uint32_t number,
                     const uint8_t hash[32], uint8_t **blob, size_t *len)
{
    pdot_buf b;
    size_t i;

    pdot_buf_init(&b);
    pdot_scale_raw(&b, (const uint8_t *)CK_MAGIC, 4);
    pdot_scale_u8(&b, CK_VERSION);
    pdot_scale_raw(&b, genesis, 32);
    pdot_scale_u64(&b, set->set_id);
    pdot_scale_u32(&b, number);
    pdot_scale_raw(&b, hash, 32);
    pdot_scale_compact(&b, set->nauthorities);
    for (i = 0; i < set->nauthorities; i++) {
        pdot_scale_raw(&b, set->authorities[i].id, 32);
        pdot_scale_u64(&b, set->authorities[i].weight);
    }
    if (b.err) {
        pdot_buf_free(&b);
        return PDOT_E_NOMEM;
    }
    *blob = b.data;
    *len = b.len;
    return 0;
}

static int ck_decode(const uint8_t *blob, size_t len, pdot_verifier *v)
{
    pdot_reader r;
    const uint8_t *p;
    uint8_t version;
    uint64_t set_id;
    int rc;

    pdot_reader_init(&r, blob, len);
    if (pdot_read_raw(&r, 4, &p) || memcmp(p, CK_MAGIC, 4) != 0 ||
        pdot_read_u8(&r, &version) || version != CK_VERSION ||
        pdot_read_raw(&r, 32, &p))
        return PDOT_E_DECODE;
    memcpy(v->genesis, p, 32);
    if (pdot_read_u64(&r, &set_id) || pdot_read_u32(&r, &v->anchor_number) ||
        pdot_read_raw(&r, 32, &p))
        return PDOT_E_DECODE;
    memcpy(v->anchor_hash, p, 32);
    if ((rc = pdot_grandpa_set_decode(r.p + r.pos, pdot_reader_remaining(&r), set_id, &v->set)))
        return rc;
    return 0;
}

static int call_u64_at(pdot_client *c, const char *fn, const uint8_t at[32], uint64_t *v)
{
    uint8_t *raw;
    size_t len;
    pdot_reader r;
    int rc;

    if ((rc = pdot_rpc_state_call_at(c, fn, NULL, 0, at, &raw, &len)))
        return rc;
    pdot_reader_init(&r, raw, len);
    rc = pdot_read_u64(&r, v) ? pdot_fail(c, PDOT_E_RPC, "%s: bad result", fn) : 0;
    free(raw);
    return rc;
}

int pdot_checkpoint_fetch(const char *relay_url, uint8_t **blob, size_t *len)
{
    return pdot_checkpoint_fetch_at(relay_url, 0, blob, len);
}

int pdot_checkpoint_fetch_at(const char *relay_url, uint32_t block, uint8_t **blob, size_t *len)
{
    pdot_client *c;
    pdot_grandpa_set set;
    uint8_t fin[32], genesis[32], *raw = NULL;
    size_t rlen;
    uint32_t number;
    uint64_t set_id;
    int rc;

    if (!relay_url || !blob || !len)
        return PDOT_E_INVALID;
    if (!(c = pdot_client_new(relay_url, NULL)))
        return PDOT_E_NOMEM;
    memset(&set, 0, sizeof set);
    if (!(rc = pdot_rpc_block_hash(c, 0, genesis)) &&
        !(rc = block ? pdot_rpc_block_hash(c, block, fin) : pdot_rpc_finalized_head(c, fin)) &&
        !(rc = pdot_rpc_block_number(c, fin, &number)) &&
        !(rc = call_u64_at(c, "GrandpaApi_current_set_id", fin, &set_id)) &&
        !(rc = pdot_rpc_state_call_at(c, "GrandpaApi_grandpa_authorities", NULL, 0, fin,
                                      &raw, &rlen)) &&
        !(rc = pdot_grandpa_set_decode(raw, rlen, set_id, &set)))
        rc = ck_encode(genesis, &set, number, fin, blob, len);
    free(raw);
    pdot_grandpa_set_free(&set);
    pdot_client_free(c);
    return rc;
}

int pdot_client_set_verifier(pdot_client *c, const char *relay_url, uint32_t para_id,
                             const uint8_t *checkpoint, size_t len)
{
    pdot_verifier *v;
    int rc;

    if (!c || !relay_url || !checkpoint)
        return PDOT_E_INVALID;
    if (!(v = calloc(1, sizeof *v)))
        return PDOT_E_NOMEM;
    if ((rc = ck_decode(checkpoint, len, v))) {
        pdot_verifier_free(v);
        return pdot_fail(c, rc, "invalid checkpoint");
    }
    if (!(v->relay = pdot_client_new(relay_url, NULL))) {
        pdot_verifier_free(v);
        return PDOT_E_NOMEM;
    }
    v->relay->timeout_ms = c->timeout_ms;
    v->para_id = para_id;
    pdot_verifier_free(c->verifier);
    c->verifier = v;
    return 0;
}

int pdot_client_checkpoint(pdot_client *c, uint8_t **blob, size_t *len)
{
    if (!c || !c->verifier || !blob || !len)
        return PDOT_E_INVALID;
    return ck_encode(c->verifier->genesis, &c->verifier->set, c->verifier->anchor_number,
                     c->verifier->anchor_hash, blob, len);
}

/* ---- Relay finality ------------------------------------------------------ */

/* Fetches a header by hash and checks it hashes to that hash. */
static int proven_header(pdot_client *c, pdot_client *rpc, const uint8_t hash[32],
                         pdot_buf *enc, pdot_header *h)
{
    int rc;

    pdot_buf_init(enc);
    if ((rc = pdot_rpc_header(rpc, hash, enc)) ||
        (rc = pdot_header_decode(enc->data, enc->len, h))) {
        if (rc == PDOT_E_DECODE)
            rc = pdot_fail(c, PDOT_E_UNVERIFIED, "undecodable header");
        else
            pdot_fail(c, rc, "%s", pdot_last_error(rpc));
        pdot_buf_free(enc);
        return rc;
    }
    if (h->encoded_len != enc->len || memcmp(h->hash, hash, 32) != 0) {
        pdot_buf_free(enc);
        return pdot_fail(c, PDOT_E_UNVERIFIED, "header does not hash to its block hash");
    }
    return 0;
}

/*
 * The authority-set change enacted at block T, if any: a ScheduledChange in T
 * itself (delay 0), or with max_walk > 0 in an ancestor `delay` blocks back.
 */
static int change_enacted_at(pdot_client *c, const uint8_t t_hash[32], int max_walk,
                             pdot_grandpa_set *next)
{
    uint8_t hash[32];
    uint32_t t_number = 0, delay;
    int i, rc;

    memcpy(hash, t_hash, 32);
    for (i = 0; i <= max_walk; i++) {
        pdot_buf enc;
        pdot_header h;

        if ((rc = proven_header(c, c->verifier->relay, hash, &enc, &h)))
            return rc;
        if (i == 0)
            t_number = h.number;
        rc = pdot_grandpa_scheduled_change(enc.data, enc.len, next, &delay);
        pdot_buf_free(&enc);
        if (rc == PDOT_E_UNSUPPORTED)
            return pdot_fail(c, PDOT_E_UNVERIFIED,
                             "forced GRANDPA authority change at relay #%u: needs a new checkpoint",
                             h.number);
        if (rc == 0) {
            if (h.number + delay == t_number)
                return 0;
            pdot_grandpa_set_free(next);
        } else if (rc != PDOT_E_NOTFOUND) {
            return pdot_fail(c, PDOT_E_UNVERIFIED, "undecodable GRANDPA digest");
        }
        if (h.number == 0)
            break;
        memcpy(hash, h.parent, 32);
    }
    return PDOT_E_NOTFOUND;
}

/*
 * Proves the newest final relay block reachable from the anchor, advancing
 * the trusted set through every change on the way. With wait 0 the anchor
 * itself is the answer when no newer proof is available yet.
 */
static int relay_final(pdot_client *c, uint8_t hash[32], uint32_t *number, int wait)
{
    pdot_verifier *v = c->verifier;
    time_t deadline = time(NULL) + VERIFY_TIMEOUT_S;
    int walked;

    for (walked = 0; walked < MAX_SET_WALK; walked++) {
        uint8_t *proof, target[32];
        const uint8_t *just;
        size_t plen, jlen;
        uint32_t tnum;
        pdot_grandpa_stats st;
        pdot_grandpa_set next;
        pdot_reader r;
        int rc;

        /* The set's latest justification, or the one ending it. */
        rc = pdot_rpc_prove_finality(v->relay, v->anchor_number + 1, &proof, &plen);
        if (rc == PDOT_E_RPC || rc == PDOT_E_NOTFOUND) {
            if (!wait) {  /* the anchor is the newest final block */
                memcpy(hash, v->anchor_hash, 32);
                *number = v->anchor_number;
                return 0;
            }
            if (time(NULL) > deadline)
                return pdot_fail(c, PDOT_E_TIMEOUT, "no relay finality proof after #%u: %s",
                                 v->anchor_number, pdot_last_error(v->relay));
            if ((rc = pdot_client_wait(c, c->poll_ms)))
                return pdot_fail(c, rc, "cancelled");
            walked--;
            continue;
        }
        if (rc)
            return pdot_fail(c, rc, "relay: %s", pdot_last_error(v->relay));

        /* FinalityProof { block, justification: Vec<u8>, unknown_headers } */
        pdot_reader_init(&r, proof, plen);
        rc = pdot_read_skip(&r, 32) || pdot_read_bytes(&r, &just, &jlen);
        rc = rc ? PDOT_E_DECODE : pdot_grandpa_verify(&v->set, just, jlen, target, &tnum, &st);
        free(proof);
        if (rc) {
            /*
             * Signed by another set: the set may have changed at the anchor
             * through a change signalled with a delay. Only then is the
             * slower search back through ancestors worth it.
             */
            int rc2 = change_enacted_at(c, v->anchor_hash, MAX_DELAY_WALK, &next);
            if (rc2 == 0) {
                next.set_id = v->set.set_id + 1;
                pdot_grandpa_set_free(&v->set);
                v->set = next;
                continue;
            }
            return pdot_fail(c, PDOT_E_UNVERIFIED,
                             "relay finality proof after #%u does not verify under set %llu (%s)",
                             v->anchor_number, (unsigned long long)v->set.set_id,
                             pdot_strerror(rc));
        }
        if (tnum <= v->anchor_number)
            return pdot_fail(c, PDOT_E_UNVERIFIED, "relay finality proof goes backwards");

        rc = change_enacted_at(c, target, 0, &next);
        if (rc == PDOT_E_NOTFOUND) {
            memcpy(hash, target, 32);
            *number = tnum;
            return 0;  /* no change: target is the newest final block of this set */
        }
        if (rc)
            return rc;
        /* The set ends at target; the next one finalizes what follows. */
        next.set_id = v->set.set_id + 1;
        pdot_grandpa_set_free(&v->set);
        v->set = next;
        v->anchor_number = tnum;
        memcpy(v->anchor_hash, target, 32);
    }
    return pdot_fail(c, PDOT_E_UNVERIFIED, "too many authority-set changes since the checkpoint");
}

/* ---- Bulletin head and state --------------------------------------------- */

static void paras_heads_key(uint32_t para_id, uint8_t key[44])
{
    uint8_t le[4] = { (uint8_t)para_id, (uint8_t)(para_id >> 8),
                      (uint8_t)(para_id >> 16), (uint8_t)(para_id >> 24) };

    pdot_twox128(key, "Paras", 5);
    pdot_twox128(key + 16, "Heads", 5);
    pdot_twox64_concat(key + 32, le, 4);
}

static int read_proven(pdot_client *c, pdot_client *rpc, const uint8_t root[32],
                       const uint8_t at[32], const uint8_t *key, size_t key_len,
                       uint8_t **value, size_t *value_len)
{
    pdot_trie_proof proof;
    const uint8_t *v;
    size_t vl;
    int rc;

    if ((rc = pdot_rpc_read_proof(rpc, key, key_len, at, &proof)))
        return pdot_fail(c, rc, "%s", pdot_last_error(rpc));
    rc = pdot_trie_read(root, proof.nodes, proof.n, key, key_len, &v, &vl);
    if (rc == 0) {
        if (!(*value = malloc(vl ? vl : 1)))
            rc = PDOT_E_NOMEM;
        else {
            memcpy(*value, v, vl);
            *value_len = vl;
        }
    } else if (rc != PDOT_E_NOTFOUND) {
        rc = pdot_fail(c, PDOT_E_UNVERIFIED, "read proof does not verify against the state root");
    }
    pdot_trie_proof_free(&proof);
    return rc;
}

/* Proves the parachain head stored in final relay block rhash. */
static int prove_para_head(pdot_client *c, const uint8_t rhash[32], uint32_t rnum)
{
    pdot_verifier *v = c->verifier;
    uint8_t key[44], *value = NULL;
    size_t vlen = 0;
    pdot_buf enc;
    pdot_header rh, ph;
    pdot_reader r;
    const uint8_t *hd;
    size_t hdlen;
    int rc;

    if ((rc = proven_header(c, v->relay, rhash, &enc, &rh)))
        return rc;
    pdot_buf_free(&enc);

    paras_heads_key(v->para_id, key);
    rc = read_proven(c, v->relay, rh.state_root, rhash, key, sizeof key, &value, &vlen);
    if (rc == PDOT_E_NOTFOUND)
        return pdot_fail(c, PDOT_E_UNVERIFIED, "relay chain has no parachain %u", v->para_id);
    if (rc)
        return rc;
    pdot_reader_init(&r, value, vlen);
    rc = pdot_read_bytes(&r, &hd, &hdlen) || pdot_header_decode(hd, hdlen, &ph);
    free(value);
    if (rc)
        return pdot_fail(c, PDOT_E_UNVERIFIED, "parachain head does not decode");
    v->head = ph;
    v->head_relay = rnum;
    v->have_head = 1;
    return 0;
}

int pdot_verify_para_head(pdot_client *c, uint32_t min_number, const pdot_header **head)
{
    pdot_verifier *v = c->verifier;
    time_t deadline = time(NULL) + VERIFY_TIMEOUT_S;

    if (!v)
        return PDOT_E_INVALID;
    for (;;) {
        uint8_t rhash[32];
        uint32_t rnum = 0;
        int rc;

        if (v->have_head && v->head.number >= min_number) {
            *head = &v->head;
            return 0;
        }
        if (v->have_head && time(NULL) > deadline)
            return pdot_fail(c, PDOT_E_TIMEOUT,
                             "no final parachain block at or past #%u (newest proven #%u)",
                             min_number, v->head.number);
        if ((rc = relay_final(c, rhash, &rnum, 1)))
            return rc;
        if (v->have_head && rnum == v->head_relay) {
            if ((rc = pdot_client_wait(c, c->poll_ms)))
                return pdot_fail(c, rc, "cancelled");
            continue;
        }
        if ((rc = prove_para_head(c, rhash, rnum)))
            return rc;
    }
}

int pdot_verify_newest_head(pdot_client *c, const pdot_header **head)
{
    pdot_verifier *v = c->verifier;
    uint8_t rhash[32];
    uint32_t rnum = 0;
    int rc;

    if (!v)
        return PDOT_E_INVALID;
    if ((rc = relay_final(c, rhash, &rnum, 0)))
        return rc;
    if (!(v->have_head && rnum == v->head_relay) && (rc = prove_para_head(c, rhash, rnum)))
        return rc;
    *head = &v->head;
    return 0;
}

int pdot_verify_read(pdot_client *c, const pdot_header *head, const uint8_t *key,
                     size_t key_len, uint8_t **value, size_t *value_len)
{
    *value = NULL;
    *value_len = 0;
    return read_proven(c, c, head->state_root, head->hash, key, key_len, value, value_len);
}

int pdot_verify_read_many(pdot_client *c, const pdot_header *head, const uint8_t *const *keys,
                          const size_t *lens, size_t nkeys, pdot_value *values)
{
    pdot_trie_proof proof;
    size_t i;
    int rc;

    memset(values, 0, nkeys * sizeof *values);
    if ((rc = pdot_rpc_read_proof_keys(c, keys, lens, nkeys, head->hash, &proof)))
        return rc;
    for (i = 0; i < nkeys && !rc; i++) {
        const uint8_t *v;
        size_t vl;

        rc = pdot_trie_read(head->state_root, proof.nodes, proof.n, keys[i], lens[i], &v, &vl);
        if (rc == PDOT_E_NOTFOUND) {
            rc = 0;
        } else if (rc) {
            rc = pdot_fail(c, PDOT_E_UNVERIFIED, "read proof does not verify against the state root");
        } else if (!(values[i].data = malloc(vl ? vl : 1))) {
            rc = PDOT_E_NOMEM;
        } else {
            memcpy(values[i].data, v, vl);
            values[i].len = vl;
            values[i].present = 1;
        }
    }
    pdot_trie_proof_free(&proof);
    if (rc)
        pdot_values_free(values, nkeys);
    return rc;
}
