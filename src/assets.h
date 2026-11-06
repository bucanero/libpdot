/*
 * Typed reads of chain state (balances, pallet-assets, pallet-nfts) as
 * queries: the storage keys to read and how to decode them. One-shot reads
 * and pdot_async watches share them.
 */
#ifndef PDOT_ASSETS_H
#define PDOT_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#include "pdot.h"
#include "client.h"
#include "jsonrpc.h"
#include "scale.h"

enum pdot_query_kind {
    PDOT_Q_STORAGE,        /* pdot_storage_value */
    PDOT_Q_BALANCE,        /* pdot_balance */
    PDOT_Q_ASSET_BALANCE,  /* pdot_asset_balance */
    PDOT_Q_ASSET_INFO,     /* pdot_asset_info */
    PDOT_Q_NFT_ITEM,       /* pdot_nft_item */
    PDOT_Q_NFT_ATTRIBUTE,  /* pdot_nft_attribute */
    PDOT_Q_PROXIES         /* pdot_proxies */
};

#define PDOT_Q_MAX_KEYS 2

typedef struct {
    int            kind;
    uint32_t       id;          /* asset id or collection */
    uint32_t       item;
    int            has_item;    /* attribute on an item, not the collection */
    int            ns;          /* PDOT_NFT_NS_* */
    uint8_t        account[32]; /* account, or the namespace's account */
    const uint8_t *bytes;       /* raw key, or the attribute key; not owned */
    size_t         len;
} pdot_query;

/* Storage keys q reads, from the client's current metadata. */
int pdot_query_keys(pdot_client *c, const pdot_query *q, pdot_buf keys[PDOT_Q_MAX_KEYS],
                    size_t *nkeys);

/*
 * Decodes the values of q's keys into its result type, read at `at`. A
 * PDOT_Q_STORAGE result takes over values[0].data.
 */
int pdot_query_decode(pdot_client *c, const pdot_query *q, pdot_value *values,
                      const pdot_final_block *at, void *out);

/* sizeof the result type of a query kind. */
size_t pdot_query_result_size(int kind);

/* Syncs metadata, reads q at the newest final block and decodes it. */
int pdot_query_run(pdot_client *c, const pdot_query *q, void *out);

#endif
