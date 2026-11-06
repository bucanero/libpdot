#include <stdlib.h>
#include <string.h>

#include "ed25519.h"

#include "pdot.h"
#include "hash.h"
#include "scale.h"
#include "grandpa.h"

int pdot_header_decode(const uint8_t *p, size_t len, pdot_header *h)
{
    pdot_reader r;
    const uint8_t *b;
    uint64_t number, n, i;

    pdot_reader_init(&r, p, len);
    if (pdot_read_raw(&r, 32, &b))
        return PDOT_E_DECODE;
    memcpy(h->parent, b, 32);
    if (pdot_read_compact(&r, &number) || number > UINT32_MAX)
        return PDOT_E_DECODE;
    h->number = (uint32_t)number;
    if (pdot_read_raw(&r, 32, &b))
        return PDOT_E_DECODE;
    memcpy(h->state_root, b, 32);
    if (pdot_read_raw(&r, 32, &b))
        return PDOT_E_DECODE;
    memcpy(h->extrinsics_root, b, 32);

    /* Digest: Vec<DigestItem>. */
    if (pdot_read_compact(&r, &n))
        return PDOT_E_DECODE;
    for (i = 0; i < n; i++) {
        uint8_t tag;
        const uint8_t *v;
        size_t vl;

        if (pdot_read_u8(&r, &tag))
            return PDOT_E_DECODE;
        switch (tag) {
        case 0:  /* Other(Vec<u8>) */
            if (pdot_read_bytes(&r, &v, &vl))
                return PDOT_E_DECODE;
            break;
        case 4: case 5: case 6:  /* Consensus, Seal, PreRuntime: ([u8; 4], Vec<u8>) */
            if (pdot_read_skip(&r, 4) || pdot_read_bytes(&r, &v, &vl))
                return PDOT_E_DECODE;
            break;
        case 8:  /* RuntimeEnvironmentUpdated */
            break;
        default:
            return PDOT_E_DECODE;
        }
    }
    h->encoded_len = r.pos;
    pdot_blake2b_256(h->hash, p, r.pos);
    return 0;
}

int pdot_grandpa_set_decode(const uint8_t *p, size_t len, uint64_t set_id, pdot_grandpa_set *set)
{
    pdot_reader r;
    uint64_t n, i;

    memset(set, 0, sizeof *set);
    pdot_reader_init(&r, p, len);
    if (pdot_read_compact(&r, &n) || n == 0 || n > len / 40)
        return PDOT_E_DECODE;
    if (!(set->authorities = calloc((size_t)n, sizeof *set->authorities)))
        return PDOT_E_NOMEM;
    for (i = 0; i < n; i++) {
        const uint8_t *id;
        if (pdot_read_raw(&r, 32, &id) || pdot_read_u64(&r, &set->authorities[i].weight)) {
            pdot_grandpa_set_free(set);
            return PDOT_E_DECODE;
        }
        memcpy(set->authorities[i].id, id, 32);
    }
    set->nauthorities = (size_t)n;
    set->set_id = set_id;
    return 0;
}

int pdot_grandpa_scheduled_change(const uint8_t *header, size_t len,
                                  pdot_grandpa_set *next, uint32_t *delay)
{
    pdot_reader r;
    uint64_t n, i, number;

    pdot_reader_init(&r, header, len);
    if (pdot_read_skip(&r, 32) || pdot_read_compact(&r, &number) || pdot_read_skip(&r, 64) ||
        pdot_read_compact(&r, &n))
        return PDOT_E_DECODE;
    for (i = 0; i < n; i++) {
        uint8_t tag;
        const uint8_t *engine = NULL, *v;
        size_t vl;

        if (pdot_read_u8(&r, &tag))
            return PDOT_E_DECODE;
        if (tag == 8)
            continue;
        if (tag != 0 && pdot_read_raw(&r, 4, &engine))
            return PDOT_E_DECODE;
        if (pdot_read_bytes(&r, &v, &vl))
            return PDOT_E_DECODE;
        /* Consensus(FRNK, ConsensusLog): 1 = ScheduledChange, 2 = ForcedChange. */
        if (tag != 4 || memcmp(engine, "FRNK", 4) != 0 || vl < 1)
            continue;
        if (v[0] == 2)
            return PDOT_E_UNSUPPORTED;
        if (v[0] == 1) {
            pdot_reader cr;
            uint64_t na;
            int rc;

            pdot_reader_init(&cr, v + 1, vl - 1);
            if (pdot_read_compact(&cr, &na) || pdot_read_skip(&cr, (size_t)na * 40) ||
                pdot_read_u32(&cr, delay))
                return PDOT_E_DECODE;
            if ((rc = pdot_grandpa_set_decode(v + 1, cr.pos - 4, 0, next)))
                return rc;
            return 0;
        }
    }
    return PDOT_E_NOTFOUND;
}

