/* Balances, pallet-assets and pallet-nfts reads at the newest final block. */
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "assets.h"
#include "decode.h"
#include "hex.h"
#include "metadata.h"
#include "storage.h"
#include "verify.h"

/* Items listed per state_getKeysPaged call, and proven per read proof. */
#define KEYS_PAGE   1000
#define PROOF_BATCH 64
#define OWNED_MAX   100000

static void le32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)(v >> 16);
    out[3] = (uint8_t)(v >> 24);
}

/* Type of the idx-th key of a map: the key type itself, or a tuple element. */
static int key_type(const pdot_metadata *md, const pdot_md_storage *st, size_t idx, uint32_t *ty)
{
    pdot_md_field f[PDOT_MD_MAX_HASHERS];
    size_t nf;

    if (st->nhashers == 1) {
        *ty = st->key_ty;
        return idx == 0 ? 0 : PDOT_E_METADATA;
    }
    if (pdot_md_fields(md, st->key_ty, f, PDOT_MD_MAX_HASHERS, &nf) || nf != st->nhashers ||
        idx >= nf)
        return PDOT_E_METADATA;
    *ty = f[idx].ty;
    return 0;
}

static int key_is_u32(const pdot_metadata *md, const pdot_md_storage *st, size_t idx)
{
    uint32_t ty;
    uint8_t prim;

    return key_type(md, st, idx, &ty) == 0 && pdot_md_primitive(md, ty, &prim) == 0 &&
           prim == PDOT_PRIM_U32;
}

/*
 * A full key of pallet.entry; ids lists which parts must be u32 on chain
 * (asset, collection and item ids), as a bit mask.
 */
static int full_key(pdot_client *c, const char *pallet, const char *entry,
                    const pdot_key_part *parts, size_t nparts, unsigned u32_mask, pdot_buf *key)
{
    pdot_md_storage st;
    size_t i;
    int rc;

    if ((rc = pdot_storage_key(c, c->md, pallet, entry, parts, nparts, &st, key)))
        return rc;
    if (st.nhashers != nparts)
        return pdot_fail(c, PDOT_E_METADATA, "%s.%s has %zu keys, expected %zu", pallet, entry,
                         st.nhashers, nparts);
    for (i = 0; i < nparts; i++)
        if ((u32_mask >> i & 1) && !key_is_u32(c->md, &st, i))
            return pdot_fail(c, PDOT_E_METADATA, "%s.%s: key %zu is not a u32 id", pallet,
                             entry, i);
    return 0;
}

/* AttributeNamespace: variant index by name, plus the account for Account. */
static int encode_namespace(pdot_client *c, const pdot_query *q, pdot_buf *out)
{
    static const char *const names[] = { "Pallet", "CollectionOwner", "ItemOwner", "Account" };
    pdot_md_storage st;
    uint32_t ty;
    uint8_t index;

    if (q->ns < 0 || q->ns > PDOT_NFT_NS_ACCOUNT)
        return pdot_fail(c, PDOT_E_INVALID, "unknown attribute namespace %d", q->ns);
    if (pdot_md_storage_entry(c->md, "Nfts", "Attribute", &st) || st.nhashers != 4 ||
        key_type(c->md, &st, 2, &ty) || pdot_md_variant(c->md, ty, names[q->ns], &index, NULL))
        return pdot_fail(c, PDOT_E_METADATA, "Nfts.Attribute has no %s namespace", names[q->ns]);
    pdot_scale_u8(out, index);
    if (q->ns == PDOT_NFT_NS_ACCOUNT)
        pdot_scale_raw(out, q->account, 32);
    return out->err;
}

