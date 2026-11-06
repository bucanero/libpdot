/*
 * RFC-0078 metadata hash: the value CheckMetadataHash adds to the signed
 * data in Mode::Enabled. A port of the merkleized-metadata crate, which
 * substrate-wasm-builder uses to embed the hash in the runtime.
 */
#ifndef PDOT_MDHASH_H
#define PDOT_MDHASH_H

#include <stddef.h>
#include <stdint.h>

#include "metadata.h"

/* MetadataDigest::V1 fields that do not come from the type registry. */
typedef struct {
    uint32_t    spec_version;
    const char *spec_name;
    uint16_t    ss58_prefix;    /* the System.SS58Prefix constant */
    uint8_t     decimals;       /* as given to the runtime build */
    const char *token_symbol;   /* likewise */
} pdot_mdhash_info;

/*
 * blake3(SCALE(MetadataDigest::V1)) over v15 metadata. PDOT_E_UNSUPPORTED
 * for v14, which lacks the extrinsic types; PDOT_E_METADATA if the
 * registry has a shape the RFC cannot express.
 */
int pdot_metadata_hash(const pdot_metadata *md, const pdot_mdhash_info *info,
                       uint8_t out[32]);

/* The SS58 prefix from the System.SS58Prefix constant. */
int pdot_md_ss58_prefix(const pdot_metadata *md, uint16_t *prefix);

#endif
