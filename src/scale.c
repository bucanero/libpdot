#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "scale.h"

void pdot_buf_init(pdot_buf *b)
{
    b->data = NULL;
    b->len = b->cap = 0;
    b->err = 0;
}

void pdot_buf_free(pdot_buf *b)
{
    free(b->data);
    pdot_buf_init(b);
}

int pdot_buf_reserve(pdot_buf *b, size_t extra)
{
    size_t need, cap;
    uint8_t *p;

    if (b->err)
        return b->err;
    if (extra > SIZE_MAX - b->len)
        return b->err = PDOT_E_NOMEM;
    need = b->len + extra;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    p = realloc(b->data, cap);
    if (!p)
        return b->err = PDOT_E_NOMEM;
    b->data = p;
    b->cap = cap;
    return 0;
}

void pdot_scale_raw(pdot_buf *b, const uint8_t *p, size_t n)
{
    if (n == 0 || pdot_buf_reserve(b, n))
        return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* Little-endian by shifts, so host byte order never matters. */
static void put_le(pdot_buf *b, uint64_t v, size_t n)
{
    uint8_t tmp[8];
    size_t i;

    for (i = 0; i < n; i++)
        tmp[i] = (uint8_t)(v >> (8 * i));
    pdot_scale_raw(b, tmp, n);
}

void pdot_scale_u8(pdot_buf *b, uint8_t v)   { put_le(b, v, 1); }
void pdot_scale_u16(pdot_buf *b, uint16_t v) { put_le(b, v, 2); }
void pdot_scale_u32(pdot_buf *b, uint32_t v) { put_le(b, v, 4); }
void pdot_scale_u64(pdot_buf *b, uint64_t v) { put_le(b, v, 8); }
void pdot_scale_bool(pdot_buf *b, int v)     { put_le(b, v ? 1 : 0, 1); }

size_t pdot_scale_compact_len(uint64_t v)
{
    size_t n;

    if (v < (1u << 6))
        return 1;
    if (v < (1u << 14))
        return 2;
    if (v < (1u << 30))
        return 4;
    for (n = 4; n < 8 && (v >> (8 * n)) != 0; n++)
        ;
    return 1 + n;
}

void pdot_scale_compact(pdot_buf *b, uint64_t v)
{
    size_t n = pdot_scale_compact_len(v);

    switch (n) {
    case 1: put_le(b, v << 2, 1); break;
    case 2: put_le(b, (v << 2) | 1, 2); break;
    case 4: put_le(b, (v << 2) | 2, 4); break;
    default:
        put_le(b, ((uint64_t)(n - 1 - 4) << 2) | 3, 1);
        put_le(b, v, n - 1);
        break;
    }
}

void pdot_scale_bytes(pdot_buf *b, const uint8_t *p, size_t n)
{
    pdot_scale_compact(b, n);
    pdot_scale_raw(b, p, n);
}

void pdot_reader_init(pdot_reader *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
}

size_t pdot_reader_remaining(const pdot_reader *r)
{
    return r->len - r->pos;
}

int pdot_read_raw(pdot_reader *r, size_t n, const uint8_t **out)
{
    if (n > r->len - r->pos)
        return PDOT_E_DECODE;
    if (out)
        *out = r->p + r->pos;
    r->pos += n;
    return 0;
}

int pdot_read_skip(pdot_reader *r, size_t n)
{
    return pdot_read_raw(r, n, NULL);
}

static int get_le(pdot_reader *r, size_t n, uint64_t *v)
{
    const uint8_t *p;
    size_t i;

    if (pdot_read_raw(r, n, &p))
        return PDOT_E_DECODE;
    *v = 0;
    for (i = 0; i < n; i++)
        *v |= (uint64_t)p[i] << (8 * i);
    return 0;
}

int pdot_read_u8(pdot_reader *r, uint8_t *v)
{
    uint64_t t;
    int rc = get_le(r, 1, &t);
    *v = (uint8_t)t;
    return rc;
}

int pdot_read_u16(pdot_reader *r, uint16_t *v)
{
    uint64_t t;
    int rc = get_le(r, 2, &t);
    *v = (uint16_t)t;
    return rc;
}

int pdot_read_u32(pdot_reader *r, uint32_t *v)
{
    uint64_t t;
    int rc = get_le(r, 4, &t);
    *v = (uint32_t)t;
    return rc;
}

int pdot_read_u64(pdot_reader *r, uint64_t *v)
{
    return get_le(r, 8, v);
}

int pdot_read_bool(pdot_reader *r, int *v)
{
    uint8_t t;

    if (pdot_read_u8(r, &t) || t > 1)
        return PDOT_E_DECODE;
    *v = t;
    return 0;
}

int pdot_read_compact(pdot_reader *r, uint64_t *v)
{
    size_t start = r->pos;
    uint8_t first;
    uint64_t t;
    size_t n;

    if (pdot_read_u8(r, &first))
        return PDOT_E_DECODE;
    switch (first & 3) {
    case 0:
        *v = first >> 2;
        return 0;
    case 1:
        r->pos = start;
        if (get_le(r, 2, &t) || (t >> 2) < (1u << 6))
            goto bad;
        *v = t >> 2;
        return 0;
    case 2:
        r->pos = start;
        if (get_le(r, 4, &t) || (t >> 2) < (1u << 14))
            goto bad;
        *v = t >> 2;
        return 0;
    default:
        n = (size_t)(first >> 2) + 4;
        if (n > 8)
            goto bad;  /* > u64; u128 values are not needed */
        if (get_le(r, n, &t))
            goto bad;
        /* Canonical: the top byte is used and small modes would not fit. */
        if ((t >> (8 * (n - 1))) == 0 || t < (1u << 30))
            goto bad;
        *v = t;
        return 0;
    }
bad:
    r->pos = start;
    return PDOT_E_DECODE;
}

int pdot_read_bytes(pdot_reader *r, const uint8_t **p, size_t *n)
{
    uint64_t len;

    if (pdot_read_compact(r, &len) || len > pdot_reader_remaining(r))
        return PDOT_E_DECODE;
    *n = (size_t)len;
    return pdot_read_raw(r, *n, p);
}
