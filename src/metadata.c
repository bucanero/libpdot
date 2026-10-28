#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "metadata.h"

#define TRY(x) do { if ((x) != 0) return PDOT_E_DECODE; } while (0)

int pdot_md_name_eq(const char *name, size_t len, const char *cstr)
{
    return strlen(cstr) == len && memcmp(name, cstr, len) == 0;
}

static int read_compact_u32(pdot_reader *r, uint32_t *v)
{
    uint64_t t;

    TRY(pdot_read_compact(r, &t));
    if (t > 0xffffffffu)
        return PDOT_E_DECODE;
    *v = (uint32_t)t;
    return 0;
}

static int read_str(pdot_reader *r, const char **s, size_t *len)
{
    const uint8_t *p;

    TRY(pdot_read_bytes(r, &p, len));
    *s = (const char *)p;
    return 0;
}

static int skip_str(pdot_reader *r)
{
    const char *s;
    size_t n;

    return read_str(r, &s, &n);
}

/* Vec<String>, as used by docs and type paths. */
static int skip_strs(pdot_reader *r)
{
    uint64_t n, i;

    TRY(pdot_read_compact(r, &n));
    for (i = 0; i < n; i++)
        TRY(skip_str(r));
    return 0;
}

/* Option tag: 0 or 1. */
static int read_option(pdot_reader *r, int *some)
{
    uint8_t t;

    TRY(pdot_read_u8(r, &t));
    if (t > 1)
        return PDOT_E_DECODE;
    *some = t;
    return 0;
}

static int skip_opt_str(pdot_reader *r)
{
    int some;

    TRY(read_option(r, &some));
    return some ? skip_str(r) : 0;
}

static int skip_opt_compact(pdot_reader *r)
{
    uint32_t v;
    int some;

    TRY(read_option(r, &some));
    return some ? read_compact_u32(r, &v) : 0;
}

/* Field { name: Option<String>, ty, type_name: Option<String>, docs } */
static int read_field(pdot_reader *r, uint32_t *ty)
{
    TRY(skip_opt_str(r));
    TRY(read_compact_u32(r, ty));
    TRY(skip_opt_str(r));
    return skip_strs(r);
}

static int skip_fields(pdot_reader *r)
{
    uint64_t n, i;
    uint32_t ty;

    TRY(pdot_read_compact(r, &n));
    for (i = 0; i < n; i++)
        TRY(read_field(r, &ty));
    return 0;
}

static int skip_typedef(pdot_reader *r)
{
    uint8_t kind, b;
    uint64_t n, i;
    uint32_t v;

    TRY(pdot_read_u8(r, &kind));
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
        return skip_fields(r);
    case PDOT_TYPE_VARIANT:
        TRY(pdot_read_compact(r, &n));
        for (i = 0; i < n; i++) {
            TRY(skip_str(r));
            TRY(skip_fields(r));
            TRY(pdot_read_u8(r, &b));
            TRY(skip_strs(r));
        }
        return 0;
    case PDOT_TYPE_SEQUENCE:
    case PDOT_TYPE_COMPACT:
        return read_compact_u32(r, &v);
    case PDOT_TYPE_ARRAY:
        TRY(pdot_read_u32(r, &v));
        return read_compact_u32(r, &v);
    case PDOT_TYPE_TUPLE:
        TRY(pdot_read_compact(r, &n));
        for (i = 0; i < n; i++)
            TRY(read_compact_u32(r, &v));
        return 0;
    case PDOT_TYPE_PRIMITIVE:
        TRY(pdot_read_u8(r, &b));
        return b <= 15 ? 0 : PDOT_E_DECODE;
    case PDOT_TYPE_BITSEQ:
        TRY(read_compact_u32(r, &v));
        return read_compact_u32(r, &v);
    default:
        return PDOT_E_DECODE;
    }
}

/* Type { path, type_params: Vec<{name, Option<ty>}>, type_def, docs } */
static int skip_type_header(pdot_reader *r)
{
    uint64_t n, i;

    TRY(skip_strs(r));
    TRY(pdot_read_compact(r, &n));
    for (i = 0; i < n; i++) {
        TRY(skip_str(r));
        TRY(skip_opt_compact(r));
    }
    return 0;
}

