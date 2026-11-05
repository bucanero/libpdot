/*
 * Transactions on any chain: calls from JSON, submission with a tip or
 * through a proxy, fee estimates, transfers and proxy management.
 */
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "pdot.h"
#include "client.h"
#include "encode.h"
#include "hex.h"
#include "rng.h"
#include "rpc.h"
#include "tx.h"

static int take(pdot_buf *b, uint8_t **out, size_t *len)
{
    if (b->err) {
        int rc = b->err;
        pdot_buf_free(b);
        return rc;
    }
    *out = b->data;
    *len = b->len;
    return 0;
}

/* JSON for an account: 0x hex, which every account-shaped type accepts. */
static cJSON *account_json(const uint8_t acct[32])
{
    char hex[67];

    pdot_hex_encode(acct, 32, hex);
    return cJSON_CreateString(hex);
}

static cJSON *amount_json(pdot_u128 v)
{
    char dec[64];

    pdot_u128_format(v, 0, dec, sizeof dec);
    return cJSON_CreateString(dec);
}

/* Encodes pallet.call(args), taking ownership of args. */
static int encode_call(pdot_client *c, const char *pallet, const char *call, cJSON *args,
                       pdot_buf *out)
{
    char err[200];
    int rc;

    if (!args)
        return pdot_fail(c, PDOT_E_NOMEM, "out of memory");
    rc = pdot_encode_call_json(c->md, pallet, call, args, out, err, sizeof err);
    cJSON_Delete(args);
    return rc ? pdot_fail(c, rc, "%s", err) : 0;
}

int pdot_call_from_json(pdot_client *c, const char *pallet, const char *call,
                        const char *json_args, uint8_t **call_out, size_t *len)
{
    cJSON *args = NULL;
    pdot_buf b;
    int rc;

    if (!c || !pallet || !call || !call_out || !len)
        return PDOT_E_INVALID;
    if ((rc = pdot_chain_sync(c)))
        return rc;
    if (json_args && *json_args && !(args = cJSON_Parse(json_args)))
        return pdot_fail(c, PDOT_E_INVALID, "%s.%s: arguments are not valid JSON", pallet, call);
    pdot_buf_init(&b);
    if (!args)
        args = cJSON_CreateArray();
    if ((rc = encode_call(c, pallet, call, args, &b))) {
        pdot_buf_free(&b);
        return rc;
    }
    return take(&b, call_out, len);
}

/* Proxy.proxy(real, None, call): runs call as `real`. */
static int wrap_proxy(pdot_client *c, const uint8_t real[32], const uint8_t *call, size_t len,
                      pdot_buf *out)
{
    pdot_md_iter it;
    pdot_md_field f[3];
    char err[200];
    cJSON *v;
    size_t i;
    int rc;

    if (pdot_encode_call_begin(c->md, "Proxy", "proxy", out, &it) || it.left != 3)
        return pdot_fail(c, PDOT_E_METADATA, "this runtime has no Proxy.proxy(real, type, call)");
    for (i = 0; i < 3; i++)
        if (pdot_md_iter_next(&it, &f[i]) != 1)
            return PDOT_E_METADATA;
    if (c->md->have_xt_types && f[2].ty != c->md->call_ty)
        return pdot_fail(c, PDOT_E_METADATA, "Proxy.proxy's third argument is not a call");

    v = account_json(real);
    rc = v ? pdot_encode_json(c->md, f[0].ty, v, out, err, sizeof err) : PDOT_E_NOMEM;
    cJSON_Delete(v);
    if (!rc) {
        v = cJSON_CreateNull();  /* force_proxy_type: any of the real account's proxies */
        rc = pdot_encode_json(c->md, f[1].ty, v, out, err, sizeof err);
        cJSON_Delete(v);
    }
    if (rc)
        return pdot_fail(c, rc, "Proxy.proxy: %s", err);
    pdot_scale_raw(out, call, len);
    return out->err;
}

/* Fetches parameters and builds the signed extrinsic for call under o. */
static int build(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                 const pdot_tx_options *o, pdot_tx_params *p, pdot_buf *xt)
{
    pdot_buf wrapped;
    int rc;

    if ((rc = pdot_tx_params_fetch(c, s, p)))
        return rc;
    if (o)
        p->tip = o->tip;
    pdot_buf_init(&wrapped);
    if (o && o->proxy_for) {
        if (!(rc = wrap_proxy(c, o->proxy_for, call, len, &wrapped))) {
            call = wrapped.data;
            len = wrapped.len;
        }
    }
    if (!rc)
        rc = pdot_tx_build(c, p, s, call, len, xt);
    pdot_buf_free(&wrapped);
    return rc;
}