int pdot_query_keys(pdot_client *c, const pdot_query *q, pdot_buf keys[PDOT_Q_MAX_KEYS],
                    size_t *nkeys)
{
    uint8_t id[4], item[4];
    pdot_key_part p[4];
    int rc = 0;

    le32(id, q->id);
    le32(item, q->item);
    *nkeys = 0;
    switch (q->kind) {
    case PDOT_Q_STORAGE:
        pdot_scale_raw(&keys[0], q->bytes, q->len);
        *nkeys = 1;
        return keys[0].err;
    case PDOT_Q_BALANCE:
        p[0].p = q->account; p[0].len = 32;
        rc = full_key(c, "System", "Account", p, 1, 0, &keys[0]);
        *nkeys = 1;
        return rc;
    case PDOT_Q_ASSET_BALANCE:
        p[0].p = id; p[0].len = 4;
        p[1].p = q->account; p[1].len = 32;
        rc = full_key(c, "Assets", "Account", p, 2, 1, &keys[0]);
        *nkeys = 1;
        return rc;
    case PDOT_Q_ASSET_INFO:
        p[0].p = id; p[0].len = 4;
        if (!(rc = full_key(c, "Assets", "Asset", p, 1, 1, &keys[0])))
            rc = full_key(c, "Assets", "Metadata", p, 1, 1, &keys[1]);
        *nkeys = 2;
        return rc;
    case PDOT_Q_NFT_ITEM:
        p[0].p = id; p[0].len = 4;
        p[1].p = item; p[1].len = 4;
        if (!(rc = full_key(c, "Nfts", "Item", p, 2, 3, &keys[0])))
            rc = full_key(c, "Nfts", "ItemMetadataOf", p, 2, 3, &keys[1]);
        *nkeys = 2;
        return rc;
    case PDOT_Q_PROXIES:
        p[0].p = q->account; p[0].len = 32;
        rc = full_key(c, "Proxy", "Proxies", p, 1, 0, &keys[0]);
        *nkeys = 1;
        return rc;
    case PDOT_Q_NFT_ATTRIBUTE: {
        pdot_buf opt, ns, k;

        pdot_buf_init(&opt);
        pdot_buf_init(&ns);
        pdot_buf_init(&k);
        pdot_scale_u8(&opt, q->has_item ? 1 : 0);  /* Option<ItemId> */
        if (q->has_item)
            pdot_scale_raw(&opt, item, 4);
        pdot_scale_bytes(&k, q->bytes, q->len);
        if (!(rc = opt.err ? opt.err : k.err) && !(rc = encode_namespace(c, q, &ns))) {
            p[0].p = id; p[0].len = 4;
            p[1].p = opt.data; p[1].len = opt.len;
            p[2].p = ns.data; p[2].len = ns.len;
            p[3].p = k.data; p[3].len = k.len;
            rc = full_key(c, "Nfts", "Attribute", p, 4, 1, &keys[0]);
        }
        pdot_buf_free(&opt);
        pdot_buf_free(&ns);
        pdot_buf_free(&k);
        *nkeys = 1;
        return rc;
    }
    default:
        return PDOT_E_INVALID;
    }
}