static int parse_types(pdot_metadata *md, pdot_reader *r)
{
    uint64_t n, i;
    uint32_t id;

    TRY(pdot_read_compact(r, &n));
    /* Every type takes at least 4 bytes; bounds the allocation. */
    if (n > pdot_reader_remaining(r) / 4)
        return PDOT_E_DECODE;
    md->ntypes = (uint32_t)n;
    md->type_off = malloc((n ? n : 1) * sizeof *md->type_off);
    if (!md->type_off)
        return PDOT_E_NOMEM;
    for (i = 0; i < n; i++) {
        TRY(read_compact_u32(r, &id));
        if (id != i)
            return PDOT_E_DECODE;  /* portable registries are dense */
        md->type_off[i] = (uint32_t)r->pos;
        TRY(skip_type_header(r));
        TRY(skip_typedef(r));
        TRY(skip_strs(r));
    }
    return 0;
}

static int skip_storage(pdot_reader *r)
{
    uint64_t n, i;
    uint8_t b;
    uint32_t v;
    const uint8_t *p;
    size_t len;

    TRY(skip_str(r));  /* prefix */
    TRY(pdot_read_compact(r, &n));
    for (i = 0; i < n; i++) {
        TRY(skip_str(r));
        TRY(pdot_read_u8(r, &b));  /* modifier */
        TRY(pdot_read_u8(r, &b));  /* entry type */
        if (b == 0) {
            TRY(read_compact_u32(r, &v));
        } else if (b == 1) {
            TRY(pdot_read_bytes(r, &p, &len));  /* hashers: Vec<u8 enum> */
            TRY(read_compact_u32(r, &v));
            TRY(read_compact_u32(r, &v));
        } else {
            return PDOT_E_DECODE;
        }
        TRY(pdot_read_bytes(r, &p, &len));  /* default */
        TRY(skip_strs(r));
    }
    return 0;
}

static int parse_pallets(pdot_metadata *md, pdot_reader *r)
{
    uint64_t n, i, j, nconst;
    int some;
    uint32_t v;
    const uint8_t *p;
    size_t len;

    TRY(pdot_read_compact(r, &n));
    if (n > 256)
        return PDOT_E_DECODE;
    md->pallets = calloc(n ? n : 1, sizeof *md->pallets);
    if (!md->pallets)
        return PDOT_E_NOMEM;
    md->npallets = (size_t)n;
    for (i = 0; i < n; i++) {
        pdot_md_pallet *pl = &md->pallets[i];

        TRY(read_str(r, &pl->name, &pl->name_len));
        TRY(read_option(r, &some));
        if (some) {
            pl->storage_off = r->pos;
            TRY(skip_storage(r));
        }
        TRY(read_option(r, &pl->has_calls));
        if (pl->has_calls)
            TRY(read_compact_u32(r, &pl->calls_ty));
        TRY(skip_opt_compact(r));  /* event */
        TRY(pdot_read_compact(r, &nconst));
        for (j = 0; j < nconst; j++) {
            TRY(skip_str(r));
            TRY(read_compact_u32(r, &v));
            TRY(pdot_read_bytes(r, &p, &len));
            TRY(skip_strs(r));
        }
        TRY(skip_opt_compact(r));  /* error */
        TRY(pdot_read_u8(r, &pl->index));
        if (md->version >= 15)
            TRY(skip_strs(r));
    }
    return 0;
}

static int parse_extrinsic(pdot_metadata *md, pdot_reader *r)
{
    uint64_t n, i;
    uint32_t v;

    if (md->version == 14) {
        TRY(read_compact_u32(r, &v));  /* UncheckedExtrinsic type */
        TRY(pdot_read_u8(r, &md->extrinsic_version));
    } else {
        TRY(pdot_read_u8(r, &md->extrinsic_version));
        TRY(read_compact_u32(r, &md->address_ty));
        TRY(read_compact_u32(r, &md->call_ty));
        TRY(read_compact_u32(r, &md->signature_ty));
        TRY(read_compact_u32(r, &v));  /* extra tuple */
        md->have_xt_types = 1;
    }
    TRY(pdot_read_compact(r, &n));
    if (n > 64)
        return PDOT_E_DECODE;
    md->ext = calloc(n ? n : 1, sizeof *md->ext);
    if (!md->ext)
        return PDOT_E_NOMEM;
    md->next = (size_t)n;
    for (i = 0; i < n; i++) {
        pdot_md_extension *e = &md->ext[i];
        TRY(read_str(r, &e->name, &e->name_len));
        TRY(read_compact_u32(r, &e->ty));
        TRY(read_compact_u32(r, &e->implicit_ty));
    }
    return 0;
}

