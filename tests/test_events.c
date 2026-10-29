/*
 * Generic decoder and event scan against System.Events captured from a dev
 * node and Paseo Bulletin; expected outcomes come from subxt-core
 * (tests/fixtures/events_expected.h).
 */
#include <stdlib.h>

#include "pdot.h"
#include "decode.h"
#include "events.h"
#include "metadata.h"
#include "test.h"
#include "fixtures/events_expected.h"

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n ? (size_t)n : 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static pdot_metadata *load_md(const char *path)
{
    pdot_metadata *md = NULL;
    size_t len;
    uint8_t *blob = load(path, &len);

    if (blob)
        pdot_metadata_parse(blob, len, &md);
    free(blob);
    return md;
}

static void test_outcomes(void)
{
    size_t i;

    for (i = 0; i < sizeof events_expected / sizeof events_expected[0]; i++) {
        pdot_metadata *md = load_md(events_expected[i].metadata);
        pdot_ext_outcome oc;
        uint8_t *ev;
        size_t len;

        ev = load(events_expected[i].events, &len);
        CHECK(md && ev);
        if (md && ev) {
            CHECK(pdot_events_outcome(md, ev, len, events_expected[i].index, &oc) == 0);
            if (oc.success != events_expected[i].success ||
                oc.failed != events_expected[i].failed ||
                strcmp(oc.error, events_expected[i].error) != 0 ||
                oc.has_stored != events_expected[i].has_stored ||
                oc.stored_index != events_expected[i].stored_index ||
                oc.nevents != events_expected[i].nevents) {
                fprintf(stderr, "  %s #%u: got ok=%d failed=%d err='%s' stored=%d/%u n=%zu\n",
                        events_expected[i].events, events_expected[i].index, oc.success,
                        oc.failed, oc.error, oc.has_stored, oc.stored_index, oc.nevents);
                test_failures++;
            }
        }
        free(ev);
        pdot_metadata_free(md);
    }
}

/* Truncated or corrupted event blobs fail cleanly (run under ASan). */
static void test_hostile(void)
{
    pdot_metadata *md = load_md("metadata-v15-paseo.scale");
    pdot_ext_outcome oc;
    uint8_t *ev;
    size_t len, cut, i;
    unsigned seed = 1;

    ev = load("events-paseo-store.scale", &len);
    CHECK(md && ev);
    if (!md || !ev)
        goto out;
    for (cut = 0; cut < len; cut++)
        CHECK(pdot_events_outcome(md, ev, cut, 4, &oc) != 0);
    /* Random byte flips: any result is fine as long as it is memory-safe. */
    for (i = 0; i < 5000; i++) {
        uint8_t *copy = malloc(len);
        size_t k;
        memcpy(copy, ev, len);
        for (k = 0; k < 3; k++) {
            seed = seed * 1103515245u + 12345u;
            copy[(seed >> 8) % len] ^= (uint8_t)(seed >> 20);
        }
        pdot_events_outcome(md, copy, len, 4, &oc);
        free(copy);
    }
out:
    free(ev);
    pdot_metadata_free(md);
}

static void test_authorization(void)
{
    pdot_metadata *md = load_md("metadata-v15-dev.scale");
    pdot_md_storage st;
    uint8_t *v;
    size_t len;
    uint64_t x;

    v = load("authorization-dev-alice.scale", &len);
    CHECK(md && v);
    if (md && v) {
        CHECK(pdot_md_storage_entry(md, "TransactionStorage", "Authorizations", &st) == 0);
        /* Dev genesis grants //Alice 100 transactions and 10 MiB; one 2 KiB store used. */
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "extent.transactions", &x) == 0 && x == 1);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "extent.transactions_allowance", &x) == 0 && x == 100);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "extent.bytes", &x) == 0 && x == 2048);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "extent.bytes_allowance", &x) == 0 && x == 10485760);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "expiration", &x) == 0 && x == 201600);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len, "extent.nope", &x) == PDOT_E_METADATA);
        CHECK(pdot_decode_path_uint(md, st.value_ty, v, len - 1, "expiration", &x) == PDOT_E_DECODE);

        /* The whole value skips to exactly its length. */
        {
            pdot_reader r;
            pdot_reader_init(&r, v, len);
            CHECK(pdot_decode_skip(md, st.value_ty, &r) == 0 && r.pos == len);
        }
    }
    free(v);
    pdot_metadata_free(md);
}

/* Every pallet's storage value types decode their own default values. */
static void test_storage_defaults(void)
{
    pdot_metadata *md = load_md("metadata-v15-paseo.scale");
    size_t i, checked = 0;

    CHECK(md != NULL);
    if (!md)
        return;
    for (i = 0; i < md->npallets; i++) {
        pdot_reader r;
        uint64_t n, j;
        const uint8_t *p;
        size_t plen;

        if (!md->pallets[i].storage_off)
            continue;
        pdot_reader_init(&r, md->blob, md->len);
        r.pos = md->pallets[i].storage_off;
        pdot_read_bytes(&r, &p, &plen);  /* prefix */
        pdot_read_compact(&r, &n);
        for (j = 0; j < n; j++) {
            const char *name;
            size_t name_len;
            pdot_md_storage st;
            pdot_reader v;
            char pallet[64], entry[96];
            uint8_t modifier;

            pdot_read_bytes(&r, &p, &plen);
            name = (const char *)p;
            name_len = plen;
            snprintf(pallet, sizeof pallet, "%.*s", (int)md->pallets[i].name_len, md->pallets[i].name);
            snprintf(entry, sizeof entry, "%.*s", (int)name_len, name);
            CHECK(pdot_md_storage_entry(md, pallet, entry, &st) == 0);
            /* Re-walk this entry to reach its default bytes. */
            pdot_read_u8(&r, &modifier);
            {
                uint8_t kind;
                uint64_t t;
                pdot_read_u8(&r, &kind);
                if (kind == 0) {
                    pdot_read_compact(&r, &t);
                } else {
                    pdot_read_bytes(&r, &p, &plen);
                    pdot_read_compact(&r, &t);
                    pdot_read_compact(&r, &t);
                }
            }
            pdot_read_bytes(&r, &p, &plen);  /* default */
            {
                uint64_t ndocs, d;
                pdot_read_compact(&r, &ndocs);
                for (d = 0; d < ndocs; d++) {
                    const uint8_t *dp;
                    size_t dl;
                    pdot_read_bytes(&r, &dp, &dl);
                }
            }
            /* Optional entries default to None, which is not a value. */
            if (modifier == 1) {
                pdot_reader_init(&v, p, plen);
                if (pdot_decode_skip(md, st.value_ty, &v) != 0 || v.pos != plen) {
                    fprintf(stderr, "  default of %s.%s did not decode\n", pallet, entry);
                    test_failures++;
                }
                checked++;
            }
        }
    }
    CHECK(checked > 20);
    pdot_metadata_free(md);
}

int main(void)
{
    test_outcomes();
    test_hostile();
    test_authorization();
    test_storage_defaults();
    TEST_DONE();
}
