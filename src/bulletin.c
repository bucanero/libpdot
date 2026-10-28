/* Bulletin Chain flows: authorization check, store, renew and fetch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "bulletin.h"
#include "hash.h"
#include "hex.h"
#include "http.h"
#include "rpc.h"
#include "tx.h"

#define STORAGE_PALLET "TransactionStorage"
#define RENEWAL_PALLET "DataRenewal"

/* TransactionStorage.store(data: Vec<u8>) */
int pdot_bulletin_store_call(pdot_client *c, const pdot_metadata *md,
                             const uint8_t *data, size_t len, pdot_buf *out)
{
    uint8_t pallet, call;
    size_t nfields;

    if (pdot_md_call_fields(md, STORAGE_PALLET, "store", &pallet, &call, NULL, 0, &nfields) ||
        nfields != 1)
        return pdot_fail(c, PDOT_E_METADATA, "metadata has no " STORAGE_PALLET ".store(data)");
    pdot_scale_u8(out, pallet);
    pdot_scale_u8(out, call);
    pdot_scale_bytes(out, data, len);
    return out->err;
}

/* DataRenewal.force_renew(entry: TransactionRef::ContentHash(hash)) */
int pdot_bulletin_renew_call(pdot_client *c, const pdot_metadata *md,
                             const uint8_t content_hash[32], pdot_buf *out)
{
    uint8_t pallet, call, variant;
    uint32_t entry_ty;
    size_t nfields;

    if (pdot_md_call_fields(md, RENEWAL_PALLET, "force_renew", &pallet, &call,
                            &entry_ty, 1, &nfields) || nfields != 1)
        return pdot_fail(c, PDOT_E_METADATA, "metadata has no " RENEWAL_PALLET ".force_renew(entry)");
    if (pdot_md_variant(md, entry_ty, "ContentHash", &variant, &nfields) || nfields != 1)
        return pdot_fail(c, PDOT_E_METADATA, "TransactionRef has no ContentHash variant");
    pdot_scale_u8(out, pallet);
    pdot_scale_u8(out, call);
    pdot_scale_u8(out, variant);
    pdot_scale_raw(out, content_hash, 32);
    return out->err;
}

/* TransactionStorage.authorize_account(who: AccountId32, transactions: u32, bytes: u64) */
int pdot_bulletin_authorize_call(pdot_client *c, const pdot_metadata *md,
                                 const uint8_t who[32], uint32_t transactions,
                                 uint64_t bytes, pdot_buf *out)
{
    uint8_t pallet, call;
    size_t nfields;

    if (pdot_md_call_fields(md, STORAGE_PALLET, "authorize_account", &pallet, &call,
                            NULL, 0, &nfields) || nfields != 3)
        return pdot_fail(c, PDOT_E_METADATA,
                         "metadata has no " STORAGE_PALLET ".authorize_account(who, transactions, bytes)");
    pdot_scale_u8(out, pallet);
    pdot_scale_u8(out, call);
    pdot_scale_raw(out, who, 32);
    pdot_scale_u32(out, transactions);
    pdot_scale_u64(out, bytes);
    return out->err;
}

static int hash_key(pdot_buf *key, uint8_t hasher, const uint8_t *k, size_t len)
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

/* twox128(prefix) | twox128("Authorizations") | hasher(AuthorizationScope::Account(id)) */
int pdot_bulletin_auth_key(pdot_client *c, const pdot_metadata *md,
                           const uint8_t account_id[32], pdot_buf *key)
{
    pdot_md_storage st;
    uint8_t scope[33], h[16];
    size_t nfields;

    if (pdot_md_storage_entry(md, STORAGE_PALLET, "Authorizations", &st) ||
        !st.is_map || st.nhashers != 1)
        return pdot_fail(c, PDOT_E_METADATA, "metadata has no " STORAGE_PALLET ".Authorizations map");
    if (pdot_md_variant(md, st.key_ty, "Account", &scope[0], &nfields) || nfields != 1)
        return pdot_fail(c, PDOT_E_METADATA, "AuthorizationScope has no Account variant");
    memcpy(scope + 1, account_id, 32);

    pdot_twox128(h, st.prefix, st.prefix_len);
    pdot_scale_raw(key, h, 16);
    pdot_twox128(h, "Authorizations", 14);
    pdot_scale_raw(key, h, 16);
    if (hash_key(key, st.hashers[0], scope, sizeof scope))
        return pdot_fail(c, PDOT_E_UNSUPPORTED, "unsupported storage hasher");
    return 0;
}

int pdot_bulletin_is_authorized(pdot_client *c, const uint8_t account_id[32],
                                int *authorized)
{
    pdot_buf key;
    uint8_t *value;
    size_t len;
    int rc;

    if (!c || !account_id || !authorized)
        return PDOT_E_INVALID;
    if ((rc = pdot_chain_sync(c)))
        return rc;
    pdot_buf_init(&key);
    if (!(rc = pdot_bulletin_auth_key(c, c->md, account_id, &key)))
        rc = pdot_rpc_storage(c, key.data, key.len, &value, &len);
    pdot_buf_free(&key);
    if (rc == PDOT_E_NOTFOUND) {
        *authorized = 0;
        return 0;
    }
    if (rc)
        return rc;
    free(value);
    *authorized = 1;
    return 0;
}

