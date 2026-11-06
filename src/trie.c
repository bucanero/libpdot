#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hash.h"
#include "scale.h"
#include "trie.h"

/* sp-trie node header prefixes (node_header.rs). */
enum node_kind { NODE_EMPTY, NODE_LEAF, NODE_BRANCH, NODE_BRANCH_VALUE,
                 NODE_HASHED_LEAF, NODE_HASHED_BRANCH };

#define MAX_DEPTH 128

void pdot_trie_proof_free(pdot_trie_proof *p)
{
    size_t i;

    for (i = 0; i < p->n; i++)
        free((void *)p->nodes[i].data);
    free(p->nodes);
    p->nodes = NULL;
    p->n = 0;
}

void pdot_trie_hash_nodes(pdot_trie_node *nodes, size_t nnodes)
{
    size_t i;

    for (i = 0; i < nnodes; i++)
        pdot_blake2b_256(nodes[i].hash, nodes[i].data, nodes[i].len);
}

static const pdot_trie_node *find(const pdot_trie_node *nodes, size_t n, const uint8_t h[32])
{
    size_t i;

    for (i = 0; i < n; i++)
        if (memcmp(nodes[i].hash, h, 32) == 0)
            return &nodes[i];
    return NULL;
}

/* Nibble count after the prefix bits: values at the mask's top spill into extra bytes. */
static int decode_size(uint8_t first, unsigned prefix_bits, pdot_reader *r, size_t *n)
{
    size_t max = 255u >> prefix_bits, v = first & max;
    uint8_t b;

    if (v < max) {
        *n = v;
        return 0;
    }
    for (;;) {
        if (pdot_read_u8(r, &b))
            return PDOT_E_DECODE;
        if (b < 255) {
            *n = v + b;
            return 0;
        }
        v += 255;
        if (v > 65535)
            return PDOT_E_DECODE;
    }
}

static int read_header(pdot_reader *r, enum node_kind *kind, size_t *nibbles)
{
    uint8_t b;

    if (pdot_read_u8(r, &b))
        return PDOT_E_DECODE;
    if (b == 0) {
        *kind = NODE_EMPTY;
        *nibbles = 0;
        return 0;
    }
    switch (b >> 6) {
    case 1: *kind = NODE_LEAF; return decode_size(b, 2, r, nibbles);
    case 2: *kind = NODE_BRANCH; return decode_size(b, 2, r, nibbles);
    case 3: *kind = NODE_BRANCH_VALUE; return decode_size(b, 2, r, nibbles);
    }
    if ((b >> 5) == 1) {
        *kind = NODE_HASHED_LEAF;
        return decode_size(b, 3, r, nibbles);
    }
    if ((b >> 4) == 1) {
        *kind = NODE_HASHED_BRANCH;
        return decode_size(b, 4, r, nibbles);
    }
    return PDOT_E_DECODE;
}

static uint8_t nibble_at(const uint8_t *bytes, size_t i)
{
    return (uint8_t)(i % 2 ? bytes[i / 2] & 15 : bytes[i / 2] >> 4);
}

/*
 * Compares a node's partial key (packed, left-padded to whole bytes) with
 * the key nibbles from position at. Returns 1 when they match.
 */
static int partial_matches(const uint8_t *partial, size_t count, const uint8_t *key,
                           size_t key_nibbles, size_t at)
{
    size_t pad = count % 2, i;

    if (key_nibbles - at < count)
        return 0;
    for (i = 0; i < count; i++)
        if (nibble_at(partial, i + pad) != nibble_at(key, at + i))
            return 0;
    return 1;
}

/* A value field: inline (compact length + bytes) or a 32-byte hash to resolve. */
static int read_value(pdot_reader *r, int hashed, const pdot_trie_node *nodes, size_t n,
                      const uint8_t **v, size_t *vlen)
{
    const uint8_t *h;
    const pdot_trie_node *node;

    if (!hashed)
        return pdot_read_bytes(r, v, vlen) ? PDOT_E_DECODE : 0;
    if (pdot_read_raw(r, 32, &h))
        return PDOT_E_DECODE;
    if (!(node = find(nodes, n, h)))
        return PDOT_E_MISMATCH;  /* proof lacks the value node */
    *v = node->data;
    *vlen = node->len;
    return 0;
}

int pdot_trie_read(const uint8_t root[32], const pdot_trie_node *nodes, size_t nnodes,
                   const uint8_t *key, size_t key_len,
                   const uint8_t **value, size_t *value_len)
{
    const pdot_trie_node *node = find(nodes, nnodes, root);
    const uint8_t *data;
    size_t len, at = 0, key_nibbles = key_len * 2;
    int depth;

    if (!node)
        return PDOT_E_MISMATCH;
    data = node->data;
    len = node->len;

    for (depth = 0; depth < MAX_DEPTH; depth++) {
        pdot_reader r;
        enum node_kind kind;
        size_t nib, i;
        const uint8_t *partial;
        uint16_t bitmap;

        pdot_reader_init(&r, data, len);
        if (read_header(&r, &kind, &nib))
            return PDOT_E_DECODE;
        if (kind == NODE_EMPTY)
            return PDOT_E_NOTFOUND;
        if (pdot_read_raw(&r, (nib + 1) / 2, &partial))
            return PDOT_E_DECODE;
        if (nib % 2 && (partial[0] >> 4) != 0)
            return PDOT_E_DECODE;  /* padding nibble must be zero */

        if (kind == NODE_LEAF || kind == NODE_HASHED_LEAF) {
            if (key_nibbles - at != nib || !partial_matches(partial, nib, key, key_nibbles, at))
                return PDOT_E_NOTFOUND;
            return read_value(&r, kind == NODE_HASHED_LEAF, nodes, nnodes, value, value_len);
        }

        /* Branch: partial key, children bitmap, optional value, children. */
        if (!partial_matches(partial, nib, key, key_nibbles, at))
            return PDOT_E_NOTFOUND;
        at += nib;
        if (pdot_read_u16(&r, &bitmap))
            return PDOT_E_DECODE;
        if (at == key_nibbles) {
            if (kind == NODE_BRANCH)
                return PDOT_E_NOTFOUND;
            return read_value(&r, kind == NODE_HASHED_BRANCH, nodes, nnodes, value, value_len);
        }
        if (kind != NODE_BRANCH) {
            const uint8_t *v;
            size_t vl;
            if (kind == NODE_HASHED_BRANCH ? pdot_read_skip(&r, 32) : pdot_read_bytes(&r, &v, &vl))
                return PDOT_E_DECODE;
        }
        {
            uint8_t want = nibble_at(key, at);
            const uint8_t *child = NULL;
            size_t child_len = 0;

            if (!(bitmap & (1u << want)))
                return PDOT_E_NOTFOUND;
            for (i = 0; i <= want; i++) {
                const uint8_t *c;
                size_t cl;
                if (!(bitmap & (1u << i)))
                    continue;
                if (pdot_read_bytes(&r, &c, &cl))
                    return PDOT_E_DECODE;
                if (i == want) {
                    child = c;
                    child_len = cl;
                }
            }
            at++;
            if (child_len == 32) {
                if (!(node = find(nodes, nnodes, child)))
                    return PDOT_E_MISMATCH;  /* proof lacks the child */
                data = node->data;
                len = node->len;
            } else if (child_len < 32) {
                data = child;  /* small nodes are embedded in their parent */
                len = child_len;
            } else {
                return PDOT_E_DECODE;
            }
        }
    }
    return PDOT_E_DECODE;
}
