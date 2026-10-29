/*
 * UnixFS file manifests in DAG-PB, as built by the Bulletin TypeScript SDK
 * (UnixFsDagBuilder): one flat node linking every chunk, links in chunk
 * order with an empty Name and Tsize = chunk length, then UnixFS Data
 * { Type: File, filesize, blocksizes[] }.
 */
#ifndef PDOT_DAG_H
#define PDOT_DAG_H

#include <stddef.h>
#include <stdint.h>

#include "pdot.h"
#include "scale.h"

typedef struct {
    pdot_cid cid;
    uint64_t size;    /* Tsize, and the matching blocksizes entry */
} pdot_dag_link;

/* Appends the manifest for nlinks chunks to out. */
int pdot_dag_encode(const pdot_dag_link *links, size_t nlinks, pdot_buf *out);

/*
 * Parses a manifest. *links is a fresh array freed with free(). Rejects
 * anything but a UnixFS file whose blocksizes and filesize agree with its
 * links, and links whose CIDs are not raw-codec leaves.
 */
int pdot_dag_decode(const uint8_t *p, size_t len, pdot_dag_link **links,
                    size_t *nlinks, uint64_t *filesize);

#endif
