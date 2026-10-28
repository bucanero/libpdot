#include "pdot.h"
#include "scale.h"
#include "test.h"

/* Vectors from parity-scale-codec's compact tests. */
static const struct {
    uint64_t    value;
    const char *hex;
} compact_vectors[] = {
    { 0, "00" },
    { 1, "04" },
    { 63, "fc" },
    { 64, "0101" },
    { 16383, "fdff" },
    { 16384, "02000100" },
    { 1073741823, "feffffff" },
    { 1073741824, "0300000040" },
    { 0xffffffffULL, "03ffffffff" },
    { 0x100000000ULL, "070000000001" },
    { 0xffffffffffULL, "07ffffffffff" },
    { 0x10000000000ULL, "0b000000000001" },
    { 0xffffffffffffULL, "0bffffffffffff" },
    { 0x1000000000000ULL, "0f00000000000001" },
    { 0xffffffffffffffULL, "0fffffffffffffff" },
    { 0x100000000000000ULL, "130000000000000001" },
    { 0xffffffffffffffffULL, "13ffffffffffffffff" },
};

static void test_compact(void)
{
    size_t i;

    for (i = 0; i < sizeof compact_vectors / sizeof compact_vectors[0]; i++) {
        pdot_buf b;
        pdot_reader r;
        uint64_t v = 0;

        pdot_buf_init(&b);
        pdot_scale_compact(&b, compact_vectors[i].value);
        CHECK(b.err == 0);
        CHECK_HEX(b.data, b.len, compact_vectors[i].hex);
        CHECK(pdot_scale_compact_len(compact_vectors[i].value) == b.len);

        pdot_reader_init(&r, b.data, b.len);
        CHECK(pdot_read_compact(&r, &v) == 0);
        CHECK(v == compact_vectors[i].value);
        CHECK(pdot_reader_remaining(&r) == 0);
        pdot_buf_free(&b);
    }
}

static void expect_compact_error(const char *hex)
{
    uint8_t in[32];
    long n = from_hex(hex, in, sizeof in);
    pdot_reader r;
    uint64_t v;

    pdot_reader_init(&r, in, (size_t)n);
    if (pdot_read_compact(&r, &v) != PDOT_E_DECODE) {
        fprintf(stderr, "  compact %s should not decode\n", hex);
        test_failures++;
    }
    CHECK(r.pos == 0);
}

static void test_compact_rejects(void)
{
    expect_compact_error("");
    expect_compact_error("01");              /* truncated */
    expect_compact_error("0100");            /* 0 in two-byte mode */
    expect_compact_error("fd00");            /* 63 in two-byte mode */
    expect_compact_error("02000000");        /* 0 in four-byte mode */
    expect_compact_error("feff0000");        /* 16383 in four-byte mode */
    expect_compact_error("03ffffff3f");      /* 2^30-1 in big mode */
    expect_compact_error("07000000ff00");    /* 0xff000000 in 5 bytes */
    expect_compact_error("17000000000000000001"); /* > u64 */
}

static void test_fixed_width(void)
{
    pdot_buf b;
    pdot_reader r;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    int flag;

    pdot_buf_init(&b);
    pdot_scale_u8(&b, 0x2a);
    pdot_scale_u16(&b, 0x1234);
    pdot_scale_u32(&b, 0xdeadbeef);
    pdot_scale_u64(&b, 0x0102030405060708ULL);
    pdot_scale_bool(&b, 1);
    CHECK_HEX(b.data, b.len, "2a3412efbeadde080706050403020101");

    pdot_reader_init(&r, b.data, b.len);
    CHECK(pdot_read_u8(&r, &u8) == 0 && u8 == 0x2a);
    CHECK(pdot_read_u16(&r, &u16) == 0 && u16 == 0x1234);
    CHECK(pdot_read_u32(&r, &u32) == 0 && u32 == 0xdeadbeef);
    CHECK(pdot_read_u64(&r, &u64) == 0 && u64 == 0x0102030405060708ULL);
    CHECK(pdot_read_bool(&r, &flag) == 0 && flag == 1);
    CHECK(pdot_read_u8(&r, &u8) == PDOT_E_DECODE);
    pdot_buf_free(&b);

    {
        static const uint8_t two = 2;
        pdot_reader_init(&r, &two, 1);
        CHECK(pdot_read_bool(&r, &flag) == PDOT_E_DECODE);
    }
}

static void test_bytes(void)
{
    static const uint8_t data[] = { 1, 2, 3, 4 };
    uint8_t big[300];
    pdot_buf b;
    pdot_reader r;
    const uint8_t *p;
    size_t n;

    pdot_buf_init(&b);
    pdot_scale_bytes(&b, data, sizeof data);
    CHECK_HEX(b.data, b.len, "1001020304");
    pdot_reader_init(&r, b.data, b.len);
    CHECK(pdot_read_bytes(&r, &p, &n) == 0 && n == 4 && p[3] == 4);

    /* Length prefix claims more than is present. */
    pdot_reader_init(&r, b.data, b.len - 1);
    CHECK(pdot_read_bytes(&r, &p, &n) == PDOT_E_DECODE);
    pdot_buf_free(&b);

    memset(big, 0xab, sizeof big);
    pdot_buf_init(&b);
    pdot_scale_bytes(&b, big, sizeof big);
    CHECK(b.len == 2 + sizeof big);
    CHECK_HEX(b.data, 2, "b104");
    pdot_buf_free(&b);
}

int main(void)
{
    test_compact();
    test_compact_rejects();
    test_fixed_width();
    test_bytes();
    TEST_DONE();
}
