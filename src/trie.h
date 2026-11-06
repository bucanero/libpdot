/*
 * Substrate base-16 Merkle-Patricia trie read-proof verification
 * (sp-trie NodeCodec, state versions V0 and V1, Blake2b-256 hashing).
 */
#ifndef PDOT_TRIE_H
#define PDOT_TRIE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t         len;
    uint8_t        hash[32];
} pdot_trie_node;

/* A read proof as returned by state_getReadProof; owns its node data. */
typedef struct {
    pdot_trie_node *nodes;
    size_t          n;
} pdot_trie_proof;

void pdot_trie_proof_free(pdot_trie_proof *p);

/*
 * Looks key up in the trie with the given root, using only the proof nodes.
 * 0 and *value (pointing into a proof node) when present; PDOT_E_NOTFOUND when
 * the proof shows the key absent; PDOT_E_MISMATCH when the proof is
 * incomplete or does not hash to root; PDOT_E_DECODE when a node is malformed.
 */
int pdot_trie_read(const uint8_t root[32], const pdot_trie_node *nodes, size_t nnodes,
                   const uint8_t *key, size_t key_len,
                   const uint8_t **value, size_t *value_len);

/* Hashes each node; call once before pdot_trie_read. */
void pdot_trie_hash_nodes(pdot_trie_node *nodes, size_t nnodes);

/*
 * Root of the trie mapping Compact<u32>(i) to values[i], as a block header's
 * extrinsics_root is built (sp-trie ordered_trie_root). state_version 1
 * stores values of 33 bytes or more hashed; 0 stores every value inline.
 */
int pdot_trie_ordered_root(const uint8_t *const *values, const size_t *lens, size_t n,
                           int state_version, uint8_t root[32]);

#endif