int pdot_tx_submit(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                   const pdot_tx_options *o, pdot_tx_result *out)
{
    pdot_tx_params p;
    pdot_buf xt;
    int rc;

    if (!c || !s || !call || !len || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    pdot_buf_init(&xt);
    if (!(rc = build(c, s, call, len, o, &p, &xt)))
        rc = pdot_tx_submit_watch(c, &p, xt.data, xt.len, out, NULL);
    pdot_buf_free(&xt);
    return rc;
}

/*
 * RuntimeDispatchInfo { weight: Weight { ref_time: Compact<u64>,
 * proof_size: Compact<u64> }, class: DispatchClass, partial_fee: u128 }.
 */
static int decode_fee(const uint8_t *p, size_t n, pdot_fee *out)
{
    pdot_reader r;

    pdot_reader_init(&r, p, n);
    if (pdot_read_compact(&r, &out->ref_time) || pdot_read_compact(&r, &out->proof_size) ||
        pdot_read_u8(&r, &out->dispatch_class) || pdot_read_u64(&r, &out->partial_fee.lo) ||
        pdot_read_u64(&r, &out->partial_fee.hi) || pdot_reader_remaining(&r) != 0)
        return PDOT_E_DECODE;
    return 0;
}

int pdot_tx_fee(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                const pdot_tx_options *o, pdot_fee *out)
{
    pdot_tx_params p;
    pdot_buf xt;
    uint8_t *res = NULL;
    size_t res_len;
    int rc;

    if (!c || !s || !call || !len || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    pdot_buf_init(&xt);
    if (!(rc = build(c, s, call, len, o, &p, &xt))) {
        /* query_info(uxt, len): the extrinsic as encoded, then its length. */
        uint32_t xl = (uint32_t)xt.len;
        pdot_scale_u32(&xt, xl);
        if (!(rc = xt.err) &&
            !(rc = pdot_rpc_state_call(c, "TransactionPaymentApi_query_info", xt.data, xt.len,
                                       &res, &res_len)) &&
            decode_fee(res, res_len, out))
            rc = pdot_fail(c, PDOT_E_DECODE, "cannot decode TransactionPaymentApi_query_info");
    }
    free(res);
    pdot_buf_free(&xt);
    return rc;
}

/* ---- Transfers ----------------------------------------------------------- */

int pdot_transfer_call(pdot_client *c, const pdot_transfer *t, uint8_t **call, size_t *len)
{
    cJSON *args;
    pdot_buf b;
    const char *pallet, *fn;
    int rc;

    if (!c || !t || !call || !len)
        return PDOT_E_INVALID;
    if ((rc = pdot_chain_sync(c)))
        return rc;
    if (!(args = cJSON_CreateArray()))
        return PDOT_E_NOMEM;
    switch (t->kind) {
    case PDOT_TRANSFER_NATIVE:
        pallet = "Balances", fn = "transfer_keep_alive";
        cJSON_AddItemToArray(args, account_json(t->dest));
        cJSON_AddItemToArray(args, amount_json(t->amount));
        break;
    case PDOT_TRANSFER_ASSET:
        pallet = "Assets", fn = "transfer_keep_alive";
        cJSON_AddItemToArray(args, cJSON_CreateNumber(t->id));
        cJSON_AddItemToArray(args, account_json(t->dest));
        cJSON_AddItemToArray(args, amount_json(t->amount));
        break;
    case PDOT_TRANSFER_NFT:
        pallet = "Nfts", fn = "transfer";
        cJSON_AddItemToArray(args, cJSON_CreateNumber(t->id));
        cJSON_AddItemToArray(args, cJSON_CreateNumber(t->item));
        cJSON_AddItemToArray(args, account_json(t->dest));
        break;
    case PDOT_TRANSFER_ALL:
        pallet = "Balances", fn = "transfer_all";
        cJSON_AddItemToArray(args, account_json(t->dest));
        cJSON_AddItemToArray(args, cJSON_CreateFalse());  /* keep_alive: no, close it */
        break;
    default:
        cJSON_Delete(args);
        return pdot_fail(c, PDOT_E_INVALID, "unknown transfer kind %d", t->kind);
    }
    pdot_buf_init(&b);
    if ((rc = encode_call(c, pallet, fn, args, &b))) {
        pdot_buf_free(&b);
        return rc;
    }
    return take(&b, call, len);
}

int pdot_transfer_submit(pdot_client *c, const pdot_signer *s, const pdot_transfer *t,
                         const pdot_tx_options *o, pdot_tx_result *out)
{
    uint8_t *call;
    size_t len;
    int rc;

    if (!out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if ((rc = pdot_transfer_call(c, t, &call, &len)))
        return rc;
    rc = pdot_tx_submit(c, s, call, len, o, out);
    free(call);
    return rc;
}

/* ---- Proxies ------------------------------------------------------------- */

int pdot_signer_sr25519_random(pdot_signer *s, uint8_t seed_out[32])
{
    uint8_t seed[32];
    int rc;

    if (!s)
        return PDOT_E_INVALID;
    if ((rc = pdot_rng_bytes(seed, sizeof seed)))
        return rc;
    rc = pdot_signer_sr25519_from_seed(s, seed);
    if (!rc && seed_out)
        memcpy(seed_out, seed, 32);
    pdot_wipe(seed, sizeof seed);
    return rc;
}

static int proxy_call(pdot_client *c, const char *fn, const uint8_t delegate[32],
                      const char *proxy_type, pdot_buf *out)
{
    cJSON *args = cJSON_CreateArray();

    if (args) {
        cJSON_AddItemToArray(args, account_json(delegate));
        cJSON_AddItemToArray(args, cJSON_CreateString(proxy_type));
        cJSON_AddItemToArray(args, cJSON_CreateNumber(0));  /* delay */
    }
    return encode_call(c, "Proxy", fn, args, out);
}

int pdot_proxy_add(pdot_client *c, const pdot_signer *s, const uint8_t delegate[32],
                   const char *proxy_type, pdot_u128 fund, pdot_tx_result *out)
{
    pdot_buf add, send, batch;
    pdot_md_iter it;
    int rc;

    if (!c || !s || !delegate || !proxy_type || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if ((rc = pdot_chain_sync(c)))
        return rc;
    pdot_buf_init(&add);
    pdot_buf_init(&send);
    pdot_buf_init(&batch);
    if (!(rc = proxy_call(c, "add_proxy", delegate, proxy_type, &add))) {
        if (!fund.lo && !fund.hi) {
            rc = pdot_tx_submit(c, s, add.data, add.len, NULL, out);
        } else {
            cJSON *args = cJSON_CreateArray();
            if (args) {
                cJSON_AddItemToArray(args, account_json(delegate));
                cJSON_AddItemToArray(args, amount_json(fund));
            }
            if (!(rc = encode_call(c, "Balances", "transfer_keep_alive", args, &send))) {
                /* Utility.batch_all(Vec<RuntimeCall>): all or nothing. */
                if (pdot_encode_call_begin(c->md, "Utility", "batch_all", &batch, &it) ||
                    it.left != 1) {
                    rc = pdot_fail(c, PDOT_E_METADATA, "this runtime has no Utility.batch_all");
                } else {
                    pdot_scale_compact(&batch, 2);
                    pdot_scale_raw(&batch, send.data, send.len);
                    pdot_scale_raw(&batch, add.data, add.len);
                    rc = batch.err ? batch.err
                                   : pdot_tx_submit(c, s, batch.data, batch.len, NULL, out);
                }
            }
        }
    }
    pdot_buf_free(&add);
    pdot_buf_free(&send);
    pdot_buf_free(&batch);
    return rc;
}

int pdot_proxy_remove(pdot_client *c, const pdot_signer *s, const uint8_t delegate[32],
                      const char *proxy_type, pdot_tx_result *out)
{
    pdot_buf b;
    int rc;

    if (!c || !s || !delegate || !proxy_type || !out)
        return PDOT_E_INVALID;
    memset(out, 0, sizeof *out);
    if ((rc = pdot_chain_sync(c)))
        return rc;
    pdot_buf_init(&b);
    if (!(rc = proxy_call(c, "remove_proxy", delegate, proxy_type, &b)))
        rc = pdot_tx_submit(c, s, b.data, b.len, NULL, out);
    pdot_buf_free(&b);
    return rc;
}
