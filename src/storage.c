#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hash.h"
#include "storage.h"
#include "verify.h"

int pdot_storage_hash(pdot_buf *key, uint8_t hasher, const uint8_t *k, size_t len)
{
    uint8_t h[32];

    switch (hasher) {
    case PDOT_HASHER_BLAKE2_128:        pdot_blake2b_128(h, k, len); pdot_scale_raw(key, h, 16); break;
    case PDOT_HASHER_BLAKE2_256:        pdot_blake2b_256(h, k, len); pdot_scale_raw(key, h, 32); break;
    case PDOT_HASHER_BLAKE2_128_CONCAT: pdot_blake2b_128(h, k, len); pdot_scale_raw(key, h, 16);
                                        pdot_scale_raw(key, k, len); break;
    case PDOT_HASHER_TWOX128:           pdot_twox(h, 16, k, len); pdot_scale_raw(key, h, 16); break;
    case PDOT_HASHER_TWOX256:           pdot_twox(h, 32, k, len); pdot_scale_raw(key, h, 32); break;
    case PDOT_HASHER_TWOX64_CONCAT:     pdot_twox(h, 8, k, len); pdot_scale_raw(key, h, 8);
                                        pdot_scale_raw(key, k, len); break;
    case PDOT_HASHER_IDENTITY:          pdot_scale_raw(key, k, len); break;
    default:                            return PDOT_E_UNSUPPORTED;
    }
    return key->err;
}

int pdot_storage_key(pdot_client *c, const pdot_metadata *md, const char *pallet,
                     const char *entry, const pdot_key_part *parts, size_t nparts,
                     pdot_md_storage *st, pdot_buf *key)
{
    uint8_t h[16];
    size_t i;

    if (pdot_md_storage_entry(md, pallet, entry, st))
        return pdot_fail(c, PDOT_E_METADATA, "metadata has no %s.%s", pallet, entry);
    if (nparts > (st->is_map ? st->nhashers : 0))
        return pdot_fail(c, PDOT_E_METADATA, "%s.%s has fewer keys than expected", pallet, entry);
    pdot_twox128(h, st->prefix, st->prefix_len);
    pdot_scale_raw(key, h, 16);
    pdot_twox128(h, entry, strlen(entry));
    pdot_scale_raw(key, h, 16);
    for (i = 0; i < nparts; i++)
        if (pdot_storage_hash(key, st->hashers[i], parts[i].p, parts[i].len))
            return pdot_fail(c, PDOT_E_UNSUPPORTED, "%s.%s: unsupported storage hasher",
                             pallet, entry);
    return key->err;
}

int pdot_final_block_get(pdot_client *c, pdot_final_block *at, const pdot_header **head)
{
    int rc;

    memset(at, 0, sizeof *at);
    *head = NULL;
    if (c->verifier) {
        if ((rc = pdot_verify_newest_head(c, head)))
            return rc;
        at->number = (*head)->number;
        memcpy(at->hash, (*head)->hash, 32);
        at->verified = 1;
        at->relay_block = c->verifier->head_relay;
        return 0;
    }
    if ((rc = pdot_rpc_finalized_head(c, at->hash)) ||
        (rc = pdot_rpc_block_number(c, at->hash, &at->number)))
        return rc;
    return 0;
}

int pdot_storage_read_at(pdot_client *c, const pdot_final_block *at, const pdot_header *head,
                         const uint8_t *const *keys, const size_t *lens, size_t nkeys,
                         pdot_value *values)
{
    if (head)
        return pdot_verify_read_many(c, head, keys, lens, nkeys, values);
    return pdot_rpc_query_storage_at(c, keys, lens, nkeys, at->hash, values);
}

int pdot_storage_get(pdot_client *c, const uint8_t *key, size_t key_len,
                     pdot_storage_value *out)
{
    const pdot_header *head;
    pdot_value v;
    int rc;

    if (!c || !key || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if ((rc = pdot_final_block_get(c, &out->at, &head)) ||
        (rc = pdot_storage_read_at(c, &out->at, head, &key, &key_len, 1, &v)))
        return rc;
    out->exists = v.present;
    out->data = v.data;
    out->len = v.len;
    return 0;
}
