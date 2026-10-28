/* Minimal test harness: CHECK macros and hex helpers. */
#ifndef PDOT_TEST_H
#define PDOT_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__)
#define TEST_UNUSED __attribute__((unused))
#else
#define TEST_UNUSED
#endif

static int test_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
            test_failures++;                                              \
        }                                                                 \
    } while (0)

#define CHECK_HEX(buf, len, hex) CHECK(hex_eq((buf), (len), (hex)))

#define TEST_DONE()                                                       \
    do {                                                                  \
        if (test_failures)                                                \
            fprintf(stderr, "%d failure(s)\n", test_failures);            \
        return test_failures ? 1 : 0;                                     \
    } while (0)

static TEST_UNUSED int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes hex into out; returns the byte count or -1. */
static TEST_UNUSED long from_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2, i;

    if (strlen(hex) % 2 || n > cap)
        return -1;
    for (i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (long)n;
}

static TEST_UNUSED int hex_eq(const uint8_t *buf, size_t len, const char *hex)
{
    size_t i;
    int ok = strlen(hex) == 2 * len;

    for (i = 0; ok && i < len; i++)
        ok = hex_nibble(hex[2 * i]) == buf[i] >> 4 &&
             hex_nibble(hex[2 * i + 1]) == (buf[i] & 15);
    if (!ok) {
        fprintf(stderr, "  expected %s\n  got      ", hex);
        for (i = 0; i < len; i++)
            fprintf(stderr, "%02x", buf[i]);
        fprintf(stderr, "\n");
    }
    return ok;
}

#endif
