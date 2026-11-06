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

/* ---- Trie roots ---------------------------------------------------------- */

/* Values this long or longer are stored hashed in a V1 trie (sp-trie threshold). */
#define V1_HASHED_VALUE 33

typedef struct {
    uint8_t        key[5];  /* Compact<u32> */
    size_t         key_len;
    const uint8_t *value;
    size_t         value_len;
} root_entry;

static int entry_cmp(const void *a, const void *b)
{
    const root_entry *x = a, *y = b;
    size_t n = x->key_len < y->key_len ? x->key_len : y->key_len;
    int c = memcmp(x->key, y->key, n);

    return c ? c : (x->key_len > y->key_len) - (x->key_len < y->key_len);
}

/* Header byte(s): prefix bits, then the nibble count (size_and_prefix_iterator). */
static void put_header(pdot_buf *b, uint8_t prefix, unsigned prefix_bits, size_t nibbles)
{
    size_t max = 255u >> prefix_bits, rem;

    if (nibbles > 65535)
        nibbles = 65535;
    if (nibbles < max) {
        pdot_scale_u8(b, (uint8_t)(prefix | nibbles));
        return;
    }
    pdot_scale_u8(b, (uint8_t)(prefix | max));
    for (rem = nibbles - max; rem >= 255; rem -= 255)
        pdot_scale_u8(b, 255);
    pdot_scale_u8(b, (uint8_t)rem);
}

/* Nibbles [from, to) of key, packed with a leading zero nibble when odd. */
static void put_partial(pdot_buf *b, const uint8_t *key, size_t from, size_t to)
{
    size_t i = from;

    if ((to - from) % 2)
        pdot_scale_u8(b, nibble_at(key, i++));
    for (; i < to; i += 2)
        pdot_scale_u8(b, (uint8_t)(nibble_at(key, i) << 4 | nibble_at(key, i + 1)));
}

static void put_value(pdot_buf *b, const uint8_t *v, size_t len, int hashed)
{
    uint8_t h[32];

    if (hashed) {
        pdot_blake2b_256(h, v, len);
        pdot_scale_raw(b, h, 32);
    } else {
        pdot_scale_bytes(b, v, len);
    }
}

/* Encodes the node for sorted entries whose keys share nibbles [0, cursor). */
static void build_node(const root_entry *e, size_t n, size_t cursor, int v1, pdot_buf *out,
                       int depth)
{
    size_t shared, first_nibbles = e[0].key_len * 2, i, start;
    const root_entry *last = &e[n - 1];
    const uint8_t *value = NULL;
    size_t value_len = 0;
    uint16_t bitmap = 0;
    int has_value, hashed;

    if (depth > MAX_DEPTH) {
        out->err = PDOT_E_INVALID;
        return;
    }
    if (n == 1) {
        hashed = v1 && e[0].value_len >= V1_HASHED_VALUE;
        if (hashed)
            put_header(out, 0x20, 3, first_nibbles - cursor);
        else
            put_header(out, 0x40, 2, first_nibbles - cursor);
        put_partial(out, e[0].key, cursor, first_nibbles);
        put_value(out, e[0].value, e[0].value_len, hashed);
        return;
    }

    /* Sorted: what the first and last keys share, all of them share. */
    for (shared = cursor; shared < first_nibbles && shared < last->key_len * 2 &&
                          nibble_at(e[0].key, shared) == nibble_at(last->key, shared);
         shared++)
        ;
    start = 0;
    has_value = shared == first_nibbles;  /* the first key ends here: the branch's value */
    if (has_value) {
        value = e[0].value;
        value_len = e[0].value_len;
        start = 1;
    }
    for (i = start; i < n; i++)
        bitmap |= (uint16_t)(1u << nibble_at(e[i].key, shared));

    hashed = has_value && v1 && value_len >= V1_HASHED_VALUE;
    if (hashed)
        put_header(out, 0x10, 4, shared - cursor);
    else
        put_header(out, has_value ? 0xc0 : 0x80, 2, shared - cursor);
    put_partial(out, e[0].key, cursor, shared);
    pdot_scale_u8(out, (uint8_t)(bitmap & 0xff));
    pdot_scale_u8(out, (uint8_t)(bitmap >> 8));
    if (has_value)
        put_value(out, value, value_len, hashed);

    /* Children in nibble order; sorted keys keep each group together. */
    for (i = start; i < n;) {
        uint8_t nib = nibble_at(e[i].key, shared);
        size_t j = i;
        pdot_buf child;

        while (j < n && nibble_at(e[j].key, shared) == nib)
            j++;
        pdot_buf_init(&child);
        build_node(e + i, j - i, shared + 1, v1, &child, depth + 1);
        if (child.err) {
            out->err = child.err;
        } else if (child.len >= 32) {
            uint8_t h[32];
            pdot_blake2b_256(h, child.data, child.len);
            pdot_scale_bytes(out, h, 32);
        } else {
            pdot_scale_bytes(out, child.data, child.len);  /* small nodes are inlined */
        }
        pdot_buf_free(&child);
        i = j;
    }
}

int pdot_trie_ordered_root(const uint8_t *const *values, const size_t *lens, size_t n,
                           int state_version, uint8_t root[32])
{
    root_entry *e;
    pdot_buf b;
    size_t i;
    int rc;

    if (n == 0) {
        static const uint8_t empty = 0;
        pdot_blake2b_256(root, &empty, 1);
        return 0;
    }
    if (n > UINT32_MAX || !(e = calloc(n, sizeof *e)))
        return PDOT_E_NOMEM;
    for (i = 0; i < n; i++) {
        pdot_buf k;
        pdot_buf_init(&k);
        pdot_scale_compact(&k, (uint64_t)i);
        if (k.err || k.len > sizeof e[i].key) {
            pdot_buf_free(&k);
            free(e);
            return PDOT_E_NOMEM;
        }
        memcpy(e[i].key, k.data, k.len);
        e[i].key_len = k.len;
        e[i].value = values[i];
        e[i].value_len = lens[i];
        pdot_buf_free(&k);
    }
    qsort(e, n, sizeof *e, entry_cmp);
    pdot_buf_init(&b);
    build_node(e, n, 0, state_version == 1, &b, 0);
    if (!(rc = b.err))
        pdot_blake2b_256(root, b.data, b.len);
    pdot_buf_free(&b);
    free(e);
    return rc;
}