/* Builds, signs and submits a call, then waits for finalized inclusion. */
static int submit_call(pdot_client *c, const pdot_signer *s, const pdot_tx_params *p,
                       const pdot_buf *call, pdot_tx_result *out)
{
    pdot_buf xt;
    int rc;

    pdot_buf_init(&xt);
    if (!(rc = pdot_tx_build(c, p, s, call->data, call->len, &xt)))
        rc = pdot_tx_submit_watch(c, p, xt.data, xt.len, out);
    pdot_buf_free(&xt);
    return rc;
}

int pdot_bulletin_store(pdot_client *c, const pdot_signer *s,
                        const uint8_t *data, size_t len, pdot_store_result *out)
{
    pdot_tx_params p;
    pdot_buf call;
    int rc;

    if (!c || !s || (!data && len) || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    pdot_cid_compute(&out->cid, data, len);
    if ((rc = pdot_tx_params_fetch(c, s, &p)))
        return rc;
    pdot_buf_init(&call);
    if (!(rc = pdot_bulletin_store_call(c, p.md, data, len, &call)))
        rc = submit_call(c, s, &p, &call, &out->tx);
    pdot_buf_free(&call);
    return rc;
}

int pdot_bulletin_renew(pdot_client *c, const pdot_signer *s,
                        const pdot_cid *cid, pdot_tx_result *out)
{
    pdot_tx_params p;
    pdot_buf call;
    const uint8_t *digest;
    size_t digest_len;
    uint64_t code;
    int rc;

    if (!c || !s || !cid || !out)
        return PDOT_E_INVALID;
    if (pdot_cid_digest(cid, &code, &digest, &digest_len) || digest_len != 32)
        return pdot_fail(c, PDOT_E_INVALID, "renew needs a CID with a 32-byte digest");
    if ((rc = pdot_tx_params_fetch(c, s, &p)))
        return rc;
    pdot_buf_init(&call);
    if (!(rc = pdot_bulletin_renew_call(c, p.md, digest, &call)))
        rc = submit_call(c, s, &p, &call, out);
    pdot_buf_free(&call);
    return rc;
}

int pdot_bulletin_authorize_account(pdot_client *c, const pdot_signer *s,
                                    const uint8_t who[32], uint32_t transactions,
                                    uint64_t bytes, pdot_tx_result *out)
{
    pdot_tx_params p;
    pdot_buf call;
    int rc;

    if (!c || !s || !who || !out)
        return PDOT_E_INVALID;
    if ((rc = pdot_tx_params_fetch(c, s, &p)))
        return rc;
    pdot_buf_init(&call);
    if (!(rc = pdot_bulletin_authorize_call(c, p.md, who, transactions, bytes, &call)))
        rc = submit_call(c, s, &p, &call, out);
    pdot_buf_free(&call);
    return rc;
}

int pdot_bulletin_fetch(pdot_client *c, const pdot_cid *cid,
                        uint8_t **data, size_t *len)
{
    char cid_str[PDOT_CID_MAX * 2];
    int rc;

    if (!c || !cid || !data || !len)
        return PDOT_E_INVALID;
    *data = NULL;
    *len = 0;
    if ((rc = pdot_cid_to_string(cid, cid_str, sizeof cid_str)))
        return pdot_fail(c, rc, "invalid CID");

    if (c->gateway_url) {
        size_t n = strlen(c->gateway_url) + strlen(cid_str) + 8;
        char *url = malloc(n);
        pdot_buf body;

        if (!url)
            return PDOT_E_NOMEM;
        snprintf(url, n, "%s/ipfs/%s", c->gateway_url, cid_str);
        pdot_buf_init(&body);
        rc = pdot_http_request(c, url, NULL, 0, &body);
        free(url);
        if (rc) {
            pdot_buf_free(&body);
            return rc;
        }
        *data = body.data ? body.data : malloc(1);
        *len = body.len;
        if (!*data)
            return PDOT_E_NOMEM;
    } else {
        cJSON *params = cJSON_CreateArray();
        cJSON_AddItemToArray(params, cJSON_CreateString(cid_str));
        rc = pdot_rpc_bytes(c, "bitswap_v1_get", params, data, len);
        if (rc == PDOT_E_NOTFOUND)
            return pdot_fail(c, rc, "bitswap_v1_get: %s not found", cid_str);
        if (rc)
            return rc;
    }

    if ((rc = pdot_cid_verify(cid, *data, *len))) {
        free(*data);
        *data = NULL;
        *len = 0;
        return pdot_fail(c, rc, rc == PDOT_E_MISMATCH
                         ? "retrieved data does not match %s" : "cannot verify %s", cid_str);
    }
    return 0;
}
