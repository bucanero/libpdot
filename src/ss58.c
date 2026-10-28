#include <string.h>

#include "pdot.h"
#include "hash.h"

static const char b58_alphabet[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Max raw SS58 payload: 2-byte prefix + 32-byte account + 2-byte checksum. */
#define SS58_RAW_MAX 36

static int b58_encode(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    uint8_t digits[SS58_RAW_MAX * 138 / 100 + 1];
    size_t ndigits = 0, zeros = 0, i, j, n = 0;

    while (zeros < len && in[zeros] == 0)
        zeros++;
    for (i = zeros; i < len; i++) {
        unsigned carry = in[i];
        for (j = 0; j < ndigits; j++) {
            carry += (unsigned)digits[j] << 8;
            digits[j] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            digits[ndigits++] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }
    if (zeros + ndigits + 1 > out_len)
        return PDOT_E_BUFFER;
    for (i = 0; i < zeros; i++)
        out[n++] = '1';
    for (i = ndigits; i > 0; i--)
        out[n++] = b58_alphabet[digits[i - 1]];
    out[n] = '\0';
    return 0;
}

static int b58_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t bytes[SS58_RAW_MAX];
    size_t nbytes = 0, zeros = 0, i, j;
    const char *s = in;

    while (*s == '1') {
        zeros++;
        s++;
    }
    for (; *s; s++) {
        const char *at = strchr(b58_alphabet, *s);
        unsigned carry;
        if (!at)
            return PDOT_E_DECODE;
        carry = (unsigned)(at - b58_alphabet);
        for (j = 0; j < nbytes; j++) {
            carry += (unsigned)bytes[j] * 58;
            bytes[j] = (uint8_t)carry;
            carry >>= 8;
        }
        while (carry) {
            if (nbytes == sizeof bytes)
                return PDOT_E_DECODE;
            bytes[nbytes++] = (uint8_t)carry;
            carry >>= 8;
        }
    }
    if (zeros + nbytes > out_cap)
        return PDOT_E_DECODE;
    memset(out, 0, zeros);
    for (i = 0; i < nbytes; i++)
        out[zeros + i] = bytes[nbytes - 1 - i];
    *out_len = zeros + nbytes;
    return 0;
}

static void ss58_checksum(const uint8_t *data, size_t len, uint8_t out[2])
{
    static const uint8_t pre[] = { 'S', 'S', '5', '8', 'P', 'R', 'E' };
    pdot_blake2b_ctx ctx;
    uint8_t h[64];

    pdot_blake2b_init(&ctx, 64);
    pdot_blake2b_update(&ctx, pre, sizeof pre);
    pdot_blake2b_update(&ctx, data, len);
    pdot_blake2b_final(&ctx, h);
    out[0] = h[0];
    out[1] = h[1];
}

int pdot_ss58_encode(const uint8_t account_id[32], uint16_t prefix,
                     char *out, size_t out_len)
{
    uint8_t raw[SS58_RAW_MAX];
    size_t n = 0;

    if (!account_id || !out || prefix > 16383)
        return PDOT_E_INVALID;
    if (prefix < 64) {
        raw[n++] = (uint8_t)prefix;
    } else {
        raw[n++] = (uint8_t)(((prefix & 0xfc) >> 2) | 0x40);
        raw[n++] = (uint8_t)((prefix >> 8) | ((prefix & 0x03) << 6));
    }
    memcpy(raw + n, account_id, 32);
    n += 32;
    ss58_checksum(raw, n, raw + n);
    n += 2;
    return b58_encode(raw, n, out, out_len);
}

int pdot_ss58_decode(const char *address, uint8_t account_id[32],
                     uint16_t *prefix)
{
    uint8_t raw[SS58_RAW_MAX], sum[2];
    size_t n, plen;
    uint16_t ident;

    if (!address || !account_id)
        return PDOT_E_INVALID;
    if (b58_decode(address, raw, sizeof raw, &n) || n < 1)
        return PDOT_E_DECODE;
    if (raw[0] < 64) {
        plen = 1;
        ident = raw[0];
    } else if (raw[0] < 128) {
        if (n < 2)
            return PDOT_E_DECODE;
        plen = 2;
        ident = (uint16_t)(((raw[0] & 0x3f) << 2) | (raw[1] >> 6) |
                           ((raw[1] & 0x3f) << 8));
    } else {
        return PDOT_E_DECODE;
    }
    if (n != plen + 32 + 2)
        return PDOT_E_UNSUPPORTED;  /* only 32-byte account ids */
    ss58_checksum(raw, plen + 32, sum);
    if (memcmp(sum, raw + plen + 32, 2))
        return PDOT_E_CHECKSUM;
    memcpy(account_id, raw + plen, 32);
    if (prefix)
        *prefix = ident;
    return 0;
}
