#include <string.h>

#include "pdot.h"
#include "decode.h"

#define MAX_DEPTH 64

/* Encoded size of each scale-info primitive; 0 for str (length-prefixed). */
static const uint8_t prim_size[] = {
    1,  /* bool */
    4,  /* char */
    0,  /* str */
    1, 2, 4, 8, 16, 32,  /* u8 .. u256 */
    1, 2, 4, 8, 16, 32,  /* i8 .. i256 */
};

/* Each element of a non-zero-sized type takes at least one byte. */
static int count_fits(const pdot_metadata *md, uint32_t elem, uint64_t n,
                      const pdot_reader *r, int *zst)
{
    *zst = pdot_md_is_zero_sized(md, elem);
    return *zst || n <= pdot_reader_remaining(r);
}

static int skip(const pdot_metadata *md, uint32_t ty, pdot_reader *r, int depth);

static int skip_fields(const pdot_metadata *md, pdot_md_iter *it, pdot_reader *r,
                       int depth)
{
    pdot_md_field f;
    int rc;

    while ((rc = pdot_md_iter_next(it, &f)) == 1)
        if ((rc = skip(md, f.ty, r, depth + 1)))
            return rc;
    return rc;
}

static int skip(const pdot_metadata *md, uint32_t ty, pdot_reader *r, int depth)
{
    enum pdot_typedef kind;
    pdot_reader t;
    pdot_md_iter it;
    uint64_t n, i, bits;
    uint32_t len, elem, store;
    uint8_t b, prim;
    int zst;

    if (depth > MAX_DEPTH || pdot_md_typedef(md, ty, &t, &kind))
        return PDOT_E_DECODE;
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
    case PDOT_TYPE_TUPLE:
        if (pdot_md_iter_fields(md, ty, &it))
            return PDOT_E_DECODE;
        return skip_fields(md, &it, r, depth);

    case PDOT_TYPE_VARIANT: {
        const char *name;
        size_t name_len;
        if (pdot_read_u8(r, &b) || pdot_md_iter_variant(md, ty, b, &it, &name, &name_len))
            return PDOT_E_DECODE;
        return skip_fields(md, &it, r, depth);
    }

    case PDOT_TYPE_SEQUENCE:
        if (pdot_read_compact(&t, &n) || n > UINT32_MAX)
            return PDOT_E_DECODE;
        elem = (uint32_t)n;
        if (pdot_read_compact(r, &n) || !count_fits(md, elem, n, r, &zst))
            return PDOT_E_DECODE;
        for (i = 0; i < n && !zst; i++)
            if (skip(md, elem, r, depth + 1))
                return PDOT_E_DECODE;
        return 0;

    case PDOT_TYPE_ARRAY:
        if (pdot_read_u32(&t, &len) || pdot_read_compact(&t, &n) || n > UINT32_MAX)
            return PDOT_E_DECODE;
        elem = (uint32_t)n;
        if (!count_fits(md, elem, len, r, &zst))
            return PDOT_E_DECODE;
        /* Byte arrays (hashes, keys) in one step. */
        if (pdot_md_primitive(md, elem, &prim) == 0 && prim == PDOT_PRIM_U8)
            return pdot_read_skip(r, len);
        for (i = 0; i < len && !zst; i++)
            if (skip(md, elem, r, depth + 1))
                return PDOT_E_DECODE;
        return 0;

    case PDOT_TYPE_PRIMITIVE:
        if (pdot_read_u8(&t, &prim) || prim >= sizeof prim_size)
            return PDOT_E_DECODE;
        if (prim == 2) {  /* str */
            const uint8_t *p;
            size_t plen;
            return pdot_read_bytes(r, &p, &plen);
        }
        return pdot_read_skip(r, prim_size[prim]);

    case PDOT_TYPE_COMPACT:
        return pdot_read_compact_skip(r);

    case PDOT_TYPE_BITSEQ:
        /* Bits packed into whole store units (u8/u16/u32/u64). */
        if (pdot_read_compact(&t, &n) || n > UINT32_MAX)
            return PDOT_E_DECODE;
        store = (uint32_t)n;
        if (pdot_md_primitive(md, store, &prim) || prim < 3 || prim > 6)
            return PDOT_E_DECODE;
        if (pdot_read_compact(r, &bits))
            return PDOT_E_DECODE;
        {
            uint64_t unit = prim_size[prim];
            uint64_t units = bits / (unit * 8) + (bits % (unit * 8) != 0);
            if (units > pdot_reader_remaining(r) / unit)
                return PDOT_E_DECODE;
            return pdot_read_skip(r, (size_t)(units * unit));
        }
    }
    return PDOT_E_DECODE;
}

