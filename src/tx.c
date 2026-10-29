#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hash.h"
#include "hex.h"
#include "rpc.h"
#include "tx.h"

uint16_t pdot_era_mortal(uint32_t period, uint32_t current)
{
    uint32_t p = 4, quantize, phase, tz = 0;

    /* Round up to a power of two in [4, 65536]. */
    while (p < period && p < 65536)
        p <<= 1;
    quantize = p >> 12 ? p >> 12 : 1;
    phase = (current % p) / quantize * quantize;
    while (!((p >> tz) & 1))
        tz++;
    tz = tz - 1 < 1 ? 1 : (tz - 1 > 15 ? 15 : tz - 1);
    return (uint16_t)(tz | ((phase / quantize) << 4));
}

enum ext_kind {
    EXT_NONE, EXT_SPEC, EXT_TXVER, EXT_GENESIS, EXT_MORTALITY, EXT_NONCE,
    EXT_TIP, EXT_METADATA_HASH
};

static const struct {
    const char   *name;
    enum ext_kind kind;
} known_ext[] = {
    { "CheckNonZeroSender", EXT_NONE },
    { "CheckSpecVersion", EXT_SPEC },
    { "CheckTxVersion", EXT_TXVER },
    { "CheckGenesis", EXT_GENESIS },
    { "CheckMortality", EXT_MORTALITY },
    { "CheckEra", EXT_MORTALITY },
    { "CheckNonce", EXT_NONCE },
    { "CheckWeight", EXT_NONE },
    { "ChargeTransactionPayment", EXT_TIP },
    { "CheckMetadataHash", EXT_METADATA_HASH },
};

/* Appends one extension's explicit and implicit data, by the rules in the plan. */
static int encode_ext(pdot_client *c, const pdot_tx_params *p,
                      const pdot_md_extension *e, pdot_buf *explicit_,
                      pdot_buf *implicit)
{
    size_t i;

    for (i = 0; i < sizeof known_ext / sizeof known_ext[0]; i++) {
        if (!pdot_md_name_eq(e->name, e->name_len, known_ext[i].name))
            continue;
        switch (known_ext[i].kind) {
        case EXT_NONE:
            break;
        case EXT_SPEC:
            pdot_scale_u32(implicit, p->spec_version);
            break;
        case EXT_TXVER:
            pdot_scale_u32(implicit, p->tx_version);
            break;
        case EXT_GENESIS:
            pdot_scale_raw(implicit, p->genesis, 32);
            break;
        case EXT_MORTALITY:
            if (p->era_period) {
                pdot_scale_u16(explicit_, pdot_era_mortal(p->era_period, p->era_block));
                pdot_scale_raw(implicit, p->era_block_hash, 32);
            } else {
                pdot_scale_u8(explicit_, 0);  /* Era::Immortal */
                pdot_scale_raw(implicit, p->genesis, 32);
            }
            break;
        case EXT_NONCE:
            pdot_scale_compact(explicit_, p->nonce);
            break;
        case EXT_TIP:
            pdot_scale_compact(explicit_, p->tip);
            break;
        case EXT_METADATA_HASH:
            pdot_scale_u8(explicit_, 0);  /* Mode::Disabled */
            pdot_scale_u8(implicit, 0);   /* None */
            break;
        }
        return 0;
    }
    if (pdot_md_is_zero_sized(p->md, e->ty) && pdot_md_is_zero_sized(p->md, e->implicit_ty))
        return 0;
    return pdot_fail(c, PDOT_E_METADATA,
                     "transaction extension %.*s carries data; add it to the known table",
                     (int)e->name_len, e->name);
}

