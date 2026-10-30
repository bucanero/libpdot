/* libFuzzer target: metadata parser, the lookups run on its output, and the
 * RFC-0078 metadata hash over it. */
#include <stddef.h>
#include <stdint.h>

#include "mdhash.h"
#include "metadata.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pdot_metadata *md;
    pdot_md_storage st;
    pdot_mdhash_info info = { 1, "fuzz", 42, 12, "UNIT" };
    uint8_t a, b, h[32];
    size_t i;

    if (pdot_metadata_parse(data, size, &md))
        return 0;
    pdot_md_call(md, "TransactionStorage", "store", &a, &b);
    pdot_md_storage_entry(md, "System", "Events", &st);
    for (i = 0; i < md->next; i++)
        pdot_md_is_zero_sized(md, md->ext[i].ty);
    pdot_md_ss58_prefix(md, &info.ss58_prefix);
    pdot_metadata_hash(md, &info, h);
    pdot_metadata_free(md);
    return 0;
}