int pdot_decode_skip(const pdot_metadata *md, uint32_t ty, pdot_reader *r)
{
    size_t start = r->pos;
    int rc = skip(md, ty, r, 0);

    if (rc)
        r->pos = start;
    return rc;
}

static int read_uint(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                     uint64_t *v, int depth)
{
    enum pdot_typedef kind;
    pdot_reader t;
    pdot_md_iter it;
    pdot_md_field f;
    uint8_t prim;

    if (depth > MAX_DEPTH || pdot_md_typedef(md, ty, &t, &kind))
        return PDOT_E_DECODE;
    switch (kind) {
    case PDOT_TYPE_PRIMITIVE: {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        if (pdot_read_u8(&t, &prim))
            return PDOT_E_DECODE;
        switch (prim) {
        case 3: if (pdot_read_u8(r, &u8)) return PDOT_E_DECODE; *v = u8; return 0;
        case 4: if (pdot_read_u16(r, &u16)) return PDOT_E_DECODE; *v = u16; return 0;
        case 5: if (pdot_read_u32(r, &u32)) return PDOT_E_DECODE; *v = u32; return 0;
        case 6: return pdot_read_u64(r, v) ? PDOT_E_DECODE : 0;
        default: return PDOT_E_UNSUPPORTED;
        }
    }
    case PDOT_TYPE_COMPACT:
        return pdot_read_compact(r, v) ? PDOT_E_DECODE : 0;
    case PDOT_TYPE_COMPOSITE:
        /* Newtype wrappers such as BlockNumberFor or Perbill. */
        if (pdot_md_iter_fields(md, ty, &it) || it.left != 1 || pdot_md_iter_next(&it, &f) != 1)
            return PDOT_E_UNSUPPORTED;
        return read_uint(md, f.ty, r, v, depth + 1);
    default:
        return PDOT_E_UNSUPPORTED;
    }
}

int pdot_decode_uint(const pdot_metadata *md, uint32_t ty, pdot_reader *r, uint64_t *v)
{
    size_t start = r->pos;
    int rc = read_uint(md, ty, r, v, 0);

    if (rc)
        r->pos = start;
    return rc;
}

int pdot_decode_variant(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                        pdot_md_iter *fields, const char **name, size_t *name_len)
{
    uint8_t idx;

    if (pdot_read_u8(r, &idx))
        return PDOT_E_DECODE;
    if (pdot_md_iter_variant(md, ty, idx, fields, name, name_len)) {
        r->pos--;
        return PDOT_E_DECODE;
    }
    return 0;
}

int pdot_decode_seek(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                     const char *path, uint32_t *field_ty)
{
    while (*path) {
        const char *dot = strchr(path, '.');
        size_t seg = dot ? (size_t)(dot - path) : strlen(path);
        pdot_md_iter it;
        pdot_md_field f;
        int rc, found = 0;

        if (pdot_md_iter_fields(md, ty, &it))
            return PDOT_E_METADATA;
        while ((rc = pdot_md_iter_next(&it, &f)) == 1) {
            if (f.name_len == seg && memcmp(f.name, path, seg) == 0) {
                found = 1;
                break;
            }
            if ((rc = pdot_decode_skip(md, f.ty, r)))
                return rc;
        }
        if (rc < 0)
            return rc;
        if (!found)
            return PDOT_E_METADATA;
        ty = f.ty;
        path += seg + (dot != NULL);
    }
    *field_ty = ty;
    return 0;
}

int pdot_decode_path_uint(const pdot_metadata *md, uint32_t ty, const uint8_t *p,
                          size_t len, const char *path, uint64_t *v)
{
    pdot_reader r;
    uint32_t fty;
    int rc;

    pdot_reader_init(&r, p, len);
    if ((rc = pdot_decode_seek(md, ty, &r, path, &fty)))
        return rc;
    return pdot_decode_uint(md, fty, &r, v);
}
