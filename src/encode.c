#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pdot.h"
#include "encode.h"
#include "hex.h"

#define MAX_DEPTH 48
#define MAX_NAME  96

enum {
    PRIM_BOOL, PRIM_CHAR, PRIM_STR, PRIM_U8, PRIM_U16, PRIM_U32, PRIM_U64, PRIM_U128,
    PRIM_U256, PRIM_I8, PRIM_I16, PRIM_I32, PRIM_I64, PRIM_I128, PRIM_I256
};

typedef struct {
    const pdot_metadata *md;
    char  *err;
    size_t err_len;
    char   path[160];
} enc;

static int fail(enc *e, const char *fmt, ...)
{
    char msg[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (e->err && e->err_len)
        snprintf(e->err, e->err_len, "%s: %s", e->path[0] ? e->path : "value", msg);
    return PDOT_E_INVALID;
}

/* Appends ".name" or "[i]" to the path; returns the length to restore. */
static size_t push_name(enc *e, const char *name, size_t len)
{
    size_t at = strlen(e->path);

    snprintf(e->path + at, sizeof e->path - at, "%s%.*s", at ? "." : "", (int)len, name);
    return at;
}

static size_t push_index(enc *e, size_t i)
{
    size_t at = strlen(e->path);

    snprintf(e->path + at, sizeof e->path - at, "[%zu]", i);
    return at;
}

static void pop(enc *e, size_t at)
{
    e->path[at] = '\0';
}

static int encode(enc *e, uint32_t ty, const cJSON *v, pdot_buf *out, int depth);

/* ---- Integers ------------------------------------------------------------ */

static int json_u128(enc *e, const cJSON *v, pdot_u128 *out)
{
    out->lo = out->hi = 0;
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d < 0 || d != floor(d) || d > 9007199254740992.0)
            return fail(e, "%g is not a whole number up to 2^53 (pass larger ones as a string)",
                        d);
        out->lo = (uint64_t)d;
        return 0;
    }
    if (cJSON_IsString(v) && pdot_u128_parse(v->valuestring, 0, out) == 0)
        return 0;
    return fail(e, "expected an unsigned integer");
}

static int json_i64(enc *e, const cJSON *v, int64_t *out)
{
    if (cJSON_IsNumber(v) && v->valuedouble == floor(v->valuedouble) &&
        fabs(v->valuedouble) <= 9007199254740992.0) {
        *out = (int64_t)v->valuedouble;
        return 0;
    }
    if (cJSON_IsString(v)) {
        const char *p = v->valuestring;
        int neg = *p == '-';
        pdot_u128 u;
        if (pdot_u128_parse(p + neg, 0, &u) == 0 && u.hi == 0 &&
            u.lo <= (uint64_t)INT64_MAX + (uint64_t)neg) {
            *out = neg ? (int64_t)(0 - u.lo) : (int64_t)u.lo;
            return 0;
        }
    }
    return fail(e, "expected an integer");
}

/* Little-endian bytes of an unsigned value, width bytes. */
static int put_uint(enc *e, const cJSON *v, size_t width, pdot_buf *out)
{
    uint8_t b[16];
    pdot_u128 u;
    size_t i;
    int rc;

    if (width > sizeof b)
        return fail(e, "integers wider than 128 bits are not supported");
    if ((rc = json_u128(e, v, &u)))
        return rc;
    if ((width < 8 && (u.hi || u.lo >> (8 * width))) || (width == 8 && u.hi))
        return fail(e, "does not fit in u%zu", width * 8);
    for (i = 0; i < width; i++)
        b[i] = (uint8_t)(i < 8 ? u.lo >> (8 * i) : u.hi >> (8 * (i - 8)));
    pdot_scale_raw(out, b, width);
    return 0;
}

