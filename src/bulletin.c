/* Bulletin Chain flows: authorization check, store, renew and fetch. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "bulletin.h"
#include "dag.h"
#include "decode.h"
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

/*
 * TransactionStorage.store_with_cid_config(cid: CidConfig { codec, hashing }, data),
 * with Blake2b-256 hashing. CidConfig is encoded field by field from metadata.
 */
int pdot_bulletin_store_cid_call(pdot_client *c, const pdot_metadata *md, uint64_t codec,
                                 const uint8_t *data, size_t len, pdot_buf *out)
{
    uint8_t pallet, call, prim, variant;
    uint32_t args[2];
    size_t nfields, ncfg = 0;
    pdot_md_iter it;
    pdot_md_field f;
    int rc;

    if (pdot_md_call_fields(md, STORAGE_PALLET, "store_with_cid_config", &pallet, &call,
                            args, 2, &nfields) || nfields != 2 ||
        pdot_md_iter_fields(md, args[0], &it))
        return pdot_fail(c, PDOT_E_METADATA,
                         "metadata has no " STORAGE_PALLET ".store_with_cid_config(cid, data)");
    pdot_scale_u8(out, pallet);
    pdot_scale_u8(out, call);
    while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
        if (pdot_md_name_eq(f.name, f.name_len, "codec") &&
            pdot_md_primitive(md, f.ty, &prim) == 0 && prim == 6 /* u64 */) {
            pdot_scale_u64(out, codec);
        } else if (pdot_md_name_eq(f.name, f.name_len, "hashing") &&
                   pdot_md_variant(md, f.ty, "Blake2b256", &variant, &nfields) == 0 &&
                   nfields == 0) {
            pdot_scale_u8(out, variant);
        } else {
            return pdot_fail(c, PDOT_E_METADATA, "unexpected CidConfig field %.*s",
                             (int)f.name_len, f.name);
        }
        ncfg++;
    }
    if (rc < 0 || ncfg != 2)
        return pdot_fail(c, PDOT_E_METADATA, "CidConfig is not { codec, hashing }");
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