/* MultiAddress::Id and the MultiSignature variant, from v15 metadata when present. */
static int address_and_sig_variants(pdot_client *c, const pdot_tx_params *p,
                                    const pdot_signer *s, uint8_t *addr_id,
                                    uint8_t *sig_variant)
{
    const char *scheme = s->scheme == PDOT_SCHEME_SR25519 ? "Sr25519" : "Ecdsa";

    *addr_id = 0;
    *sig_variant = s->scheme;
    if (!p->md->have_xt_types)
        return 0;
    if (pdot_md_variant(p->md, p->md->address_ty, "Id", addr_id, NULL))
        return pdot_fail(c, PDOT_E_METADATA, "address type has no Id variant");
    if (pdot_md_variant(p->md, p->md->signature_ty, scheme, sig_variant, NULL))
        return pdot_fail(c, PDOT_E_METADATA, "signature type has no %s variant", scheme);
    return 0;
}

int pdot_tx_build(pdot_client *c, const pdot_tx_params *p, const pdot_signer *s,
                  const uint8_t *call, size_t call_len, pdot_buf *out)
{
    pdot_buf ext, imp, payload, body;
    uint8_t sig[PDOT_SIG_MAX], digest[32], addr_id, sig_variant;
    size_t i;
    int rc = 0;

    if (!p || !p->md || !s || !s->sign || !call || !out)
        return PDOT_E_INVALID;
    if (p->md->extrinsic_version != 4)
        return pdot_fail(c, PDOT_E_UNSUPPORTED, "extrinsic version %u",
                         p->md->extrinsic_version);
    if ((rc = address_and_sig_variants(c, p, s, &addr_id, &sig_variant)))
        return rc;

    pdot_buf_init(&ext);
    pdot_buf_init(&imp);
    pdot_buf_init(&payload);
    pdot_buf_init(&body);

    for (i = 0; i < p->md->next && !rc; i++)
        rc = encode_ext(c, p, &p->md->ext[i], &ext, &imp);

    /* Signing payload: call | explicit | implicit, hashed when over 256 bytes. */
    if (!rc) {
        pdot_scale_raw(&payload, call, call_len);
        pdot_scale_raw(&payload, ext.data, ext.len);
        pdot_scale_raw(&payload, imp.data, imp.len);
        if ((rc = payload.err))
            ;
        else if (payload.len > 256) {
            pdot_blake2b_256(digest, payload.data, payload.len);
            rc = pdot_signer_sign(s, digest, 32, sig);
        } else {
            rc = pdot_signer_sign(s, payload.data, payload.len, sig);
        }
        if (rc)
            rc = pdot_fail(c, rc, "signing failed: %s", pdot_strerror(rc));
    }

    if (!rc) {
        pdot_scale_u8(&body, 0x84);  /* signed, version 4 */
        pdot_scale_u8(&body, addr_id);
        pdot_scale_raw(&body, s->account_id, 32);
        pdot_scale_u8(&body, sig_variant);
        pdot_scale_raw(&body, sig, s->sig_len);
        pdot_scale_raw(&body, ext.data, ext.len);
        pdot_scale_raw(&body, call, call_len);
        pdot_scale_bytes(out, body.data, body.len);
        rc = body.err ? body.err : out->err;
    }

    pdot_buf_free(&ext);
    pdot_buf_free(&imp);
    pdot_buf_free(&payload);
    pdot_buf_free(&body);
    return rc;
}

int pdot_tx_params_fetch(pdot_client *c, const pdot_signer *s, pdot_tx_params *p)
{
    int rc;

    memset(p, 0, sizeof *p);
    if ((rc = pdot_chain_sync(c)))
        return rc;
    p->md = c->md;
    memcpy(p->genesis, c->genesis, 32);
    p->spec_version = c->spec_version;
    p->tx_version = c->tx_version;
    p->era_period = PDOT_ERA_PERIOD;
    if ((rc = pdot_rpc_account_nonce(c, s->account_id, &p->nonce)) ||
        (rc = pdot_rpc_finalized_head(c, p->era_block_hash)) ||
        (rc = pdot_rpc_block_number(c, p->era_block_hash, &p->era_block)))
        return rc;
    return 0;
}