static int put_int(enc *e, const cJSON *v, size_t width, pdot_buf *out)
{
    uint8_t b[16];
    int64_t x;
    size_t i;
    int rc;

    if (width > sizeof b)
        return fail(e, "integers wider than 128 bits are not supported");
    if ((rc = json_i64(e, v, &x)))
        return rc;
    if (width < 8) {
        int64_t lim = (int64_t)1 << (8 * width - 1);
        if (x < -lim || x >= lim)
            return fail(e, "does not fit in i%zu", width * 8);
    }
    for (i = 0; i < width; i++)
        b[i] = (uint8_t)((uint64_t)x >> (8 * (i < 8 ? i : 7)));
    if (width > 8)
        memset(b + 8, x < 0 ? 0xff : 0, width - 8);
    pdot_scale_raw(out, b, width);
    return 0;
}

static int encode_primitive(enc *e, uint8_t prim, const cJSON *v, pdot_buf *out)
{
    static const uint8_t width[] = { 0, 0, 0, 1, 2, 4, 8, 16, 32, 1, 2, 4, 8, 16, 32 };

    switch (prim) {
    case PRIM_BOOL:
        if (!cJSON_IsBool(v))
            return fail(e, "expected true or false");
        pdot_scale_bool(out, cJSON_IsTrue(v));
        return 0;
    case PRIM_STR:
        if (!cJSON_IsString(v))
            return fail(e, "expected a string");
        pdot_scale_bytes(out, (const uint8_t *)v->valuestring, strlen(v->valuestring));
        return 0;
    case PRIM_U8: case PRIM_U16: case PRIM_U32: case PRIM_U64: case PRIM_U128:
        return put_uint(e, v, width[prim], out);
    case PRIM_I8: case PRIM_I16: case PRIM_I32: case PRIM_I64: case PRIM_I128:
        return put_int(e, v, width[prim], out);
    default:
        return fail(e, "char and 256-bit integers are not supported");
    }
}

/* ---- Byte strings ------------------------------------------------------- */

static int is_u8(const pdot_metadata *md, uint32_t ty)
{
    uint8_t prim;

    return pdot_md_primitive(md, ty, &prim) == 0 && prim == PRIM_U8;
}

/*
 * Bytes from a string: 0x hex, an SS58 address when exactly 32 bytes are
 * wanted, or else the text itself. want is the fixed length, or 0 for any.
 */
static int string_bytes(enc *e, const char *s, size_t want, pdot_buf *out, size_t *got)
{
    uint8_t acct[32];
    size_t n = strlen(s);

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint8_t *p;
        if (pdot_hex_decode_alloc(s, &p, &n))
            return fail(e, "bad hex");
        pdot_scale_raw(out, p, n);
        free(p);
    } else if (want == 32 && pdot_ss58_decode(s, acct, NULL) == 0) {
        pdot_scale_raw(out, acct, 32);
        n = 32;
    } else if (want == 32) {
        return fail(e, "expected an SS58 address or 32 bytes of 0x hex");
    } else {
        pdot_scale_raw(out, (const uint8_t *)s, n);
    }
    *got = n;
    return 0;
}

/* ---- Composites, tuples and enums ----------------------------------------- */

static int name_is(const char *name, size_t len, const char *s)
{
    return pdot_md_name_eq(name, len, s);
}

