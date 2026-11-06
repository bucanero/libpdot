/*
 * RFC-0078 merkleized metadata, following the merkleized-metadata crate
 * (from_frame_metadata.rs, merkle_tree.rs):
 *
 *  1. Collect the types reachable from the extrinsic types and extensions,
 *     not descending into Compact or BitSequence.
 *  2. "Basic" types are the reachable ones other than primitives, Compact,
 *     and empty composites, enums and tuples. They get new ids 0.. in
 *     registry order; the others become TypeRef primitives or Void.
 *  3. Each basic type is one leaf (an enum: one leaf per variant, by index),
 *     hashed with blake3 and combined into a complete binary tree.
 *  4. The digest binds the tree root, the extrinsic metadata and the chain
 *     constants; its blake3 is the metadata hash.
 */
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hash.h"
#include "mdhash.h"

#define TRY(x) do { if ((rc = (x)) != 0) goto done; } while (0)
#define DEC(x) do { if ((x) != 0) { rc = PDOT_E_DECODE; goto done; } } while (0)

#define NO_ID      0xffffffffu
#define MAX_DEPTH  256

/* TypeRef variant indices. */
enum {
    REF_U8 = 3, REF_COMPACT_U8 = 15, REF_VOID = 21, REF_BY_ID = 22
};

/* scale-info primitives, which TypeRef numbers the same way. */
enum { PRIM_U8 = 3, PRIM_U16, PRIM_U32, PRIM_U64, PRIM_U128, PRIM_U256 };

typedef struct {
    const pdot_metadata *md;
    uint32_t *new_id;     /* frame id -> basic type id, or NO_ID */
    uint32_t *seen;       /* primitive collector: visit generation per type */
    uint32_t  gen;
    uint8_t   prims[2];   /* first primitives found, and how many */
    size_t    nprims;
} ctx_t;

static int read_u32c(pdot_reader *r, uint32_t *v)
{
    uint64_t t;

    if (pdot_read_compact(r, &t) || t > 0xffffffffu)
        return PDOT_E_DECODE;
    *v = (uint32_t)t;
    return 0;
}

static int skip_strs(pdot_reader *r)
{
    const uint8_t *p;
    size_t len;
    uint64_t n, i;

    if (pdot_read_compact(r, &n))
        return PDOT_E_DECODE;
    for (i = 0; i < n; i++)
        if (pdot_read_bytes(r, &p, &len))
            return PDOT_E_DECODE;
    return 0;
}

static int skip_opt_str(pdot_reader *r)
{
    const uint8_t *p;
    size_t len;
    uint8_t some;

    if (pdot_read_u8(r, &some) || some > 1)
        return PDOT_E_DECODE;
    return some && pdot_read_bytes(r, &p, &len) ? PDOT_E_DECODE : 0;
}

/* Field { name, ty, type_name, docs }: returns ty. */
static int skip_field(pdot_reader *r, uint32_t *ty)
{
    if (skip_opt_str(r) || read_u32c(r, ty) || skip_opt_str(r) || skip_strs(r))
        return PDOT_E_DECODE;
    return 0;
}

/*
 * Positions r after the kind byte of ty's TypeDef. *count is the number of
 * fields, variants or tuple elements, so empty types can be told apart.
 */
static int typedef_at(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                      enum pdot_typedef *kind, uint64_t *count)
{
    if (pdot_md_typedef(md, ty, r, kind))
        return PDOT_E_DECODE;
    *count = 0;
    if (*kind == PDOT_TYPE_COMPOSITE || *kind == PDOT_TYPE_VARIANT || *kind == PDOT_TYPE_TUPLE) {
        size_t save = r->pos;
        if (pdot_read_compact(r, count))
            return PDOT_E_DECODE;
        r->pos = save;
    }
    return 0;
}

static int is_basic(enum pdot_typedef kind, uint64_t count)
{
    switch (kind) {
    case PDOT_TYPE_PRIMITIVE:
    case PDOT_TYPE_COMPACT:
        return 0;
    case PDOT_TYPE_COMPOSITE:
    case PDOT_TYPE_VARIANT:
    case PDOT_TYPE_TUPLE:
        return count > 0;
    default:
        return 1;
    }
}