/* Looks for an extrinsic hashing to tx_hash in block `number`. */
static int find_in_block(pdot_client *c, uint32_t number, const uint8_t tx_hash[32],
                         size_t xt_len, pdot_tx_result *out, int *found)
{
    cJSON *xts, *item;
    uint32_t idx = 0;
    int rc;

    *found = 0;
    if ((rc = pdot_rpc_block_hash(c, number, out->block_hash)) ||
        (rc = pdot_rpc_block_extrinsics(c, out->block_hash, &xts)))
        return rc;
    cJSON_ArrayForEach(item, xts) {
        uint8_t *raw, h[32];
        size_t n;

        /* Only same-length candidates are decoded and hashed. */
        if (cJSON_IsString(item) && strlen(item->valuestring) == 2 + 2 * xt_len &&
            pdot_hex_decode_alloc(item->valuestring, &raw, &n) == 0) {
            pdot_blake2b_256(h, raw, n);
            free(raw);
            if (memcmp(h, tx_hash, 32) == 0) {
                out->block = number;
                out->index = idx;
                *found = 1;
                break;
            }
        }
        idx++;
    }
    cJSON_Delete(xts);
    return 0;
}

/* Fills out->dispatch and out->error from the block's System.Events. */
static int read_outcome(pdot_client *c, const pdot_tx_params *p, pdot_tx_result *out,
                        pdot_ext_outcome *oc)
{
    uint8_t key[32], *events;
    size_t len;
    int rc;

    out->dispatch = PDOT_DISPATCH_UNKNOWN;
    pdot_events_key(key);
    rc = pdot_rpc_storage_at(c, key, sizeof key, out->block_hash, &events, &len);
    if (rc)
        return rc == PDOT_E_RPC || rc == PDOT_E_NOTFOUND ? 0 : rc;  /* pruned */
    rc = pdot_events_outcome(p->md, events, len, out->index, oc);
    free(events);
    if (rc)
        return pdot_fail(c, rc, "cannot decode events of block #%u: %s", out->block,
                         pdot_strerror(rc));
    if (oc->failed) {
        out->dispatch = PDOT_DISPATCH_FAILED;
        snprintf(out->error, sizeof out->error, "%s", oc->error);
        return pdot_fail(c, PDOT_E_DISPATCH, "%s", oc->error);
    }
    if (oc->success)
        out->dispatch = PDOT_DISPATCH_OK;
    return 0;
}

int pdot_tx_submit_watch(pdot_client *c, const pdot_tx_params *p,
                         const uint8_t *xt, size_t len, pdot_tx_result *out,
                         pdot_ext_outcome *oc)
{
    pdot_ext_outcome local;
    uint8_t node_hash[32], head[32];
    uint32_t next = p->era_block + 1, finalized;
    uint32_t last = p->era_block + (p->era_period ? p->era_period : PDOT_ERA_PERIOD);
    int rc, found;

    memset(out, 0, sizeof *out);
    pdot_blake2b_256(out->tx_hash, xt, len);
    if ((rc = pdot_rpc_submit(c, xt, len, node_hash)))
        return rc;
    if (memcmp(node_hash, out->tx_hash, 32) != 0)
        return pdot_fail(c, PDOT_E_RPC, "node reported a different transaction hash");

    for (;;) {
        if ((rc = pdot_rpc_finalized_head(c, head)) ||
            (rc = pdot_rpc_block_number(c, head, &finalized)))
            return rc;
        for (; next <= finalized; next++) {
            if ((rc = find_in_block(c, next, out->tx_hash, len, out, &found)))
                return rc;
            if (found)
                return read_outcome(c, p, out, oc ? oc : &local);
            if (next >= last)
                return pdot_fail(c, PDOT_E_TIMEOUT,
                                 "not included by block %u (mortality window)", last);
        }
        pdot_sleep_ms(c->poll_ms);
    }
}
