/* Signed extrinsic (v4) construction and submission. */
#ifndef PDOT_TX_H
#define PDOT_TX_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"
#include "events.h"
#include "metadata.h"
#include "scale.h"

#define PDOT_ERA_PERIOD 64

/* Everything a signed extrinsic depends on besides the call and the key. */
typedef struct {
    const pdot_metadata *md;
    uint8_t  genesis[32];
    uint32_t spec_version;
    uint32_t tx_version;
    uint32_t nonce;
    uint64_t tip;
    uint32_t era_period;        /* 0: immortal */
    uint32_t era_block;         /* mortality checkpoint */
    uint8_t  era_block_hash[32];
} pdot_tx_params;

/* Two-byte mortal Era for a transaction checkpointed at block `current`. */
uint16_t pdot_era_mortal(uint32_t period, uint32_t current);

/*
 * Encodes the transaction extensions, signs and assembles
 * compact(len) | 0x84 | address | signature | extensions | call into out.
 * c is used for error messages only and may be NULL.
 */
int pdot_tx_build(pdot_client *c, const pdot_tx_params *p, const pdot_signer *s,
                  const uint8_t *call, size_t call_len, pdot_buf *out);

/*
 * Submits xt and polls finalized blocks after p->era_block until one contains
 * it, then reads the block's events into out (and oc, if not NULL).
 * PDOT_E_TIMEOUT after the mortality window; PDOT_E_DISPATCH if the call failed.
 */
int pdot_tx_submit_watch(pdot_client *c, const pdot_tx_params *p,
                         const uint8_t *xt, size_t len, pdot_tx_result *out,
                         pdot_ext_outcome *oc);

/* Fills p from the node: metadata, versions, nonce and a finalized checkpoint. */
int pdot_tx_params_fetch(pdot_client *c, const pdot_signer *s, pdot_tx_params *p);

#endif
