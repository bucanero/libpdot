#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hex.h"

void pdot_hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    *out++ = '0';
    *out++ = 'x';
    for (i = 0; i < len; i++) {
        *out++ = digits[in[i] >> 4];
        *out++ = digits[in[i] & 15];
    }
    *out = '\0';
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_prefix(const char *hex)
{
    return (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) ? hex + 2 : hex;
}

int pdot_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *len)
{
    size_t n, i;

    if (!hex || !out)
        return PDOT_E_INVALID;
    hex = skip_prefix(hex);
    n = strlen(hex);
    if (n % 2 || n / 2 > cap)
        return PDOT_E_DECODE;
    for (i = 0; i < n / 2; i++) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return PDOT_E_DECODE;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    if (len)
        *len = n / 2;
    return 0;
}

int pdot_hex_decode_alloc(const char *hex, uint8_t **out, size_t *len)
{
    size_t n;
    int rc;

    if (!hex || !out)
        return PDOT_E_INVALID;
    n = strlen(skip_prefix(hex)) / 2;
    *out = malloc(n ? n : 1);
    if (!*out)
        return PDOT_E_NOMEM;
    if ((rc = pdot_hex_decode(hex, *out, n, len))) {
        free(*out);
        *out = NULL;
    }
    return rc;
}
