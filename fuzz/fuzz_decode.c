/*
 * libFuzzer target: generic SCALE decoder, event scan and chunked-file
 * manifest parser over hostile data,
 * with real Bulletin metadata (PDOT_FUZZ_METADATA, default
 * tests/fixtures/metadata-v15-paseo.scale). The first 4 input bytes pick a
 * type id for pdot_decode_skip; the whole input is also scanned as events.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dag.h"
#include "decode.h"
#include "events.h"
#include "metadata.h"

static pdot_metadata *md;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *path = getenv("PDOT_FUZZ_METADATA");
    FILE *f;
    uint8_t *blob;
    long n;

    (void)argc;
    (void)argv;
    f = fopen(path ? path : "tests/fixtures/metadata-v15-paseo.scale", "rb");
    if (!f)
        abort();
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n || pdot_metadata_parse(blob, (size_t)n, &md))
        abort();
    free(blob);
    fclose(f);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pdot_ext_outcome oc;
    pdot_reader r;
    pdot_dag_link *links;
    size_t nlinks;
    uint64_t filesize;

    if (pdot_dag_decode(data, size, &links, &nlinks, &filesize) == 0)
        free(links);

    pdot_events_outcome(md, data, size, data && size ? data[0] : 0, &oc);
    if (size >= 4) {
        uint32_t ty = (uint32_t)data[0] | (uint32_t)data[1] << 8 |
                      (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        pdot_reader_init(&r, data + 4, size - 4);
        pdot_decode_skip(md, ty % md->ntypes, &r);
    }
    return 0;
}