static const cJSON *member(const cJSON *obj, const char *name, size_t len)
{
    char key[MAX_NAME];

    if (len >= sizeof key)
        return NULL;
    memcpy(key, name, len);
    key[len] = '\0';
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int is_option(const pdot_metadata *md, uint32_t ty)
{
    const char *n;
    size_t l;
    uint8_t idx;

    return pdot_md_type_name(md, ty, &n, &l) == 0 && name_is(n, l, "Option") &&
           pdot_md_variant(md, ty, "None", &idx, NULL) == 0;
}

/*
 * Encodes the fields from it with v: an object by name or an array in order;
 * a single field also takes its value directly unless v names it.
 */
static int encode_fields(enc *e, pdot_md_iter *it, const cJSON *v, pdot_buf *out, int depth)
{
    size_t nfields = (size_t)it->left, i = 0, used = 0;
    pdot_md_field f;
    int rc;

    if (nfields == 0)
        return 0;  /* the value, if any, carries nothing */
    if (nfields == 1) {
        if (pdot_md_iter_next(it, &f) != 1)
            return PDOT_E_METADATA;
        if (cJSON_IsObject(v) && f.name_len && cJSON_GetArraySize(v) == 1 &&
            member(v, f.name, f.name_len)) {
            size_t at = push_name(e, f.name, f.name_len);
            rc = encode(e, f.ty, member(v, f.name, f.name_len), out, depth + 1);
            pop(e, at);
            return rc;
        }
        return encode(e, f.ty, v, out, depth + 1);
    }
    if (cJSON_IsArray(v)) {
        if ((size_t)cJSON_GetArraySize(v) != nfields)
            return fail(e, "expected %zu values, got %d", nfields, cJSON_GetArraySize(v));
        while ((rc = pdot_md_iter_next(it, &f)) == 1) {
            size_t at = f.name_len ? push_name(e, f.name, f.name_len) : push_index(e, i);
            rc = encode(e, f.ty, cJSON_GetArrayItem(v, (int)i), out, depth + 1);
            pop(e, at);
            if (rc)
                return rc;
            i++;
        }
        return rc < 0 ? rc : 0;
    }
    if (!cJSON_IsObject(v))
        return fail(e, "expected an object or an array of %zu values", nfields);
    while ((rc = pdot_md_iter_next(it, &f)) == 1) {
        const cJSON *m = f.name_len ? member(v, f.name, f.name_len) : NULL;
        size_t at = push_name(e, f.name, f.name_len);
        if (!f.name_len) {
            pop(e, at);
            return fail(e, "unnamed fields take an array");
        }
        if (m) {
            used++;
            rc = encode(e, f.ty, m, out, depth + 1);
        } else if (is_option(e->md, f.ty)) {
            pdot_scale_u8(out, 0);  /* omitted Option: None */
            rc = 0;
        } else {
            rc = fail(e, "missing");
        }
        pop(e, at);
        if (rc)
            return rc;
    }
    if (rc < 0)
        return rc;
    if (used != (size_t)cJSON_GetArraySize(v))
        return fail(e, "has fields the type does not");
    return 0;
}

static int encode_variant(enc *e, uint32_t ty, const cJSON *v, pdot_buf *out, int depth)
{
    const char *vname, *tname;
    size_t vlen, tlen, nf;
    const cJSON *fields = NULL;
    pdot_md_iter it;
    uint8_t index;
    int rc;

    if (pdot_md_type_name(e->md, ty, &tname, &tlen))
        tname = "", tlen = 0;

    if (cJSON_IsNull(v)) {
        vname = "None";
    } else if (cJSON_IsString(v)) {
        vname = v->valuestring;
        /* MultiAddress from an address alone: Id. */
        if (pdot_md_variant(e->md, ty, vname, &index, &nf) && name_is(tname, tlen, "MultiAddress"))
            vname = "Id", fields = v;
    } else if (cJSON_IsObject(v) && cJSON_GetArraySize(v) == 1 &&
               pdot_md_variant(e->md, ty, v->child->string, &index, &nf) == 0) {
        vname = v->child->string;
        fields = v->child;
    } else if (is_option(e->md, ty)) {
        vname = "Some", fields = v;  /* an Option's value, given directly */
    } else {
        return fail(e, "expected a variant of %.*s: \"Name\" or {\"Name\": fields}",
                    (int)tlen, tname);
    }
    if (pdot_md_variant(e->md, ty, vname, &index, &nf))
        return fail(e, "%.*s has no variant %s", (int)tlen, tname, vname);
    if (nf && !fields)
        return fail(e, "variant %s needs a value: {\"%s\": ...}", vname, vname);
    if (pdot_md_iter_variant(e->md, ty, index, &it, &vname, &vlen))
        return PDOT_E_METADATA;
    pdot_scale_u8(out, index);
    {
        size_t at = push_name(e, vname, vlen);
        rc = encode_fields(e, &it, fields, out, depth);
        pop(e, at);
    }
    return rc;
}

/* ---- Sequences and arrays -------------------------------------------- */

static int encode_elems(enc *e, uint32_t elem, const cJSON *v, size_t want, pdot_buf *out,
                        int depth)
{
    const cJSON *item;
    size_t i = 0;
    int rc;

    if (cJSON_IsString(v) && is_u8(e->md, elem)) {
        pdot_buf tmp;
        size_t got;

        pdot_buf_init(&tmp);
        rc = string_bytes(e, v->valuestring, want, &tmp, &got);
        if (!rc && want && got != want)
            rc = fail(e, "expected %zu bytes, got %zu", want, got);
        if (!rc) {
            if (!want)
                pdot_scale_compact(out, got);
            pdot_scale_raw(out, tmp.data, tmp.len);
            rc = tmp.err;
        }
        pdot_buf_free(&tmp);
        return rc;
    }
    if (!cJSON_IsArray(v))
        return fail(e, is_u8(e->md, elem) ? "expected 0x hex, text or an array"
                                          : "expected an array");
    if (want && (size_t)cJSON_GetArraySize(v) != want)
        return fail(e, "expected %zu elements, got %d", want, cJSON_GetArraySize(v));
    if (!want)
        pdot_scale_compact(out, (uint64_t)cJSON_GetArraySize(v));
    cJSON_ArrayForEach(item, v) {
        size_t at = push_index(e, i++);
        rc = encode(e, elem, item, out, depth + 1);
        pop(e, at);
        if (rc)
            return rc;
    }
    return 0;
}

static int encode(enc *e, uint32_t ty, const cJSON *v, pdot_buf *out, int depth)
{
    enum pdot_typedef kind;
    pdot_reader t;
    pdot_md_iter it;
    uint64_t n;
    uint32_t len;
    uint8_t prim;

    if (!v)
        return fail(e, "missing");
    if (depth > MAX_DEPTH)
        return fail(e, "nested too deeply");
    if (pdot_md_typedef(e->md, ty, &t, &kind))
        return PDOT_E_METADATA;
    switch (kind) {
    case PDOT_TYPE_PRIMITIVE:
        if (pdot_read_u8(&t, &prim))
            return PDOT_E_METADATA;
        return encode_primitive(e, prim, v, out);

    case PDOT_TYPE_COMPACT: {
        pdot_u128 u;
        int rc;
        if ((rc = json_u128(e, v, &u)))
            return rc;
        pdot_scale_compact128(out, u.lo, u.hi);
        return 0;
    }

    case PDOT_TYPE_COMPOSITE:
        if (pdot_md_iter_fields(e->md, ty, &it))
            return PDOT_E_METADATA;
        return encode_fields(e, &it, v, out, depth);

    case PDOT_TYPE_TUPLE:
        if (pdot_md_iter_fields(e->md, ty, &it))
            return PDOT_E_METADATA;
        if (it.left > 1 && !cJSON_IsArray(v))
            return fail(e, "expected an array of %u values", (unsigned)it.left);
        return encode_fields(e, &it, v, out, depth);

    case PDOT_TYPE_VARIANT:
        return encode_variant(e, ty, v, out, depth);

    case PDOT_TYPE_SEQUENCE:
        if (pdot_read_compact(&t, &n) || n >= e->md->ntypes)
            return PDOT_E_METADATA;
        return encode_elems(e, (uint32_t)n, v, 0, out, depth);

    case PDOT_TYPE_ARRAY:
        if (pdot_read_u32(&t, &len) || pdot_read_compact(&t, &n) || n >= e->md->ntypes)
            return PDOT_E_METADATA;
        if (len == 0)
            return 0;
        return encode_elems(e, (uint32_t)n, v, len, out, depth);

    case PDOT_TYPE_BITSEQ:
        return fail(e, "bit sequences are not supported");
    }
    return PDOT_E_METADATA;
}

/* ---- Public ------------------------------------------------------------ */

int pdot_encode_json(const pdot_metadata *md, uint32_t ty, const cJSON *v, pdot_buf *out,
                     char *err, size_t err_len)
{
    enc e;
    pdot_buf tmp;
    int rc;

    if (!md || !out)
        return PDOT_E_INVALID;
    memset(&e, 0, sizeof e);
    e.md = md;
    e.err = err;
    e.err_len = err_len;
    pdot_buf_init(&tmp);
    rc = encode(&e, ty, v, &tmp, 0);
    if (rc == PDOT_E_METADATA)
        fail(&e, "the metadata describes this type in a way libpdot cannot read");
    if (!rc) {
        pdot_scale_raw(out, tmp.data, tmp.len);
        rc = tmp.err ? tmp.err : out->err;
    }
    pdot_buf_free(&tmp);
    return rc;
}

int pdot_encode_call_begin(const pdot_metadata *md, const char *pallet, const char *call,
                           pdot_buf *out, pdot_md_iter *args)
{
    const pdot_md_pallet *pl;
    const char *name;
    size_t len;
    uint8_t pi, ci;

    if (!md || !pallet || !call)
        return PDOT_E_INVALID;
    if (!(pl = pdot_md_find_pallet(md, pallet)) || !pl->has_calls ||
        pdot_md_call(md, pallet, call, &pi, &ci) ||
        pdot_md_iter_variant(md, pl->calls_ty, ci, args, &name, &len))
        return PDOT_E_NOTFOUND;
    pdot_scale_u8(out, pi);
    pdot_scale_u8(out, ci);
    return out->err;
}

int pdot_encode_call_json(const pdot_metadata *md, const char *pallet, const char *call,
                          const cJSON *args, pdot_buf *out, char *err, size_t err_len)
{
    pdot_md_iter it;
    pdot_buf tmp;
    enc e;
    int rc;

    memset(&e, 0, sizeof e);
    e.md = md;
    e.err = err;
    e.err_len = err_len;
    snprintf(e.path, sizeof e.path, "%s.%s", pallet ? pallet : "", call ? call : "");
    pdot_buf_init(&tmp);
    if ((rc = pdot_encode_call_begin(md, pallet, call, &tmp, &it))) {
        if (rc == PDOT_E_NOTFOUND)
            fail(&e, "no such call in this runtime");
    } else if (it.left == 0 && (!args || cJSON_GetArraySize(args) == 0)) {
        rc = 0;
    } else if (!cJSON_IsArray(args) && !cJSON_IsObject(args)) {
        rc = fail(&e, "arguments are an array in order or an object by name");
    } else if (it.left == 1 && cJSON_IsArray(args)) {
        pdot_md_field f;
        if (cJSON_GetArraySize(args) != 1)
            rc = fail(&e, "takes 1 argument, got %d", cJSON_GetArraySize(args));
        else if (pdot_md_iter_next(&it, &f) != 1)
            rc = PDOT_E_METADATA;
        else {
            size_t at = push_name(&e, f.name, f.name_len);
            rc = encode(&e, f.ty, args->child, &tmp, 0);
            pop(&e, at);
        }
    } else {
        rc = encode_fields(&e, &it, args, &tmp, 0);
    }
    if (rc == PDOT_E_METADATA)
        fail(&e, "the metadata describes this call in a way libpdot cannot read");
    if (!rc) {
        pdot_scale_raw(out, tmp.data, tmp.len);
        rc = tmp.err ? tmp.err : out->err;
    }
    pdot_buf_free(&tmp);
    return rc;
}
