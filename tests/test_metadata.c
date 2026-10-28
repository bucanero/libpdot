/*
 * Metadata parser against blobs fetched from Paseo and Westend Bulletin.
 * PDOT_MD_DUMP=1 prints the pallets and extensions it found.
 */
#include <stdlib.h>

#include "pdot.h"
#include "metadata.h"
#include "test.h"

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
    p = malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void dump(const pdot_metadata *md)
{
    size_t i;

    printf("v%u, %u types, extrinsic v%u\n", md->version, md->ntypes,
           md->extrinsic_version);
    for (i = 0; i < md->npallets; i++)
        printf("  pallet %3u %.*s\n", md->pallets[i].index,
               (int)md->pallets[i].name_len, md->pallets[i].name);
    for (i = 0; i < md->next; i++)
        printf("  ext %-32.*s explicit %4u%s implicit %4u%s\n",
               (int)md->ext[i].name_len, md->ext[i].name,
               md->ext[i].ty, pdot_md_is_zero_sized(md, md->ext[i].ty) ? " (zst)" : "      ",
               md->ext[i].implicit_ty,
               pdot_md_is_zero_sized(md, md->ext[i].implicit_ty) ? " (zst)" : "");
}

static void check_bulletin(const char *path, int version)
{
    pdot_metadata *md;
    pdot_md_storage st;
    uint8_t *blob, pallet, call, idx;
    size_t len, nf;
    const char *name;
    size_t name_len;

    blob = load(path, &len);
    CHECK(blob != NULL);
    if (!blob)
        return;
    CHECK(pdot_metadata_parse(blob, len, &md) == 0);
    free(blob);
    if (!md)
        return;
    if (getenv("PDOT_MD_DUMP"))
        dump(md);

    CHECK(md->version == version);
    CHECK(md->extrinsic_version == 4);
    CHECK(md->have_xt_types == (version >= 15));

    CHECK(pdot_md_call(md, "TransactionStorage", "store", &pallet, &call) == 0);
    CHECK(pallet == 40 && call == 0);
    CHECK(pdot_md_call(md, "DataRenewal", "force_renew", &pallet, &call) == 0);
    CHECK(pallet == 42 && call == 1);
    CHECK(pdot_md_call(md, "TransactionStorage", "renew", &pallet, &call) == PDOT_E_NOTFOUND);
    CHECK(pdot_md_call(md, "NoSuchPallet", "store", &pallet, &call) == PDOT_E_NOTFOUND);

    CHECK(pdot_md_storage_entry(md, "TransactionStorage", "Authorizations", &st) == 0);
    CHECK(pdot_md_name_eq(st.prefix, st.prefix_len, "TransactionStorage"));
    CHECK(st.is_map && st.nhashers == 1 && st.hashers[0] == PDOT_HASHER_BLAKE2_128_CONCAT);
    CHECK(pdot_md_variant(md, st.key_ty, "Account", &idx, &nf) == 0 && idx == 0 && nf == 1);

    if (md->have_xt_types) {
        CHECK(pdot_md_type_name(md, md->address_ty, &name, &name_len) == 0);
        CHECK(pdot_md_name_eq(name, name_len, "MultiAddress"));
        CHECK(pdot_md_variant(md, md->address_ty, "Id", &idx, NULL) == 0 && idx == 0);
        CHECK(pdot_md_variant(md, md->signature_ty, "Sr25519", &idx, NULL) == 0 && idx == 1);
        CHECK(pdot_md_variant(md, md->signature_ty, "Ecdsa", &idx, NULL) == 0 && idx == 2);
    }
    pdot_metadata_free(md);
}

static void check_rejects(void)
{
    static const uint8_t bad_magic[] = { 'm', 'e', 't', 'b', 15, 0 };
    static const uint8_t v16[] = { 'm', 'e', 't', 'a', 16, 0 };
    static const uint8_t truncated[] = { 'm', 'e', 't', 'a', 15, 0x04 };
    pdot_metadata *md;
    uint8_t *blob;
    size_t len, cut;

    CHECK(pdot_metadata_parse(bad_magic, sizeof bad_magic, &md) == PDOT_E_DECODE);
    CHECK(pdot_metadata_parse(v16, sizeof v16, &md) == PDOT_E_UNSUPPORTED);
    CHECK(pdot_metadata_parse(truncated, sizeof truncated, &md) == PDOT_E_DECODE);

    /* Every truncation before the end of the extrinsic section fails cleanly. */
    blob = load("metadata-v15-paseo.scale", &len);
    if (!blob)
        return;
    CHECK(pdot_metadata_parse(blob, len, &md) == 0);
    len = md->parsed_len;
    pdot_metadata_free(md);
    for (cut = 0; cut < len; cut += 997)
        CHECK(pdot_metadata_parse(blob, cut, &md) != 0 && md == NULL);
    free(blob);
}

int main(void)
{
    check_bulletin("metadata-v14-paseo.scale", 14);
    check_bulletin("metadata-v15-paseo.scale", 15);
    check_bulletin("metadata-v14-westend.scale", 14);
    check_bulletin("metadata-v15-westend.scale", 15);
    check_rejects();
    TEST_DONE();
}
