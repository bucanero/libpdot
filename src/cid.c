#include <string.h>

#include "pdot.h"
#include "hash.h"

static size_t put_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;

    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

/* Unsigned LEB128 as used by multiformats: at most 9 bytes, minimal. */
static int get_varint(const uint8_t *p, size_t len, size_t *pos, uint64_t *v)
{
    size_t i;

    *v = 0;
    for (i = 0; i < 9 && *pos < len; i++) {
        uint8_t b = p[(*pos)++];
        *v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80))
            return (i > 0 && b == 0) ? PDOT_E_DECODE : 0;
    }
    return PDOT_E_DECODE;
}

int pdot_cid_compute(pdot_cid *cid, const uint8_t *data, size_t len)
{
    return pdot_cid_compute_codec(cid, PDOT_CODEC_RAW, data, len);
}

int pdot_cid_compute_codec(pdot_cid *cid, uint64_t codec, const uint8_t *data, size_t len)
{
    size_t n = 0;

    if (!cid || (!data && len))
        return PDOT_E_INVALID;
    cid->bytes[n++] = 0x01;
    n += put_varint(cid->bytes + n, codec);
    n += put_varint(cid->bytes + n, PDOT_MH_BLAKE2B_256);
    cid->bytes[n++] = 32;
    pdot_blake2b_256(cid->bytes + n, data, len);
    cid->len = n + 32;
    return 0;
}

/* Splits a binary CIDv1 into its multihash code and digest. */
static int cid_parse(const pdot_cid *cid, uint64_t *mh_code,
                     const uint8_t **digest, size_t *digest_len)
{
    size_t pos = 0;
    uint64_t version, codec, mlen;

    if (get_varint(cid->bytes, cid->len, &pos, &version) || version != 1)
        return PDOT_E_UNSUPPORTED;
    if (get_varint(cid->bytes, cid->len, &pos, &codec) ||
        get_varint(cid->bytes, cid->len, &pos, mh_code) ||
        get_varint(cid->bytes, cid->len, &pos, &mlen) ||
        mlen != cid->len - pos)
        return PDOT_E_DECODE;
    *digest = cid->bytes + pos;
    *digest_len = (size_t)mlen;
    return 0;
}

uint64_t pdot_cid_codec(const pdot_cid *cid)
{
    size_t pos = 0;
    uint64_t version, codec;

    if (!cid || cid->len > PDOT_CID_MAX ||
        get_varint(cid->bytes, cid->len, &pos, &version) || version != 1 ||
        get_varint(cid->bytes, cid->len, &pos, &codec))
        return 0;
    return codec;
}

int pdot_cid_digest(const pdot_cid *cid, uint64_t *mh_code,
                    const uint8_t **digest, size_t *digest_len)
{
    if (!cid || !mh_code || !digest || !digest_len || cid->len > PDOT_CID_MAX)
        return PDOT_E_INVALID;
    return cid_parse(cid, mh_code, digest, digest_len);
}

int pdot_cid_verify(const pdot_cid *cid, const uint8_t *data, size_t len)
{
    uint64_t code;
    const uint8_t *digest;
    size_t dlen;
    uint8_t h[32];
    int rc;

    if (!cid || cid->len > PDOT_CID_MAX || (!data && len))
        return PDOT_E_INVALID;
    if ((rc = cid_parse(cid, &code, &digest, &dlen)))
        return rc;
    if (code != PDOT_MH_BLAKE2B_256 || dlen != 32)
        return PDOT_E_UNSUPPORTED;  /* sha2-256 arrives with mbedTLS */
    pdot_blake2b_256(h, data, len);
    return memcmp(h, digest, 32) ? PDOT_E_MISMATCH : 0;
}

static const char b32_alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";

int pdot_cid_to_string(const pdot_cid *cid, char *out, size_t out_len)
{
    size_t i, n = 0, need;
    uint32_t acc = 0;
    int bits = 0;

    if (!cid || !out || cid->len > PDOT_CID_MAX)
        return PDOT_E_INVALID;
    need = 1 + (cid->len * 8 + 4) / 5 + 1;
    if (out_len < need)
        return PDOT_E_BUFFER;
    out[n++] = 'b';
    for (i = 0; i < cid->len; i++) {
        acc = (acc << 8) | cid->bytes[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[n++] = b32_alphabet[(acc >> bits) & 31];
        }
    }
    if (bits > 0)
        out[n++] = b32_alphabet[(acc << (5 - bits)) & 31];
    out[n] = '\0';
    return 0;
}

int pdot_cid_from_string(pdot_cid *cid, const char *str)
{
    const char *s;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    uint64_t code;
    const uint8_t *digest;
    size_t dlen;

    if (!cid || !str)
        return PDOT_E_INVALID;
    if (str[0] != 'b')
        return PDOT_E_UNSUPPORTED;  /* only base32 lower multibase */
    for (s = str + 1; *s; s++) {
        const char *at = strchr(b32_alphabet, *s);
        if (!at)
            return PDOT_E_DECODE;
        acc = (acc << 5) | (uint32_t)(at - b32_alphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n == PDOT_CID_MAX)
                return PDOT_E_DECODE;
            cid->bytes[n++] = (uint8_t)(acc >> bits);
        }
    }
    /* Leftover bits are padding and must be zero. */
    if (bits >= 5 || (acc & ((1u << bits) - 1)))
        return PDOT_E_DECODE;
    cid->len = n;
    return cid_parse(cid, &code, &digest, &dlen);
}