static int check_type_refs(const pdot_metadata *md)
{
    size_t i;

    for (i = 0; i < md->npallets; i++)
        if (md->pallets[i].has_calls && md->pallets[i].calls_ty >= md->ntypes)
            return PDOT_E_DECODE;
    for (i = 0; i < md->next; i++)
        if (md->ext[i].ty >= md->ntypes || md->ext[i].implicit_ty >= md->ntypes)
            return PDOT_E_DECODE;
    if (md->have_xt_types && (md->address_ty >= md->ntypes ||
                              md->call_ty >= md->ntypes ||
                              md->signature_ty >= md->ntypes))
        return PDOT_E_DECODE;
    return 0;
}

int pdot_metadata_parse(const uint8_t *blob, size_t len, pdot_metadata **out)
{
    pdot_metadata *md;
    pdot_reader r;
    uint32_t magic;
    int rc;

    *out = NULL;
    if (!blob)
        return PDOT_E_INVALID;
    md = calloc(1, sizeof *md);
    if (!md || !(md->blob = malloc(len ? len : 1))) {
        free(md);
        return PDOT_E_NOMEM;
    }
    memcpy(md->blob, blob, len);
    md->len = len;

    pdot_reader_init(&r, md->blob, len);
    if (pdot_read_u32(&r, &magic) || magic != 0x6174656d ||  /* "meta" */
        pdot_read_u8(&r, &md->version)) {
        rc = PDOT_E_DECODE;
    } else if (md->version != 14 && md->version != 15) {
        rc = PDOT_E_UNSUPPORTED;
    } else if (!(rc = parse_types(md, &r)) && !(rc = parse_pallets(md, &r)) &&
               !(rc = parse_extrinsic(md, &r))) {
        md->parsed_len = r.pos;
        rc = check_type_refs(md);
    }
    if (rc) {
        pdot_metadata_free(md);
        return rc;
    }
    *out = md;
    return 0;
}

void pdot_metadata_free(pdot_metadata *md)
{
    if (!md)
        return;
    free(md->blob);
    free(md->type_off);
    free(md->pallets);
    free(md->ext);
    free(md);
}

const pdot_md_pallet *pdot_md_find_pallet(const pdot_metadata *md, const char *name)
{
    size_t i;

    for (i = 0; i < md->npallets; i++)
        if (pdot_md_name_eq(md->pallets[i].name, md->pallets[i].name_len, name))
            return &md->pallets[i];
    return NULL;
}

int pdot_md_typedef(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                    enum pdot_typedef *kind)
{
    uint8_t k;

    if (ty >= md->ntypes)
        return PDOT_E_DECODE;
    pdot_reader_init(r, md->blob, md->len);
    r->pos = md->type_off[ty];
    TRY(skip_type_header(r));
    TRY(pdot_read_u8(r, &k));
    *kind = (enum pdot_typedef)k;
    return 0;
}

int pdot_md_type_name(const pdot_metadata *md, uint32_t ty, const char **name,
                      size_t *len)
{
    pdot_reader r;
    uint64_t n, i;

    if (ty >= md->ntypes)
        return PDOT_E_DECODE;
    pdot_reader_init(&r, md->blob, md->len);
    r.pos = md->type_off[ty];
    *name = "";
    *len = 0;
    TRY(pdot_read_compact(&r, &n));
    for (i = 0; i < n; i++)
        TRY(read_str(&r, name, len));
    return 0;
}

int pdot_md_variant_fields(const pdot_metadata *md, uint32_t ty, const char *name,
                           uint8_t *index, uint32_t *field_tys, size_t max_fields,
                           size_t *nfields)
{
    enum pdot_typedef kind;
    pdot_reader r;
    uint64_t n, i, nf, j;

    if (pdot_md_typedef(md, ty, &r, &kind) || kind != PDOT_TYPE_VARIANT)
        return PDOT_E_METADATA;
    TRY(pdot_read_compact(&r, &n));
    for (i = 0; i < n; i++) {
        const char *vname;
        size_t vlen;
        uint8_t idx;
        int match;

        TRY(read_str(&r, &vname, &vlen));
        match = pdot_md_name_eq(vname, vlen, name);
        TRY(pdot_read_compact(&r, &nf));
        for (j = 0; j < nf; j++) {
            uint32_t fty;
            TRY(read_field(&r, &fty));
            if (match && field_tys && j < max_fields)
                field_tys[j] = fty;
        }
        TRY(pdot_read_u8(&r, &idx));
        TRY(skip_strs(&r));
        if (match) {
            *index = idx;
            if (nfields)
                *nfields = (size_t)nf;
            return 0;
        }
    }
    return PDOT_E_NOTFOUND;
}