/* Child type ids of ty, for reachability: none through Compact or BitSequence. */
static int children(const pdot_metadata *md, uint32_t ty, uint32_t **out, size_t *n)
{
    pdot_reader r;
    enum pdot_typedef kind;
    uint64_t count, i, nf, j;
    uint32_t v, *list = NULL;
    size_t len = 0, cap = 0;
    uint8_t b;
    const uint8_t *p;
    size_t plen;
    int rc = 0;

#define PUSH(x) do { \
        if (len == cap) { \
            uint32_t *t = realloc(list, (cap = cap ? 2 * cap : 8) * sizeof *list); \
            if (!t) { rc = PDOT_E_NOMEM; goto done; } \
            list = t; \
        } \
        list[len++] = (x); \
    } while (0)

    TRY(typedef_at(md, ty, &r, &kind, &count));
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
        DEC(pdot_read_compact(&r, &nf));
        for (j = 0; j < nf; j++) {
            DEC(skip_field(&r, &v));
            PUSH(v);
        }
        break;
    case PDOT_TYPE_VARIANT:
        DEC(pdot_read_compact(&r, &count));
        for (i = 0; i < count; i++) {
            DEC(pdot_read_bytes(&r, &p, &plen));
            DEC(pdot_read_compact(&r, &nf));
            for (j = 0; j < nf; j++) {
                DEC(skip_field(&r, &v));
                PUSH(v);
            }
            DEC(pdot_read_u8(&r, &b));
            DEC(skip_strs(&r));
        }
        break;
    case PDOT_TYPE_SEQUENCE:
        DEC(read_u32c(&r, &v));
        PUSH(v);
        break;
    case PDOT_TYPE_ARRAY:
        DEC(pdot_read_u32(&r, &v));
        DEC(read_u32c(&r, &v));
        PUSH(v);
        break;
    case PDOT_TYPE_TUPLE:
        DEC(pdot_read_compact(&r, &count));
        for (i = 0; i < count; i++) {
            DEC(read_u32c(&r, &v));
            PUSH(v);
        }
        break;
    default:
        break;
    }
#undef PUSH
done:
    if (rc) {
        free(list);
        return rc;
    }
    *out = list;
    *n = len;
    return 0;
}

/* Marks every type reachable from root in acc, with an explicit stack. */
static int collect_accessible(const pdot_metadata *md, uint32_t root, uint8_t *acc,
                              uint32_t *stack)
{
    size_t sp = 0, n, i;
    uint32_t *kids;
    int rc;

    if (root >= md->ntypes)
        return PDOT_E_DECODE;
    if (acc[root])
        return 0;
    acc[root] = 1;
    stack[sp++] = root;
    while (sp) {
        uint32_t ty = stack[--sp];
        if ((rc = children(md, ty, &kids, &n)))
            return rc;
        for (i = 0; i < n; i++) {
            if (kids[i] >= md->ntypes) {
                free(kids);
                return PDOT_E_DECODE;
            }
            if (!acc[kids[i]]) {
                acc[kids[i]] = 1;
                stack[sp++] = kids[i];   /* each type is pushed once: sp < ntypes */
            }
        }
        free(kids);
    }
    return 0;
}

/*
 * The crate's CollectPrimitives: every primitive reachable from ty's
 * definition, visiting each type id at most once. Only the first two
 * matter: callers need exactly one.
 */
static int visit_def(ctx_t *x, uint32_t ty, int depth);

static int visit(ctx_t *x, uint32_t ty, int depth)
{
    if (ty >= x->md->ntypes)
        return PDOT_E_DECODE;
    if (x->seen[ty] == x->gen)
        return 0;
    x->seen[ty] = x->gen;
    return visit_def(x, ty, depth + 1);
}

