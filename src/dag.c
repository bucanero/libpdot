#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "dag.h"

/* Protobuf wire types and the tags that DAG-PB and UnixFS use. */
#define WT_VARINT 0
#define WT_LEN    2
#define TAG(field, wt) ((uint8_t)((field) << 3 | (wt)))

static void put_varint(pdot_buf *b, uint64_t v)
{
    while (v >= 0x80) {
        pdot_scale_u8(b, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    pdot_scale_u8(b, (uint8_t)v);
}

static size_t varint_len(uint64_t v)
{
    size_t n = 1;

    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

int pdot_dag_encode(const pdot_dag_link *links, size_t nlinks, pdot_buf *out)
{
    uint64_t total = 0;
    size_t i, unixfs_len;

    if (!links || nlinks == 0)
        return PDOT_E_INVALID;
    for (i = 0; i < nlinks; i++)
        total += links[i].size;

    /*
     * PBNode: Links (field 2) come before Data (field 1) on the wire.
     * PBLink: Hash (1), Name (2), Tsize (3). @ipld/dag-pb writes Name
     * whenever it is a string, so the SDK's Name: "" encodes as 12 00.
     */
    for (i = 0; i < nlinks; i++) {
        const pdot_dag_link *l = &links[i];
        size_t link_len = 1 + varint_len(l->cid.len) + l->cid.len + 2 +
                          1 + varint_len(l->size);

        pdot_scale_u8(out, TAG(2, WT_LEN));
        put_varint(out, link_len);
        pdot_scale_u8(out, TAG(1, WT_LEN));
        put_varint(out, l->cid.len);
        pdot_scale_raw(out, l->cid.bytes, l->cid.len);
        pdot_scale_u8(out, TAG(2, WT_LEN));
        pdot_scale_u8(out, 0);
        pdot_scale_u8(out, TAG(3, WT_VARINT));
        put_varint(out, l->size);
    }

    /* UnixFS Data: Type=File (2), filesize, then unpacked blocksizes. */
    unixfs_len = 2 + 1 + varint_len(total);
    for (i = 0; i < nlinks; i++)
        unixfs_len += 1 + varint_len(links[i].size);
    pdot_scale_u8(out, TAG(1, WT_LEN));
    put_varint(out, unixfs_len);
    pdot_scale_u8(out, TAG(1, WT_VARINT));
    pdot_scale_u8(out, 2);
    pdot_scale_u8(out, TAG(3, WT_VARINT));
    put_varint(out, total);
    for (i = 0; i < nlinks; i++) {
        pdot_scale_u8(out, TAG(4, WT_VARINT));
        put_varint(out, links[i].size);
    }
    return out->err;
}

/* ---- Decoding ------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
} pb_reader;

static int pb_varint(pb_reader *r, uint64_t *v)
{
    int shift;

    *v = 0;
    for (shift = 0; shift < 64; shift += 7) {
        uint8_t b;
        if (r->pos >= r->len)
            return PDOT_E_DECODE;
        b = r->p[r->pos++];
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return 0;
    }
    return PDOT_E_DECODE;
}

static int pb_bytes(pb_reader *r, const uint8_t **p, size_t *n)
{
    uint64_t len;

    if (pb_varint(r, &len) || len > r->len - r->pos)
        return PDOT_E_DECODE;
    *p = r->p + r->pos;
    *n = (size_t)len;
    r->pos += (size_t)len;
    return 0;
}

static int decode_link(const uint8_t *p, size_t len, pdot_dag_link *l)
{
    pb_reader r = { p, len, 0 };
    int have_hash = 0, have_size = 0;

    while (r.pos < r.len) {
        uint64_t key, v;
        const uint8_t *b;
        size_t n;

        if (pb_varint(&r, &key))
            return PDOT_E_DECODE;
        if (key == TAG(1, WT_LEN)) {
            if (pb_bytes(&r, &b, &n) || n > PDOT_CID_MAX)
                return PDOT_E_DECODE;
            memcpy(l->cid.bytes, b, n);
            l->cid.len = n;
            have_hash = 1;
        } else if (key == TAG(2, WT_LEN)) {
            if (pb_bytes(&r, &b, &n))  /* Name: ignored */
                return PDOT_E_DECODE;
        } else if (key == TAG(3, WT_VARINT)) {
            if (pb_varint(&r, &v))
                return PDOT_E_DECODE;
            l->size = v;
            have_size = 1;
        } else {
            return PDOT_E_DECODE;
        }
    }
    return have_hash && have_size ? 0 : PDOT_E_DECODE;
}

int pdot_dag_decode(const uint8_t *p, size_t len, pdot_dag_link **links,
                    size_t *nlinks, uint64_t *filesize)
{
    pb_reader r = { p, len, 0 };
    pdot_dag_link *ls = NULL;
    size_t n = 0, cap = 0, nblocks = 0, i;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    uint64_t total = 0, fsize = 0, type = 0, sum = 0;
    int have_fsize = 0, rc = PDOT_E_DECODE;

    *links = NULL;
    *nlinks = 0;
    while (r.pos < r.len) {
        uint64_t key;
        const uint8_t *b;
        size_t bn;

        if (pb_varint(&r, &key) || pb_bytes(&r, &b, &bn))
            goto out;
        if (key == TAG(2, WT_LEN) && !data) {
            if (n == cap) {
                pdot_dag_link *nl;
                cap = cap ? cap * 2 : 16;
                if (cap > len || !(nl = realloc(ls, cap * sizeof *ls))) {
                    rc = PDOT_E_NOMEM;
                    goto out;
                }
                ls = nl;
            }
            if (decode_link(b, bn, &ls[n]))
                goto out;
            n++;
        } else if (key == TAG(1, WT_LEN) && !data) {
            data = b;
            data_len = bn;
        } else {
            goto out;  /* unknown field, or out of DAG-PB's canonical order */
        }
    }
    if (!data || n == 0)
        goto out;

    /* UnixFS Data: the blocksizes must match the links one for one. */
    {
        pb_reader u = { data, data_len, 0 };
        while (u.pos < u.len) {
            uint64_t key, v;
            const uint8_t *b;
            size_t bn;

            if (pb_varint(&u, &key))
                goto out;
            if (key == TAG(1, WT_VARINT)) {
                if (pb_varint(&u, &type))
                    goto out;
            } else if (key == TAG(3, WT_VARINT)) {
                if (pb_varint(&u, &fsize))
                    goto out;
                have_fsize = 1;
            } else if (key == TAG(4, WT_VARINT)) {
                if (pb_varint(&u, &v) || nblocks >= n || v != ls[nblocks].size)
                    goto out;
                nblocks++;
            } else if (key == TAG(2, WT_LEN)) {
                /* Inline data in a root node: not produced by the SDKs. */
                if (pb_bytes(&u, &b, &bn) || bn != 0)
                    goto out;
            } else {
                goto out;
            }
        }
    }
    if (type != 2 || nblocks != n || !have_fsize)
        goto out;
    for (i = 0; i < n; i++) {
        uint64_t code;
        const uint8_t *digest;
        size_t dlen;

        /* Only single-level files: every link must be a raw leaf. */
        if (pdot_cid_digest(&ls[i].cid, &code, &digest, &dlen) ||
            ls[i].cid.bytes[1] != PDOT_CODEC_RAW)
            goto out;
        if (ls[i].size > UINT64_MAX - sum)
            goto out;
        sum += ls[i].size;
    }
    total = sum;
    if (total != fsize)
        goto out;

    *links = ls;
    *nlinks = n;
    *filesize = fsize;
    return 0;
out:
    free(ls);
    return rc;
}
