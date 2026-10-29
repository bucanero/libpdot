/* libFuzzer target: metadata parser and the lookups run on its output. */
#include <stddef.h>
#include <stdint.h>

#include "metadata.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pdot_metadata *md;
    pdot_md_storage st;
    uint8_t a, b;
    size_t i;

    if (pdot_metadata_parse(data, size, &md))
        return 0;
    pdot_md_call(md, "TransactionStorage", "store", &a, &b);
    pdot_md_storage_entry(md, "System", "Events", &st);
    for (i = 0; i < md->next; i++)
        pdot_md_is_zero_sized(md, md->ext[i].ty);
    pdot_metadata_free(md);
    return 0;
}