static int visit_def(ctx_t *x, uint32_t ty, int depth)
{
    pdot_reader r;
    enum pdot_typedef kind;
    uint64_t count, i, nf, j;
    uint32_t v, w;
    uint8_t b;
    const uint8_t *p;
    size_t plen;
    int rc = 0;

    if (depth > MAX_DEPTH)
        return PDOT_E_METADATA;
    TRY(typedef_at(x->md, ty, &r, &kind, &count));
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
        DEC(pdot_read_compact(&r, &nf));
        for (j = 0; j < nf; j++) {
            DEC(skip_field(&r, &v));
            TRY(visit(x, v, depth));
        }
        break;
    case PDOT_TYPE_VARIANT:
        DEC(pdot_read_compact(&r, &count));
        for (i = 0; i < count; i++) {
            DEC(pdot_read_bytes(&r, &p, &plen));
            DEC(pdot_read_compact(&r, &nf));
            for (j = 0; j < nf; j++) {
                DEC(skip_field(&r, &v));
                TRY(visit(x, v, depth));
            }
            DEC(pdot_read_u8(&r, &b));
            DEC(skip_strs(&r));
        }
        break;
    case PDOT_TYPE_SEQUENCE:
    case PDOT_TYPE_COMPACT:
        DEC(read_u32c(&r, &v));
        TRY(visit(x, v, depth));
        break;
    case PDOT_TYPE_ARRAY:
        DEC(pdot_read_u32(&r, &v));
        DEC(read_u32c(&r, &v));
        TRY(visit(x, v, depth));
        break;
    case PDOT_TYPE_TUPLE:
        DEC(pdot_read_compact(&r, &count));
        for (i = 0; i < count; i++) {
            DEC(read_u32c(&r, &v));
            TRY(visit(x, v, depth));
        }
        break;
    case PDOT_TYPE_PRIMITIVE:
        DEC(pdot_read_u8(&r, &b));
        if (x->nprims < 2)
            x->prims[x->nprims] = b;
        x->nprims++;
        break;
    case PDOT_TYPE_BITSEQ:   /* bit_store_type, bit_order_type; order visited first */
        DEC(read_u32c(&r, &v));
        DEC(read_u32c(&r, &w));
        TRY(visit(x, w, depth));
        TRY(visit(x, v, depth));
        break;
    default:
        return PDOT_E_DECODE;
    }
done:
    return rc;
}

/* The single primitive reachable from ty, or 0 found / an error. */
static int single_primitive(ctx_t *x, uint32_t ty, uint8_t *prim, int *found)
{
    int rc;

    x->gen++;
    x->nprims = 0;
    if ((rc = visit_def(x, ty, 0)))
        return rc;
    if (x->nprims > 1)
        return PDOT_E_METADATA;
    *found = x->nprims == 1;
    *prim = x->prims[0];
    return 0;
}

/* TypeRef for a reference to frame type ty. */
static int type_ref(ctx_t *x, uint32_t ty, pdot_buf *out)
{
    pdot_reader r;
    enum pdot_typedef kind;
    uint64_t count;
    uint8_t b;
    int found, rc;

    if (ty >= x->md->ntypes || typedef_at(x->md, ty, &r, &kind, &count))
        return PDOT_E_DECODE;
    switch (kind) {
    case PDOT_TYPE_PRIMITIVE:
        if (pdot_read_u8(&r, &b) || b > 14)
            return PDOT_E_DECODE;
        pdot_scale_u8(out, b);
        return 0;
    case PDOT_TYPE_COMPACT:
        if ((rc = single_primitive(x, ty, &b, &found)))
            return rc;
        if (!found) {
            pdot_scale_u8(out, REF_VOID);
            return 0;
        }
        if (b < PRIM_U8 || b > PRIM_U256)
            return PDOT_E_METADATA;
        pdot_scale_u8(out, (uint8_t)(REF_COMPACT_U8 + (b - PRIM_U8)));
        return 0;
    default:
        if (!is_basic(kind, count)) {
            pdot_scale_u8(out, REF_VOID);
            return 0;
        }
        if (x->new_id[ty] == NO_ID)
            return PDOT_E_METADATA;   /* referenced but not reachable */
        pdot_scale_u8(out, REF_BY_ID);
        pdot_scale_compact(out, x->new_id[ty]);
        return 0;
    }
}

