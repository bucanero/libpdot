/*
 * Storage keys and reads at the newest final block: proven through the
 * verifier when one is set, else taken from the RPC node.
 */
#ifndef PDOT_STORAGE_H
#define PDOT_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "pdot.h"
#include "client.h"
#include "grandpa.h"
#include "metadata.h"
#include "rpc.h"
#include "scale.h"

/* One SCALE-encoded key of a storage map. */
typedef struct {
    const uint8_t *p;
    size_t         len;
} pdot_key_part;

/* Appends hasher(k), e.g. Blake2_128Concat, to key. */
int pdot_storage_hash(pdot_buf *key, uint8_t hasher, const uint8_t *k, size_t len);

/*
 * twox128(prefix) | twox128(entry) | hasher_i(parts[i])... for pallet.entry.
 * Fewer parts than the map has keys gives a prefix for iteration.
 */
int pdot_storage_key(pdot_client *c, const pdot_metadata *md, const char *pallet,
                     const char *entry, const pdot_key_part *parts, size_t nparts,
                     pdot_md_storage *st, pdot_buf *key);

/*
 * The newest final block. With a verifier, *head is its proven header;
 * without, NULL and the block is the RPC node's finalized head.
 */
int pdot_final_block_get(pdot_client *c, pdot_final_block *at, const pdot_header **head);

/* Reads keys at that block, through one read proof when head is set. */
int pdot_storage_read_at(pdot_client *c, const pdot_final_block *at, const pdot_header *head,
                         const uint8_t *const *keys, const size_t *lens, size_t nkeys,
                         pdot_value *values);

#endif
