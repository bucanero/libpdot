/*
 * libFuzzer target: light-client parsers over hostile data. The first byte
 * picks one: 0 header, 1 GRANDPA justification (under the Paseo set from
 * tests/fixtures/verify-paseo.json, or PDOT_FUZZ_SET), 2 trie read proof
 * as root (32) | key length (1) | key | Vec<Vec<u8>> nodes, 3 ScheduledChange.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grandpa.h"
#include "scale.h"
#include "trie.h"

static pdot_grandpa_set set;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    /* The set is the hex "authorities" of a fixture, read without a JSON parser. */
    const char *path = getenv("PDOT_FUZZ_SET");
    FILE *f = fopen(path ? path : "tests/fixtures/verify-paseo.json", "rb");
    static char buf[1 << 16];
    size_t n, i, len = 0;
    char *a;
    static uint8_t raw[1 << 15];

    (void)argc;
    (void)argv;
    if (!f)
        abort();
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    if (!(a = strstr(buf, "\"authorities\"")) || !(a = strstr(a, "0x")))
        abort();
    a += 2;
    for (i = 0; a[i] != '"' && a[i + 1] != '"' && len < sizeof raw; i += 2) {
        unsigned v;
        sscanf(a + i, "%2x", &v);
        raw[len++] = (uint8_t)v;
    }
    if (pdot_grandpa_set_decode(raw, len, 470, &set))
        abort();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pdot_header h;
    pdot_grandpa_stats st;
    pdot_grandpa_set next;
    uint8_t target[32];
    uint32_t number, delay;

    if (size < 1)
        return 0;
    switch (data[0] % 4) {
    case 0:
        pdot_header_decode(data + 1, size - 1, &h);
        break;
    case 1:
        pdot_grandpa_verify(&set, data + 1, size - 1, target, &number, &st);
        break;
    case 2: {
        pdot_reader r;
        const uint8_t *root, *key, *v;
        uint8_t klen;
        uint64_t n, i;
        pdot_trie_node nodes[64];
        size_t vl, count = 0;

        pdot_reader_init(&r, data + 1, size - 1);
        if (pdot_read_raw(&r, 32, &root) || pdot_read_u8(&r, &klen) ||
            pdot_read_raw(&r, klen, &key) || pdot_read_compact(&r, &n))
            break;
        for (i = 0; i < n && count < 64; i++) {
            if (pdot_read_bytes(&r, &nodes[count].data, &nodes[count].len))
                break;
            count++;
        }
        pdot_trie_hash_nodes(nodes, count);
        pdot_trie_read(root, nodes, count, key, klen, &v, &vl);
        break;
    }
    case 3:
        if (pdot_grandpa_scheduled_change(data + 1, size - 1, &next, &delay) == 0)
            pdot_grandpa_set_free(&next);
        break;
    }
    return 0;
}