/* Option<String> copied from the reader. */
static int copy_opt_str(pdot_reader *r, pdot_buf *out)
{
    const uint8_t *p;
    size_t len;
    uint8_t some;

    if (pdot_read_u8(r, &some) || some > 1)
        return PDOT_E_DECODE;
    pdot_scale_u8(out, some);
    if (some) {
        if (pdot_read_bytes(r, &p, &len))
            return PDOT_E_DECODE;
        pdot_scale_bytes(out, p, len);
    }
    return 0;
}

/* Vec<Field> with each field as { name, TypeRef, type_name }. */
static int encode_fields(ctx_t *x, pdot_reader *r, pdot_buf *out)
{
    uint64_t n, i;
    uint32_t ty;
    int rc;

    if (pdot_read_compact(r, &n))
        return PDOT_E_DECODE;
    pdot_scale_compact(out, n);
    for (i = 0; i < n; i++) {
        if ((rc = copy_opt_str(r, out)) || (rc = read_u32c(r, &ty)) ||
            (rc = type_ref(x, ty, out)) || (rc = copy_opt_str(r, out)) ||
            (rc = skip_strs(r)))
            return rc;
    }
    return 0;
}

typedef struct {
    uint8_t (*h)[32];
    size_t  n, cap;
} leaves_t;

static int add_leaf(leaves_t *l, const pdot_buf *ty)
{
    if (ty->err)
        return ty->err;
    if (l->n == l->cap) {
        size_t cap = l->cap ? 2 * l->cap : 256;
        uint8_t (*t)[32] = realloc(l->h, cap * sizeof *t);
        if (!t)
            return PDOT_E_NOMEM;
        l->h = t;
        l->cap = cap;
    }
    pdot_blake3(l->h[l->n++], ty->data, ty->len);
    return 0;
}

typedef struct {
    uint8_t index;
    size_t  pos;    /* offset of the variant record */
} variant_ref;

static int cmp_variant(const void *a, const void *b)
{
    const variant_ref *x = a, *y = b;

    return (x->index > y->index) - (x->index < y->index);
}

