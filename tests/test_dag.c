/*
 * Chunked-file manifests against vectors from the TypeScript SDK's libraries
 * (tests/fixtures/dag_vectors.h), plus manifest parsing and rejection.
 */
#include <stdlib.h>

#include "pdot.h"
#include "dag.h"
#include "test.h"
#include "fixtures/dag_vectors.h"

/* Builds the manifest for a vector's data, as pdot_bulletin_store_chunked does. */
static int build(size_t size, size_t chunk, pdot_buf *out, pdot_dag_link **links_out,
                 size_t *n_out)
{
    uint8_t *data = malloc(size);
    size_t n = (size + chunk - 1) / chunk, i;
    pdot_dag_link *links = calloc(n, sizeof *links);
    int rc;

    for (i = 0; i < size; i++)
        data[i] = (uint8_t)((i * 31 + 7) & 0xff);
    for (i = 0; i < n; i++) {
        size_t off = i * chunk, len = size - off < chunk ? size - off : chunk;
        pdot_cid_compute(&links[i].cid, data + off, len);
        links[i].size = len;
    }
    rc = pdot_dag_encode(links, n, out);
    free(data);
    *links_out = links;
    *n_out = n;
    return rc;
}

static void test_vectors(void)
{
    size_t v;

    for (v = 0; v < sizeof dag_vectors / sizeof dag_vectors[0]; v++) {
        pdot_dag_link *links, *back;
        size_t n, nback, i;
        uint64_t filesize;
        pdot_buf m;
        pdot_cid root;
        char str[128];

        pdot_buf_init(&m);
        CHECK(build(dag_vectors[v].size, dag_vectors[v].chunk, &m, &links, &n) == 0);
        CHECK_HEX(m.data, m.len, dag_vectors[v].manifest);

        CHECK(pdot_cid_compute_codec(&root, PDOT_CODEC_DAG_PB, m.data, m.len) == 0);
        CHECK(pdot_cid_codec(&root) == PDOT_CODEC_DAG_PB);
        CHECK(pdot_cid_to_string(&root, str, sizeof str) == 0);
        if (strcmp(str, dag_vectors[v].root) != 0) {
            fprintf(stderr, "  %s: root %s, expected %s\n", dag_vectors[v].name, str,
                    dag_vectors[v].root);
            test_failures++;
        }

        /* Round trip through the parser. */
        CHECK(pdot_dag_decode(m.data, m.len, &back, &nback, &filesize) == 0);
        CHECK(nback == n && filesize == dag_vectors[v].size);
        for (i = 0; i < n && i < nback; i++)
            CHECK(back[i].size == links[i].size && back[i].cid.len == links[i].cid.len &&
                  memcmp(back[i].cid.bytes, links[i].cid.bytes, links[i].cid.len) == 0);
        free(back);
        free(links);
        pdot_buf_free(&m);
    }
}

static int decodes(const uint8_t *p, size_t len)
{
    pdot_dag_link *links;
    size_t n;
    uint64_t size;
    int rc = pdot_dag_decode(p, len, &links, &n, &size);

    if (rc == 0)
        free(links);
    return rc == 0;
}

static void test_rejects(void)
{
    pdot_dag_link *links;
    size_t n, cut, i;
    pdot_buf m;
    uint8_t *bad;

    pdot_buf_init(&m);
    CHECK(build(1000, 400, &m, &links, &n) == 0);
    CHECK(decodes(m.data, m.len));

    /* Every truncation fails. */
    for (cut = 0; cut < m.len; cut++)
        CHECK(!decodes(m.data, cut));

    bad = malloc(m.len);

    /* A blocksize that disagrees with its link's Tsize. */
    memcpy(bad, m.data, m.len);
    for (i = m.len; i-- > 0;)
        if (bad[i] == 0x20) {  /* last blocksizes tag; its value follows */
            bad[i + 1] ^= 1;
            break;
        }
    CHECK(!decodes(bad, m.len));

    /* A link to a dag-pb node instead of a raw leaf. */
    memcpy(bad, m.data, m.len);
    CHECK(bad[4] == 0x01 && bad[5] == PDOT_CODEC_RAW);  /* 12 len 0a 26 | 01 55 ... */
    bad[5] = PDOT_CODEC_DAG_PB;
    CHECK(!decodes(bad, m.len));

    /* Not a file: UnixFS type directory. */
    memcpy(bad, m.data, m.len);
    for (i = 0; i + 1 < m.len; i++)
        if (bad[i] == 0x08 && bad[i + 1] == 0x02 && bad[i - 2] == 0x0a) {
            bad[i + 1] = 0x01;
            break;
        }
    CHECK(!decodes(bad, m.len));

    free(bad);
    free(links);
    pdot_buf_free(&m);

    CHECK(pdot_dag_encode(NULL, 0, &m) == PDOT_E_INVALID);
}

int main(void)
{
    test_vectors();
    test_rejects();
    TEST_DONE();
}