/* Copies a byte string field into a fixed array, NUL-terminating text. */
static void copy_text(char *dst, size_t cap, const uint8_t *p, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static int field_account(const pdot_metadata *md, uint32_t ty, const pdot_value *v,
                         const char *path, uint8_t out[32])
{
    const uint8_t *p;
    size_t n;
    int rc;

    if ((rc = pdot_decode_path_bytes(md, ty, v->data, v->len, path, &p, &n)))
        return rc;
    if (n != 32)
        return PDOT_E_METADATA;
    memcpy(out, p, 32);
    return 0;
}

static uint32_t value_type(const pdot_metadata *md, const char *pallet, const char *entry)
{
    pdot_md_storage st;

    return pdot_md_storage_entry(md, pallet, entry, &st) ? UINT32_MAX : st.value_ty;
}

static int decode_balance(const pdot_metadata *md, const pdot_value *v, pdot_balance *out)
{
    uint32_t ty = value_type(md, "System", "Account");
    uint64_t nonce;

    if (!v->present)
        return 0;
    out->exists = 1;
    if (pdot_decode_path_uint(md, ty, v->data, v->len, "nonce", &nonce) ||
        pdot_decode_path_u128(md, ty, v->data, v->len, "data.free", &out->free) ||
        pdot_decode_path_u128(md, ty, v->data, v->len, "data.reserved", &out->reserved) ||
        pdot_decode_path_u128(md, ty, v->data, v->len, "data.frozen", &out->frozen))
        return PDOT_E_METADATA;
    out->nonce = (uint32_t)nonce;
    return 0;
}

static int decode_asset_balance(const pdot_metadata *md, const pdot_value *v,
                                pdot_asset_balance *out)
{
    uint32_t ty = value_type(md, "Assets", "Account");
    const char *name;
    size_t len;

    if (!v->present)
        return 0;
    out->exists = 1;
    if (pdot_decode_path_u128(md, ty, v->data, v->len, "balance", &out->balance) ||
        pdot_decode_path_variant(md, ty, v->data, v->len, "status", &name, &len))
        return PDOT_E_METADATA;
    out->frozen = !pdot_md_name_eq(name, len, "Liquid");
    return 0;
}

static int decode_asset_info(const pdot_metadata *md, const pdot_value *v, pdot_asset_info *out)
{
    uint32_t ty = value_type(md, "Assets", "Asset");
    uint32_t mty = value_type(md, "Assets", "Metadata");
    uint64_t accounts, sufficient, decimals;
    const uint8_t *p;
    const char *name;
    size_t n;

    if (!v[0].present)
        return 0;
    out->exists = 1;
    if (field_account(md, ty, &v[0], "owner", out->owner) ||
        field_account(md, ty, &v[0], "issuer", out->issuer) ||
        field_account(md, ty, &v[0], "admin", out->admin) ||
        field_account(md, ty, &v[0], "freezer", out->freezer) ||
        pdot_decode_path_u128(md, ty, v[0].data, v[0].len, "supply", &out->supply) ||
        pdot_decode_path_u128(md, ty, v[0].data, v[0].len, "min_balance", &out->min_balance) ||
        pdot_decode_path_uint(md, ty, v[0].data, v[0].len, "accounts", &accounts) ||
        pdot_decode_path_uint(md, ty, v[0].data, v[0].len, "is_sufficient", &sufficient) ||
        pdot_decode_path_variant(md, ty, v[0].data, v[0].len, "status", &name, &n))
        return PDOT_E_METADATA;
    out->accounts = (uint32_t)accounts;
    out->sufficient = sufficient != 0;
    out->live = pdot_md_name_eq(name, n, "Live");
    if (!v[1].present)
        return 0;
    if (pdot_decode_path_bytes(md, mty, v[1].data, v[1].len, "name", &p, &n))
        return PDOT_E_METADATA;
    copy_text(out->name, sizeof out->name, p, n);
    if (pdot_decode_path_bytes(md, mty, v[1].data, v[1].len, "symbol", &p, &n))
        return PDOT_E_METADATA;
    copy_text(out->symbol, sizeof out->symbol, p, n);
    if (pdot_decode_path_uint(md, mty, v[1].data, v[1].len, "decimals", &decimals))
        return PDOT_E_METADATA;
    out->decimals = (uint8_t)decimals;
    return 0;
}

static int decode_nft_item(const pdot_metadata *md, const pdot_value *v, pdot_nft_item *out)
{
    uint32_t ty = value_type(md, "Nfts", "Item");
    uint32_t mty = value_type(md, "Nfts", "ItemMetadataOf");
    const uint8_t *p;
    size_t n;

    if (!v[0].present)
        return 0;
    out->exists = 1;
    if (field_account(md, ty, &v[0], "owner", out->owner))
        return PDOT_E_METADATA;
    if (!v[1].present)
        return 0;
    if (pdot_decode_path_bytes(md, mty, v[1].data, v[1].len, "data", &p, &n))
        return PDOT_E_METADATA;
    out->has_metadata = 1;
    out->metadata_len = n < sizeof out->metadata ? n : sizeof out->metadata;
    memcpy(out->metadata, p, out->metadata_len);
    return 0;
}

static int decode_nft_attribute(const pdot_metadata *md, const pdot_value *v,
                                pdot_nft_attribute *out)
{
    pdot_md_field f[2];
    pdot_reader r;
    const uint8_t *p;
    size_t nf, n;

    if (!v->present)
        return 0;
    out->exists = 1;
    /* (value: BoundedVec<u8>, deposit) */
    if (pdot_md_fields(md, value_type(md, "Nfts", "Attribute"), f, 2, &nf) || nf != 2)
        return PDOT_E_METADATA;
    pdot_reader_init(&r, v->data, v->len);
    if (pdot_decode_bytes(md, f[0].ty, &r, &p, &n))
        return PDOT_E_METADATA;
    out->len = n < sizeof out->value ? n : sizeof out->value;
    memcpy(out->value, p, out->len);
    return 0;
}

/* Element type of a Vec<T>, or of a one-field wrapper of one (BoundedVec). */
static int seq_elem(const pdot_metadata *md, uint32_t ty, uint32_t *elem)
{
    enum pdot_typedef kind;
    pdot_reader t;
    pdot_md_field f;
    size_t nf;
    uint64_t n;
    int depth;

    for (depth = 0; depth < 4; depth++) {
        if (pdot_md_typedef(md, ty, &t, &kind))
            return PDOT_E_METADATA;
        if (kind == PDOT_TYPE_SEQUENCE) {
            if (pdot_read_compact(&t, &n) || n >= md->ntypes)
                return PDOT_E_METADATA;
            *elem = (uint32_t)n;
            return 0;
        }
        if (kind != PDOT_TYPE_COMPOSITE || pdot_md_fields(md, ty, &f, 1, &nf) || nf != 1)
            return PDOT_E_METADATA;
        ty = f.ty;
    }
    return PDOT_E_METADATA;
}

/* (BoundedVec<ProxyDefinition { delegate, proxy_type, delay }>, deposit) */
static int decode_proxies(const pdot_metadata *md, const pdot_value *v, pdot_proxies *out)
{
    pdot_md_field f[2], df;
    pdot_md_iter it;
    pdot_reader r;
    uint32_t elem;
    uint64_t n, i, delay;
    size_t nf;
    int rc;

    if (!v->present)
        return 0;
    if (pdot_md_fields(md, value_type(md, "Proxy", "Proxies"), f, 2, &nf) || nf != 2 ||
        seq_elem(md, f[0].ty, &elem))
        return PDOT_E_METADATA;
    pdot_reader_init(&r, v->data, v->len);
    if (pdot_read_compact(&r, &n))
        return PDOT_E_DECODE;
    for (i = 0; i < n; i++) {
        pdot_proxy_def def;

        memset(&def, 0, sizeof def);
        if (pdot_md_iter_fields(md, elem, &it))
            return PDOT_E_METADATA;
        while ((rc = pdot_md_iter_next(&it, &df)) == 1) {
            if (pdot_md_name_eq(df.name, df.name_len, "delegate")) {
                const uint8_t *p;
                size_t len;
                if ((rc = pdot_decode_bytes(md, df.ty, &r, &p, &len)) || len != 32)
                    return rc ? rc : PDOT_E_METADATA;
                memcpy(def.delegate, p, 32);
            } else if (pdot_md_name_eq(df.name, df.name_len, "proxy_type")) {
                pdot_md_iter vf;
                pdot_md_field x;
                const char *name;
                size_t len;
                if ((rc = pdot_decode_variant(md, df.ty, &r, &vf, &name, &len)))
                    return rc;
                copy_text(def.type, sizeof def.type, (const uint8_t *)name, len);
                while ((rc = pdot_md_iter_next(&vf, &x)) == 1)
                    if ((rc = pdot_decode_skip(md, x.ty, &r)))
                        return rc;
            } else if (pdot_md_name_eq(df.name, df.name_len, "delay")) {
                if ((rc = pdot_decode_uint(md, df.ty, &r, &delay)))
                    return rc;
                def.delay = (uint32_t)delay;
            } else if ((rc = pdot_decode_skip(md, df.ty, &r))) {
                return rc;
            }
        }
        if (rc < 0)
            return rc;
        if (out->n < PDOT_PROXIES_MAX)
            out->list[out->n++] = def;
    }
    return pdot_decode_u128(md, f[1].ty, &r, &out->deposit);
}

size_t pdot_query_result_size(int kind)
{
    switch (kind) {
    case PDOT_Q_STORAGE:       return sizeof(pdot_storage_value);
    case PDOT_Q_BALANCE:       return sizeof(pdot_balance);
    case PDOT_Q_ASSET_BALANCE: return sizeof(pdot_asset_balance);
    case PDOT_Q_ASSET_INFO:    return sizeof(pdot_asset_info);
    case PDOT_Q_NFT_ITEM:      return sizeof(pdot_nft_item);
    case PDOT_Q_NFT_ATTRIBUTE: return sizeof(pdot_nft_attribute);
    case PDOT_Q_PROXIES:       return sizeof(pdot_proxies);
    default:                   return 0;
    }
}

int pdot_query_decode(pdot_client *c, const pdot_query *q, pdot_value *values,
                      const pdot_final_block *at, void *out)
{
    static const char *const what[] = {
        "storage", "System.Account", "Assets.Account", "Assets.Asset", "Nfts.Item",
        "Nfts.Attribute", "Proxy.Proxies"
    };
    int rc;

    memset(out, 0, pdot_query_result_size(q->kind));
    switch (q->kind) {
    case PDOT_Q_STORAGE: {
        pdot_storage_value *sv = out;
        sv->exists = values[0].present;
        sv->data = values[0].data;
        sv->len = values[0].len;
        sv->at = *at;
        values[0].data = NULL;
        return 0;
    }
    case PDOT_Q_BALANCE:
        rc = decode_balance(c->md, values, out);
        ((pdot_balance *)out)->at = *at;
        break;
    case PDOT_Q_ASSET_BALANCE:
        rc = decode_asset_balance(c->md, values, out);
        ((pdot_asset_balance *)out)->at = *at;
        break;
    case PDOT_Q_ASSET_INFO:
        rc = decode_asset_info(c->md, values, out);
        ((pdot_asset_info *)out)->at = *at;
        break;
    case PDOT_Q_NFT_ITEM:
        rc = decode_nft_item(c->md, values, out);
        ((pdot_nft_item *)out)->at = *at;
        break;
    case PDOT_Q_NFT_ATTRIBUTE:
        rc = decode_nft_attribute(c->md, values, out);
        ((pdot_nft_attribute *)out)->at = *at;
        break;
    case PDOT_Q_PROXIES:
        rc = decode_proxies(c->md, values, out);
        ((pdot_proxies *)out)->at = *at;
        break;
    default:
        return PDOT_E_INVALID;
    }
    return rc ? pdot_fail(c, rc, "cannot decode %s at #%u", what[q->kind], at->number) : 0;
}

int pdot_query_run(pdot_client *c, const pdot_query *q, void *out)
{
    pdot_buf keys[PDOT_Q_MAX_KEYS];
    pdot_value values[PDOT_Q_MAX_KEYS];
    const uint8_t *kp[PDOT_Q_MAX_KEYS];
    size_t kl[PDOT_Q_MAX_KEYS], n = 0, i;
    pdot_final_block at;
    const pdot_header *head;
    int rc;

    memset(out, 0, pdot_query_result_size(q->kind));
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_init(&keys[i]);
    if (!(rc = pdot_chain_sync(c)) && !(rc = pdot_query_keys(c, q, keys, &n)) &&
        !(rc = pdot_final_block_get(c, &at, &head))) {
        for (i = 0; i < n; i++) {
            kp[i] = keys[i].data;
            kl[i] = keys[i].len;
        }
        if (!(rc = pdot_storage_read_at(c, &at, head, kp, kl, n, values))) {
            rc = pdot_query_decode(c, q, values, &at, out);
            pdot_values_free(values, n);
        }
    }
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_free(&keys[i]);
    return rc;
}

/* ---- Public reads -------------------------------------------------------- */

int pdot_balance_get(pdot_client *c, const uint8_t account_id[32], pdot_balance *out)
{
    pdot_query q;

    if (!c || !account_id || !out)
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_BALANCE;
    memcpy(q.account, account_id, 32);
    return pdot_query_run(c, &q, out);
}

int pdot_asset_balance_get(pdot_client *c, uint32_t asset_id, const uint8_t account_id[32],
                           pdot_asset_balance *out)
{
    pdot_query q;

    if (!c || !account_id || !out)
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_ASSET_BALANCE;
    q.id = asset_id;
    memcpy(q.account, account_id, 32);
    return pdot_query_run(c, &q, out);
}

int pdot_asset_info_get(pdot_client *c, uint32_t asset_id, pdot_asset_info *out)
{
    pdot_query q;

    if (!c || !out)
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_ASSET_INFO;
    q.id = asset_id;
    return pdot_query_run(c, &q, out);
}

int pdot_nft_item_get(pdot_client *c, uint32_t collection, uint32_t item, pdot_nft_item *out)
{
    pdot_query q;

    if (!c || !out)
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_NFT_ITEM;
    q.id = collection;
    q.item = item;
    return pdot_query_run(c, &q, out);
}

int pdot_nft_attribute_get(pdot_client *c, uint32_t collection, int has_item, uint32_t item,
                           int ns, const uint8_t ns_account[32], const uint8_t *key,
                           size_t key_len, pdot_nft_attribute *out)
{
    pdot_query q;

    if (!c || !out || (!key && key_len) || (ns == PDOT_NFT_NS_ACCOUNT && !ns_account))
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_NFT_ATTRIBUTE;
    q.id = collection;
    q.has_item = has_item;
    q.item = item;
    q.ns = ns;
    if (ns == PDOT_NFT_NS_ACCOUNT)
        memcpy(q.account, ns_account, 32);
    q.bytes = key;
    q.len = key_len;
    return pdot_query_run(c, &q, out);
}

int pdot_proxies_get(pdot_client *c, const uint8_t account_id[32], pdot_proxies *out)
{
    pdot_query q;

    if (!c || !account_id || !out)
        return PDOT_E_INVALID;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_PROXIES;
    memcpy(q.account, account_id, 32);
    return pdot_query_run(c, &q, out);
}

/* ---- Owned NFTs ---------------------------------------------------------- */

/* Appends the items listed under prefix (Nfts.Account(account, collection, *)). */
static int list_owned(pdot_client *c, const pdot_buf *prefix, const pdot_final_block *at,
                      size_t max, uint32_t *items, size_t *n)
{
    uint8_t *last = NULL;
    size_t last_len = 0;
    int rc = 0;

    while (*n < max && !rc) {
        uint32_t want = (uint32_t)(max - *n < KEYS_PAGE ? max - *n : KEYS_PAGE), got = 0;
        cJSON *keys, *k;

        if ((rc = pdot_rpc_keys_paged(c, prefix->data, prefix->len, want, last, last_len,
                                      at->hash, &keys)))
            break;
        cJSON_ArrayForEach(k, keys) {
            uint8_t *raw;
            size_t len;

            if (!cJSON_IsString(k) || pdot_hex_decode_alloc(k->valuestring, &raw, &len)) {
                rc = pdot_fail(c, PDOT_E_RPC, "state_getKeysPaged: malformed key");
                break;
            }
            /* prefix | blake2_128(item) | item */
            if (len != prefix->len + 20 || memcmp(raw, prefix->data, prefix->len) != 0) {
                free(raw);
                rc = pdot_fail(c, PDOT_E_RPC, "state_getKeysPaged: key outside the prefix");
                break;
            }
            if (*n < max)
                items[(*n)++] = (uint32_t)raw[len - 4] | (uint32_t)raw[len - 3] << 8 |
                                (uint32_t)raw[len - 2] << 16 | (uint32_t)raw[len - 1] << 24;
            free(last);
            last = raw;
            last_len = len;
            got++;
        }
        cJSON_Delete(keys);
        if (got < want)
            break;
    }
    free(last);
    return rc;
}

/* Proves each listed item's Nfts.Account entry at head. */
static int prove_owned(pdot_client *c, const pdot_header *head, const pdot_buf *prefix,
                       const uint8_t hasher, const uint32_t *items, size_t n)
{
    size_t start, i;
    int rc = 0;

    for (start = 0; start < n && !rc; start += PROOF_BATCH) {
        size_t count = n - start < PROOF_BATCH ? n - start : PROOF_BATCH;
        pdot_buf keys[PROOF_BATCH];
        const uint8_t *kp[PROOF_BATCH];
        size_t kl[PROOF_BATCH];
        pdot_value values[PROOF_BATCH];

        for (i = 0; i < count; i++) {
            uint8_t id[4];

            le32(id, items[start + i]);
            pdot_buf_init(&keys[i]);
            pdot_scale_raw(&keys[i], prefix->data, prefix->len);
            if (!rc && (rc = pdot_storage_hash(&keys[i], hasher, id, 4)))
                rc = pdot_fail(c, rc, "Nfts.Account: unsupported hasher");
            kp[i] = keys[i].data;
            kl[i] = keys[i].len;
        }
        if (!rc && !(rc = pdot_verify_read_many(c, head, kp, kl, count, values))) {
            for (i = 0; i < count && !rc; i++)
                if (!values[i].present)
                    rc = pdot_fail(c, PDOT_E_UNVERIFIED,
                                   "the node listed item %u, which the proof shows is not owned",
                                   items[start + i]);
            pdot_values_free(values, count);
        }
        for (i = 0; i < count; i++)
            pdot_buf_free(&keys[i]);
    }
    return rc;
}

int pdot_nft_owned(pdot_client *c, const uint8_t account_id[32], uint32_t collection,
                   size_t max, uint32_t **items, size_t *n, pdot_final_block *at)
{
    pdot_md_storage st;
    pdot_key_part p[2];
    pdot_buf prefix;
    pdot_final_block blk;
    const pdot_header *head;
    uint8_t id[4];
    int rc;

    memset(&blk, 0, sizeof blk);
    if (!c || !account_id || !items || !n || max > OWNED_MAX)
        return PDOT_E_INVALID;
    *items = NULL;
    *n = 0;
    if (max == 0)
        return 0;
    if (!(*items = malloc(max * sizeof **items)))
        return PDOT_E_NOMEM;
    le32(id, collection);
    p[0].p = account_id; p[0].len = 32;
    p[1].p = id; p[1].len = 4;
    pdot_buf_init(&prefix);
    if (!(rc = pdot_chain_sync(c)) &&
        !(rc = pdot_storage_key(c, c->md, "Nfts", "Account", p, 2, &st, &prefix))) {
        if (st.nhashers != 3 || st.hashers[2] != PDOT_HASHER_BLAKE2_128_CONCAT ||
            !key_is_u32(c->md, &st, 1) || !key_is_u32(c->md, &st, 2))
            rc = pdot_fail(c, PDOT_E_METADATA, "Nfts.Account is not (account, u32, u32)");
    }
    if (!rc && !(rc = pdot_final_block_get(c, &blk, &head)) &&
        !(rc = list_owned(c, &prefix, &blk, max, *items, n)) && head)
        rc = prove_owned(c, head, &prefix, st.hashers[2], *items, *n);
    pdot_buf_free(&prefix);
    if (rc) {
        free(*items);
        *items = NULL;
        *n = 0;
        return rc;
    }
    if (at)
        *at = blk;
    return 0;
}