/* Leaves for basic type ty: Type { path, type_def, type_id }. */
static int type_leaves(ctx_t *x, uint32_t ty, leaves_t *l)
{
    const pdot_metadata *md = x->md;
    pdot_reader r;
    enum pdot_typedef kind;
    uint64_t count, i;
    const uint8_t *path, *p;
    size_t path_len, plen;
    uint32_t v;
    uint8_t b;
    pdot_buf t;
    variant_ref *vs = NULL;
    int rc = 0;

    /* The path is Vec<String> in both encodings: copy its bytes. */
    pdot_reader_init(&r, md->blob, md->len);
    r.pos = md->type_off[ty];
    DEC(skip_strs(&r));
    path = md->blob + md->type_off[ty];
    path_len = r.pos - md->type_off[ty];

    TRY(typedef_at(md, ty, &r, &kind, &count));
    pdot_buf_init(&t);

    if (kind == PDOT_TYPE_VARIANT) {
        DEC(pdot_read_compact(&r, &count));
        if (count > 256 || !(vs = malloc(count * sizeof *vs))) {
            rc = count > 256 ? PDOT_E_DECODE : PDOT_E_NOMEM;
            goto done;
        }
        for (i = 0; i < count; i++) {   /* index each variant, then sort */
            uint64_t nf, j;
            vs[i].pos = r.pos;
            DEC(pdot_read_bytes(&r, &p, &plen));
            DEC(pdot_read_compact(&r, &nf));
            for (j = 0; j < nf; j++)
                DEC(skip_field(&r, &v));
            DEC(pdot_read_u8(&r, &vs[i].index));
            DEC(skip_strs(&r));
        }
        qsort(vs, (size_t)count, sizeof *vs, cmp_variant);
        for (i = 0; i < count; i++) {
            r.pos = vs[i].pos;
            t.len = 0;
            pdot_scale_raw(&t, path, path_len);
            pdot_scale_u8(&t, 1);                 /* TypeDef::Enumeration */
            DEC(pdot_read_bytes(&r, &p, &plen));
            pdot_scale_bytes(&t, p, plen);        /* name */
            TRY(encode_fields(x, &r, &t));
            pdot_scale_compact(&t, vs[i].index);
            pdot_scale_compact(&t, x->new_id[ty]);
            TRY(add_leaf(l, &t));
        }
        goto done;
    }

    pdot_scale_raw(&t, path, path_len);
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
        pdot_scale_u8(&t, 0);
        TRY(encode_fields(x, &r, &t));
        break;
    case PDOT_TYPE_SEQUENCE:
        pdot_scale_u8(&t, 2);
        DEC(read_u32c(&r, &v));
        TRY(type_ref(x, v, &t));
        break;
    case PDOT_TYPE_ARRAY:
        pdot_scale_u8(&t, 3);
        DEC(pdot_read_u32(&r, &v));
        pdot_scale_u32(&t, v);
        DEC(read_u32c(&r, &v));
        TRY(type_ref(x, v, &t));
        break;
    case PDOT_TYPE_TUPLE:
        pdot_scale_u8(&t, 4);
        DEC(pdot_read_compact(&r, &count));
        pdot_scale_compact(&t, count);
        for (i = 0; i < count; i++) {
            DEC(read_u32c(&r, &v));
            TRY(type_ref(x, v, &t));
        }
        break;
    case PDOT_TYPE_BITSEQ: {
        uint32_t store, order;
        const uint8_t *seg = NULL;
        size_t seg_len = 0;
        int found, lsb = -1;
        uint64_t n;

        DEC(read_u32c(&r, &store));
        DEC(read_u32c(&r, &order));
        TRY(single_primitive(x, store, &b, &found));
        if (!found || b < PRIM_U8 || b > PRIM_U64) {
            rc = PDOT_E_METADATA;
            goto done;
        }
        pdot_scale_u8(&t, 5);
        pdot_scale_u8(&t, (uint8_t)(1u << (b - PRIM_U8)));
        if (order >= md->ntypes) {
            rc = PDOT_E_DECODE;
            goto done;
        }
        /* The order type's path names Lsb0 or Msb0. */
        pdot_reader_init(&r, md->blob, md->len);
        r.pos = md->type_off[order];
        DEC(pdot_read_compact(&r, &n));
        for (i = 0; i < n && lsb < 0; i++) {
            DEC(pdot_read_bytes(&r, &seg, &seg_len));
            if (pdot_md_name_eq((const char *)seg, seg_len, "Lsb0"))
                lsb = 1;
            else if (pdot_md_name_eq((const char *)seg, seg_len, "Msb0"))
                lsb = 0;
        }
        if (lsb < 0) {
            rc = PDOT_E_METADATA;
            goto done;
        }
        pdot_scale_bool(&t, lsb);
        break;
    }
    default:
        rc = PDOT_E_METADATA;
        goto done;
    }
    pdot_scale_compact(&t, x->new_id[ty]);
    TRY(add_leaf(l, &t));
done:
    free(vs);
    pdot_buf_free(&t);
    return rc;
}

/* Root of the complete binary tree, built as a heap with the leaves last. */
static int tree_root(leaves_t *l, uint8_t root[32])
{
    size_t n = l->n, i;
    uint8_t (*h)[32], pair[64];

    if (n == 0) {
        memset(root, 0, 32);
        return 0;
    }
    if (!(h = malloc((2 * n - 1) * sizeof *h)))
        return PDOT_E_NOMEM;
    memcpy(h[n - 1], l->h, n * sizeof *h);
    for (i = n - 1; i-- > 0;) {
        memcpy(pair, h[2 * i + 1], 32);
        memcpy(pair + 32, h[2 * i + 2], 32);
        pdot_blake3(h[i], pair, 64);
    }
    memcpy(root, h[0], 32);
    free(h);
    return 0;
}

