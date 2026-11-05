#include <string.h>

#include "pdot.h"

/* v /= 10, returning the remainder; long division over 32-bit limbs. */
static unsigned div10(pdot_u128 *v)
{
    uint32_t limb[4];
    uint64_t rem = 0;
    int i;

    limb[0] = (uint32_t)(v->hi >> 32);
    limb[1] = (uint32_t)v->hi;
    limb[2] = (uint32_t)(v->lo >> 32);
    limb[3] = (uint32_t)v->lo;
    for (i = 0; i < 4; i++) {
        uint64_t cur = (rem << 32) | limb[i];
        limb[i] = (uint32_t)(cur / 10);
        rem = cur % 10;
    }
    v->hi = ((uint64_t)limb[0] << 32) | limb[1];
    v->lo = ((uint64_t)limb[2] << 32) | limb[3];
    return (unsigned)rem;
}

int pdot_u128_format(pdot_u128 v, uint8_t decimals, char *out, size_t out_len)
{
    char digits[40];  /* 2^128 has 39 digits; least significant first */
    size_t n = 0, nint, nfrac, need, i, pos = 0;

    if (!out)
        return PDOT_E_INVALID;
    do {
        digits[n++] = (char)('0' + div10(&v));
    } while (v.lo || v.hi);

    /* The fraction is digits decimals-1 .. 0; drop its trailing zeros. */
    nfrac = decimals;
    while (nfrac > 0) {
        size_t d = (size_t)decimals - nfrac;  /* its last digit */
        if (d < n && digits[d] != '0')
            break;
        nfrac--;
    }
    nint = n > decimals ? n - decimals : 1;
    need = nint + (nfrac ? 1 + nfrac : 0) + 1;
    if (need > out_len)
        return PDOT_E_BUFFER;

    if (n > decimals)
        for (i = n; i > decimals; i--)
            out[pos++] = digits[i - 1];
    else
        out[pos++] = '0';
    if (nfrac) {
        out[pos++] = '.';
        for (i = 0; i < nfrac; i++) {
            size_t d = (size_t)decimals - 1 - i;  /* digit index, most significant first */
            out[pos++] = d < n ? digits[d] : '0';
        }
    }
    out[pos] = '\0';
    return 0;
}

double pdot_u128_to_double(pdot_u128 v, uint8_t decimals)
{
    double d = (double)v.hi * 18446744073709551616.0 + (double)v.lo;

    while (decimals--)
        d /= 10.0;
    return d;
}

/* v = v * 10 + d; PDOT_E_INVALID on overflow. */
static int mul10_add(pdot_u128 *v, unsigned d)
{
    uint32_t limb[4];
    uint64_t carry = d;
    int i;

    limb[0] = (uint32_t)v->lo;
    limb[1] = (uint32_t)(v->lo >> 32);
    limb[2] = (uint32_t)v->hi;
    limb[3] = (uint32_t)(v->hi >> 32);
    for (i = 0; i < 4; i++) {
        uint64_t cur = (uint64_t)limb[i] * 10 + carry;
        limb[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    if (carry)
        return PDOT_E_INVALID;
    v->lo = ((uint64_t)limb[1] << 32) | limb[0];
    v->hi = ((uint64_t)limb[3] << 32) | limb[2];
    return 0;
}

int pdot_u128_parse(const char *text, uint8_t decimals, pdot_u128 *out)
{
    const char *p = text;
    unsigned frac = 0;
    int digits = 0, dot = 0;

    if (!text || !out)
        return PDOT_E_INVALID;
    out->lo = out->hi = 0;
    for (; *p; p++) {
        if (*p == '.' && !dot) {
            dot = 1;
            continue;
        }
        if (*p < '0' || *p > '9')
            return PDOT_E_INVALID;
        if (dot && frac == decimals)
            return PDOT_E_INVALID;  /* more precision than the token has */
        if (mul10_add(out, (unsigned)(*p - '0')))
            return PDOT_E_INVALID;
        frac += dot;
        digits++;
    }
    if (!digits)
        return PDOT_E_INVALID;
    for (; frac < decimals; frac++)
        if (mul10_add(out, 0))
            return PDOT_E_INVALID;
    return 0;
}