int pdot_bulletin_authorization(pdot_client *c, const uint8_t account_id[32],
                                pdot_authorization *out)
{
    static const struct { const char *path; int wide; size_t off; } fields[] = {
        { "extent.transactions", 0, offsetof(pdot_authorization, transactions) },
        { "extent.transactions_allowance", 0, offsetof(pdot_authorization, transactions_allowance) },
        { "extent.bytes", 1, offsetof(pdot_authorization, bytes) },
        { "extent.bytes_allowance", 1, offsetof(pdot_authorization, bytes_allowance) },
        { "expiration", 0, offsetof(pdot_authorization, expiration) },
    };
    pdot_md_storage st;
    pdot_buf key;
    uint8_t *value;
    size_t len, i;
    int rc;

    if (!c || !account_id || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if ((rc = pdot_chain_sync(c)) || (rc = pdot_rpc_best_number(c, &out->now)))
        return rc;
    pdot_buf_init(&key);
    if (!(rc = pdot_bulletin_auth_key(c, c->md, account_id, &key)))
        rc = pdot_rpc_storage(c, key.data, key.len, &value, &len);
    pdot_buf_free(&key);
    if (rc == PDOT_E_NOTFOUND)
        return 0;
    if (rc)
        return rc;

    pdot_md_storage_entry(c->md, STORAGE_PALLET, "Authorizations", &st);
    for (i = 0; i < sizeof fields / sizeof fields[0] && !rc; i++) {
        uint64_t v;
        if ((rc = pdot_decode_path_uint(c->md, st.value_ty, value, len, fields[i].path, &v)))
            rc = pdot_fail(c, rc, "cannot decode Authorization.%s", fields[i].path);
        else if (fields[i].wide)
            *(uint64_t *)((char *)out + fields[i].off) = v;
        else
            *(uint32_t *)((char *)out + fields[i].off) = (uint32_t)v;
    }
    free(value);
    if (!rc)
        out->exists = 1;
    return rc;
}

int pdot_bulletin_is_authorized(pdot_client *c, const uint8_t account_id[32],
                                int *authorized)
{
    pdot_authorization a;
    int rc;

    if (!authorized)
        return PDOT_E_INVALID;
    if ((rc = pdot_bulletin_authorization(c, account_id, &a)))
        return rc;
    *authorized = a.exists && a.now < a.expiration;
    return 0;
}

/* Builds, signs and submits a call, then waits for finalized inclusion. */
static int submit_call(pdot_client *c, const pdot_signer *s, const pdot_tx_params *p,
                       const pdot_buf *call, pdot_tx_result *out, pdot_ext_outcome *oc)
{
    pdot_buf xt;
    int rc;

    pdot_buf_init(&xt);
    if (!(rc = pdot_tx_build(c, p, s, call->data, call->len, &xt)))
        rc = pdot_tx_submit_watch(c, p, xt.data, xt.len, out, oc);
    pdot_buf_free(&xt);
    return rc;
}

/* store, or store_with_cid_config for a codec other than raw. */
static int store_codec(pdot_client *c, const pdot_signer *s, uint64_t codec,
                       const uint8_t *data, size_t len, pdot_store_result *out)
{
    pdot_tx_params p;
    pdot_buf call;
    int rc;

    if (!c || !s || (!data && len) || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    pdot_cid_compute_codec(&out->cid, codec, data, len);
    if ((rc = pdot_tx_params_fetch(c, s, &p)))
        return rc;
    pdot_buf_init(&call);
    rc = codec == PDOT_CODEC_RAW
        ? pdot_bulletin_store_call(c, p.md, data, len, &call)
        : pdot_bulletin_store_cid_call(c, p.md, codec, data, len, &call);
    if (!rc) {
        pdot_ext_outcome oc;
        memset(&oc, 0, sizeof oc);
        rc = submit_call(c, s, &p, &call, &out->tx, &oc);
        out->has_storage_index = oc.has_stored;
        out->storage_index = oc.stored_index;
    }
    pdot_buf_free(&call);
    return rc;
}

int pdot_bulletin_store(pdot_client *c, const pdot_signer *s,
                        const uint8_t *data, size_t len, pdot_store_result *out)
{
    return store_codec(c, s, PDOT_CODEC_RAW, data, len, out);
}

int pdot_bulletin_store_chunked(pdot_client *c, const pdot_signer *s,
                                const uint8_t *data, size_t len, size_t chunk_size,
                                pdot_progress_fn progress, void *ud,
                                pdot_chunked_result *out)
{
    pdot_dag_link *links;
    pdot_store_result r;
    pdot_buf manifest;
    size_t n, i;
    int rc = 0;

    if (!c || !s || !data || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if (chunk_size == 0)
        chunk_size = PDOT_CHUNK_SIZE_DEFAULT;
    if (len == 0 || len > PDOT_FILE_SIZE_MAX || chunk_size > PDOT_CHUNK_SIZE_MAX)
        return pdot_fail(c, PDOT_E_INVALID, "need 1 byte to %u MiB of data and chunks of at most %u MiB",
                         PDOT_FILE_SIZE_MAX >> 20, PDOT_CHUNK_SIZE_MAX >> 20);
    n = (len + chunk_size - 1) / chunk_size;
    if (!(links = calloc(n, sizeof *links)))
        return PDOT_E_NOMEM;

    for (i = 0; i < n && !rc; i++) {
        size_t off = i * chunk_size, clen = len - off < chunk_size ? len - off : chunk_size;
        rc = store_codec(c, s, PDOT_CODEC_RAW, data + off, clen, &r);
        if (!rc) {
            links[i].cid = r.cid;
            links[i].size = clen;
            out->nchunks = i + 1;
        }
        if (progress)
            progress(ud, i, n + 1, &r.cid, &r.tx, rc);
    }
    if (!rc) {
        pdot_buf_init(&manifest);
        if (!(rc = pdot_dag_encode(links, n, &manifest))) {
            rc = store_codec(c, s, PDOT_CODEC_DAG_PB, manifest.data, manifest.len,
                             &out->manifest);
            out->root = out->manifest.cid;
            if (progress)
                progress(ud, n, n + 1, &out->manifest.cid, &out->manifest.tx, rc);
        }
        pdot_buf_free(&manifest);
    }
    out->size = len;
    free(links);
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
        rc = submit_call(c, s, &p, &call, out, NULL);
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
        rc = submit_call(c, s, &p, &call, out, NULL);
    pdot_buf_free(&call);
    return rc;
}

/*
 * One block by CID, verified against it. Gateways are asked for the raw block
 * (?format=raw), so a manifest comes back as itself, not as the assembled file.
 */
static int fetch_block(pdot_client *c, const pdot_cid *cid, uint8_t **data, size_t *len)
{
    char cid_str[PDOT_CID_MAX * 2];
    int rc;

    *data = NULL;
    *len = 0;
    if ((rc = pdot_cid_to_string(cid, cid_str, sizeof cid_str)))
        return pdot_fail(c, rc, "invalid CID");

    if (c->gateway_url) {
        size_t n = strlen(c->gateway_url) + strlen(cid_str) + 24;
        char *url = malloc(n);
        pdot_buf body;

        if (!url)
            return PDOT_E_NOMEM;
        snprintf(url, n, "%s/ipfs/%s?format=raw", c->gateway_url, cid_str);
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

/* Fetches and parses a dag-pb manifest. */
static int fetch_manifest(pdot_client *c, const pdot_cid *root, pdot_dag_link **links,
                          size_t *nlinks, uint64_t *size)
{
    uint8_t *block;
    size_t len;
    int rc;

    if ((rc = fetch_block(c, root, &block, &len)))
        return rc;
    rc = pdot_dag_decode(block, len, links, nlinks, size);
    free(block);
    if (rc)
        return pdot_fail(c, rc, "not a single-level UnixFS file manifest");
    return 0;
}

int pdot_bulletin_fetch(pdot_client *c, const pdot_cid *cid,
                        uint8_t **data, size_t *len)
{
    pdot_dag_link *links;
    size_t nlinks, i, off = 0;
    uint64_t size;
    uint8_t *file;
    int rc;

    if (!c || !cid || !data || !len)
        return PDOT_E_INVALID;
    if (pdot_cid_codec(cid) != PDOT_CODEC_DAG_PB)
        return fetch_block(c, cid, data, len);

    *data = NULL;
    *len = 0;
    if ((rc = fetch_manifest(c, cid, &links, &nlinks, &size)))
        return rc;
    if (size > PDOT_FILE_SIZE_MAX) {
        free(links);
        return pdot_fail(c, PDOT_E_UNSUPPORTED, "file of %llu bytes exceeds %u MiB",
                         (unsigned long long)size, PDOT_FILE_SIZE_MAX >> 20);
    }
    if (!(file = malloc(size ? (size_t)size : 1))) {
        free(links);
        return PDOT_E_NOMEM;
    }
    for (i = 0; i < nlinks && !rc; i++) {
        uint8_t *chunk;
        size_t clen;

        if ((rc = fetch_block(c, &links[i].cid, &chunk, &clen)))
            break;
        /* The manifest's size is what the root CID commits to. */
        if (clen != links[i].size)
            rc = pdot_fail(c, PDOT_E_MISMATCH, "chunk %u is %zu bytes, manifest says %llu",
                           (unsigned)i, clen, (unsigned long long)links[i].size);
        else
            memcpy(file + off, chunk, clen);
        off += clen;
        free(chunk);
    }
    free(links);
    if (rc) {
        free(file);
        return rc;
    }
    *data = file;
    *len = (size_t)size;
    return 0;
}

int pdot_bulletin_renew_tree(pdot_client *c, const pdot_signer *s, const pdot_cid *root,
                             pdot_progress_fn progress, void *ud)
{
    pdot_dag_link *links = NULL;
    pdot_tx_result tx;
    size_t nlinks = 0, i;
    uint64_t size;
    int rc = 0;

    if (!c || !s || !root)
        return PDOT_E_INVALID;
    if (pdot_cid_codec(root) == PDOT_CODEC_DAG_PB &&
        (rc = fetch_manifest(c, root, &links, &nlinks, &size)))
        return rc;
    for (i = 0; i < nlinks && !rc; i++) {
        rc = pdot_bulletin_renew(c, s, &links[i].cid, &tx);
        if (progress)
            progress(ud, i, nlinks + 1, &links[i].cid, &tx, rc);
    }
    if (!rc) {
        rc = pdot_bulletin_renew(c, s, root, &tx);
        if (progress)
            progress(ud, nlinks, nlinks + 1, root, &tx, rc);
    }
    free(links);
    return rc;
}