/* ExtrinsicMetadata { version, address_ty, call_ty, signature_ty, extensions }. */
static int extrinsic_hash(ctx_t *x, uint8_t out[32])
{
    const pdot_metadata *md = x->md;
    pdot_buf b;
    size_t i;
    int rc;

    pdot_buf_init(&b);
    pdot_scale_u8(&b, md->extrinsic_version);
    if ((rc = type_ref(x, md->address_ty, &b)) || (rc = type_ref(x, md->call_ty, &b)) ||
        (rc = type_ref(x, md->signature_ty, &b)))
        goto done;
    pdot_scale_compact(&b, md->next);
    for (i = 0; i < md->next; i++) {
        pdot_scale_bytes(&b, (const uint8_t *)md->ext[i].name, md->ext[i].name_len);
        if ((rc = type_ref(x, md->ext[i].ty, &b)) ||
            (rc = type_ref(x, md->ext[i].implicit_ty, &b)))
            goto done;
    }
    if (!(rc = b.err))
        pdot_blake3(out, b.data, b.len);
done:
    pdot_buf_free(&b);
    return rc;
}

int pdot_metadata_hash(const pdot_metadata *md, const pdot_mdhash_info *info,
                       uint8_t out[32])
{
    ctx_t x;
    uint8_t *acc = NULL, root[32], xt[32];
    uint32_t *stack = NULL, ty, next_id = 0;
    leaves_t l = { NULL, 0, 0 };
    pdot_buf d;
    size_t i;
    int rc = 0;

    if (!md || !info || !info->spec_name || !info->token_symbol || !out)
        return PDOT_E_INVALID;
    if (!md->have_xt_types)
        return PDOT_E_UNSUPPORTED;
    memset(&x, 0, sizeof x);
    x.md = md;
    pdot_buf_init(&d);
    acc = calloc(md->ntypes + 1, 1);
    stack = malloc((md->ntypes + 1) * sizeof *stack);
    x.new_id = malloc((md->ntypes + 1) * sizeof *x.new_id);
    x.seen = calloc(md->ntypes + 1, sizeof *x.seen);
    if (!acc || !stack || !x.new_id || !x.seen) {
        rc = PDOT_E_NOMEM;
        goto done;
    }

    TRY(collect_accessible(md, md->call_ty, acc, stack));
    TRY(collect_accessible(md, md->address_ty, acc, stack));
    TRY(collect_accessible(md, md->signature_ty, acc, stack));
    for (i = 0; i < md->next; i++) {
        TRY(collect_accessible(md, md->ext[i].ty, acc, stack));
        TRY(collect_accessible(md, md->ext[i].implicit_ty, acc, stack));
    }

    for (ty = 0; ty < md->ntypes; ty++) {
        pdot_reader r;
        enum pdot_typedef kind;
        uint64_t count;

        x.new_id[ty] = NO_ID;
        if (!acc[ty])
            continue;
        if (typedef_at(md, ty, &r, &kind, &count)) {
            rc = PDOT_E_DECODE;
            goto done;
        }
        if (is_basic(kind, count))
            x.new_id[ty] = next_id++;
    }
    for (ty = 0; ty < md->ntypes; ty++)
        if (x.new_id[ty] != NO_ID)
            TRY(type_leaves(&x, ty, &l));
    TRY(tree_root(&l, root));
    TRY(extrinsic_hash(&x, xt));

    pdot_scale_u8(&d, 1);   /* MetadataDigest::V1 */
    pdot_scale_raw(&d, root, 32);
    pdot_scale_raw(&d, xt, 32);
    pdot_scale_u32(&d, info->spec_version);
    pdot_scale_bytes(&d, (const uint8_t *)info->spec_name, strlen(info->spec_name));
    pdot_scale_u16(&d, info->ss58_prefix);
    pdot_scale_u8(&d, info->decimals);
    pdot_scale_bytes(&d, (const uint8_t *)info->token_symbol, strlen(info->token_symbol));
    if (!(rc = d.err))
        pdot_blake3(out, d.data, d.len);
done:
    free(acc);
    free(stack);
    free(x.new_id);
    free(x.seen);
    free(l.h);
    pdot_buf_free(&d);
    return rc;
}

int pdot_md_ss58_prefix(const pdot_metadata *md, uint16_t *prefix)
{
    const uint8_t *v;
    size_t len;
    int rc;

    if ((rc = pdot_md_constant(md, "System", "SS58Prefix", &v, &len)))
        return rc;
    if (len != 2)
        return PDOT_E_DECODE;
    *prefix = (uint16_t)(v[0] | v[1] << 8);
    return 0;
}