int pdot_md_variant(const pdot_metadata *md, uint32_t ty, const char *name,
                    uint8_t *index, size_t *nfields)
{
    return pdot_md_variant_fields(md, ty, name, index, NULL, 0, nfields);
}

int pdot_md_call_fields(const pdot_metadata *md, const char *pallet, const char *call,
                        uint8_t *pallet_index, uint8_t *call_index,
                        uint32_t *field_tys, size_t max_fields, size_t *nfields)
{
    const pdot_md_pallet *pl = pdot_md_find_pallet(md, pallet);

    if (!pl || !pl->has_calls)
        return PDOT_E_NOTFOUND;
    *pallet_index = pl->index;
    return pdot_md_variant_fields(md, pl->calls_ty, call, call_index,
                                  field_tys, max_fields, nfields);
}

int pdot_md_call(const pdot_metadata *md, const char *pallet, const char *call,
                 uint8_t *pallet_index, uint8_t *call_index)
{
    return pdot_md_call_fields(md, pallet, call, pallet_index, call_index, NULL, 0, NULL);
}

int pdot_md_primitive(const pdot_metadata *md, uint32_t ty, uint8_t *prim)
{
    enum pdot_typedef kind;
    pdot_reader r;

    if (pdot_md_typedef(md, ty, &r, &kind) || kind != PDOT_TYPE_PRIMITIVE)
        return PDOT_E_METADATA;
    return pdot_read_u8(&r, prim) ? PDOT_E_DECODE : 0;
}

int pdot_md_storage_entry(const pdot_metadata *md, const char *pallet,
                          const char *entry, pdot_md_storage *out)
{
    const pdot_md_pallet *pl = pdot_md_find_pallet(md, pallet);
    pdot_reader r;
    uint64_t n, i;

    if (!pl || !pl->storage_off)
        return PDOT_E_NOTFOUND;
    pdot_reader_init(&r, md->blob, md->len);
    r.pos = pl->storage_off;
    TRY(read_str(&r, &out->prefix, &out->prefix_len));
    TRY(pdot_read_compact(&r, &n));
    for (i = 0; i < n; i++) {
        const char *name;
        size_t name_len, nh, j;
        const uint8_t *hashers, *p;
        uint8_t modifier, kind;

        TRY(read_str(&r, &name, &name_len));
        TRY(pdot_read_u8(&r, &modifier));
        TRY(pdot_read_u8(&r, &kind));
        out->is_map = kind == 1;
        out->nhashers = 0;
        if (kind == 0) {
            TRY(read_compact_u32(&r, &out->value_ty));
        } else {
            TRY(pdot_read_bytes(&r, &hashers, &nh));
            TRY(read_compact_u32(&r, &out->key_ty));
            TRY(read_compact_u32(&r, &out->value_ty));
            if (nh > PDOT_MD_MAX_HASHERS)
                return PDOT_E_UNSUPPORTED;
            for (j = 0; j < nh; j++) {
                if (hashers[j] > PDOT_HASHER_IDENTITY)
                    return PDOT_E_DECODE;
                out->hashers[j] = hashers[j];
            }
            out->nhashers = nh;
        }
        TRY(pdot_read_bytes(&r, &p, &nh));
        TRY(skip_strs(&r));
        if (pdot_md_name_eq(name, name_len, entry))
            return 0;
    }
    return PDOT_E_NOTFOUND;
}

static int zero_sized(const pdot_metadata *md, uint32_t ty, int depth)
{
    enum pdot_typedef kind;
    pdot_reader r;
    uint64_t n, i;
    uint32_t len, elem;

    if (depth > 16 || pdot_md_typedef(md, ty, &r, &kind))
        return 0;
    switch (kind) {
    case PDOT_TYPE_COMPOSITE:
        if (pdot_read_compact(&r, &n))
            return 0;
        for (i = 0; i < n; i++)
            if (read_field(&r, &elem) || !zero_sized(md, elem, depth + 1))
                return 0;
        return 1;
    case PDOT_TYPE_TUPLE:
        if (pdot_read_compact(&r, &n))
            return 0;
        for (i = 0; i < n; i++)
            if (read_compact_u32(&r, &elem) || !zero_sized(md, elem, depth + 1))
                return 0;
        return 1;
    case PDOT_TYPE_ARRAY:
        if (pdot_read_u32(&r, &len) || read_compact_u32(&r, &elem))
            return 0;
        return len == 0 || zero_sized(md, elem, depth + 1);
    default:
        return 0;
    }
}

int pdot_md_is_zero_sized(const pdot_metadata *md, uint32_t ty)
{
    return zero_sized(md, ty, 0);
}