void pdot_grandpa_set_free(pdot_grandpa_set *set)
{
    free(set->authorities);
    memset(set, 0, sizeof *set);
}

/* Is `hash` the target, or reachable from it through votes_ancestries? */
static int descends(const uint8_t hash[32], const uint8_t target[32],
                    const pdot_header *anc, size_t nanc)
{
    uint8_t cur[32];
    size_t steps, i;

    memcpy(cur, hash, 32);
    for (steps = 0; steps <= nanc; steps++) {
        if (memcmp(cur, target, 32) == 0)
            return 1;
        for (i = 0; i < nanc && memcmp(anc[i].hash, cur, 32) != 0; i++)
            ;
        if (i == nanc)
            return 0;
        memcpy(cur, anc[i].parent, 32);
    }
    return 0;
}

int pdot_grandpa_verify(const pdot_grandpa_set *set, const uint8_t *j, size_t len,
                        uint8_t target_hash[32], uint32_t *target_number,
                        pdot_grandpa_stats *stats)
{
    pdot_reader r;
    uint64_t round, nprec, nanc, total = 0, i;
    const uint8_t *target, *prec_start;
    uint32_t tnum;
    pdot_header *anc = NULL;
    uint8_t *counted = NULL;
    int rc = PDOT_E_DECODE;

    memset(stats, 0, sizeof *stats);
    pdot_reader_init(&r, j, len);
    if (pdot_read_u64(&r, &round) || pdot_read_raw(&r, 32, &target) ||
        pdot_read_u32(&r, &tnum) || pdot_read_compact(&r, &nprec) ||
        nprec > pdot_reader_remaining(&r) / 132)
        return PDOT_E_DECODE;
    prec_start = r.p + r.pos;
    if (pdot_read_skip(&r, (size_t)nprec * 132))  /* hash 32, number 4, sig 64, id 32 */
        return PDOT_E_DECODE;

    /* votes_ancestries: headers linking each precommit target to the commit target. */
    if (pdot_read_compact(&r, &nanc) || nanc > pdot_reader_remaining(&r) / 100)
        return PDOT_E_DECODE;
    if (nanc && !(anc = calloc((size_t)nanc, sizeof *anc)))
        return PDOT_E_NOMEM;
    for (i = 0; i < nanc; i++) {
        if (pdot_header_decode(r.p + r.pos, pdot_reader_remaining(&r), &anc[i]))
            goto out;
        r.pos += anc[i].encoded_len;
    }
    if (pdot_reader_remaining(&r) != 0)
        goto out;

    for (i = 0; i < set->nauthorities; i++)
        total += set->authorities[i].weight;
    stats->precommits = (size_t)nprec;
    stats->threshold = total - (total - 1) / 3;
    if (!(counted = calloc(set->nauthorities, 1))) {
        rc = PDOT_E_NOMEM;
        goto out;
    }

    for (i = 0; i < nprec; i++) {
        const uint8_t *p = prec_start + i * 132;
        const uint8_t *hash = p, *sig = p + 36, *id = p + 100;
        uint8_t msg[1 + 32 + 4 + 8 + 8];
        size_t a;

        for (a = 0; a < set->nauthorities && memcmp(set->authorities[a].id, id, 32) != 0; a++)
            ;
        if (a == set->nauthorities) {
            rc = PDOT_E_CRYPTO;  /* signer not in the set */
            goto out;
        }
        /* SCALE((Message::Precommit(precommit), round, set_id)) */
        msg[0] = 1;
        memcpy(msg + 1, p, 36);
        {
            size_t k;
            for (k = 0; k < 8; k++) {
                msg[37 + k] = (uint8_t)(round >> (8 * k));
                msg[45 + k] = (uint8_t)(set->set_id >> (8 * k));
            }
        }
        if (ed25519_sign_open(msg, sizeof msg, id, sig) != 0) {
            rc = PDOT_E_CRYPTO;
            goto out;
        }
        if (!descends(hash, target, anc, (size_t)nanc)) {
            rc = PDOT_E_MISMATCH;
            goto out;
        }
        if (!counted[a]) {  /* an equivocating voter counts once */
            counted[a] = 1;
            stats->signers++;
            stats->weight += set->authorities[a].weight;
        }
    }
    if (stats->weight < stats->threshold) {
        rc = PDOT_E_MISMATCH;
        goto out;
    }
    memcpy(target_hash, target, 32);
    *target_number = tnum;
    rc = 0;
out:
    free(anc);
    free(counted);
    return rc;
}
