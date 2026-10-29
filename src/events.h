/* Reads the outcome of one extrinsic from a block's System.Events. */
#ifndef PDOT_EVENTS_H
#define PDOT_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "metadata.h"
#include "scale.h"

typedef struct {
    int      success;          /* System.ExtrinsicSuccess seen */
    int      failed;           /* System.ExtrinsicFailed seen */
    char     error[96];        /* "Pallet.Error", "BadOrigin", "Token.FundsUnavailable" */
    int      has_stored;
    uint32_t stored_index;     /* TransactionStorage.Stored { index } */
    size_t   nevents;          /* events attributed to the extrinsic */
} pdot_ext_outcome;

/* twox128("System") | twox128("Events") */
void pdot_events_key(uint8_t key[32]);

/* Scans an encoded Vec<EventRecord> for events in phase ApplyExtrinsic(index). */
int pdot_events_outcome(const pdot_metadata *md, const uint8_t *events, size_t len,
                        uint32_t index, pdot_ext_outcome *out);

/* Formats a DispatchError value at r as "Pallet.Error" (Module) or "Variant[.Inner]". */
int pdot_dispatch_error(const pdot_metadata *md, uint32_t ty, pdot_reader *r,
                        char *out, size_t out_len);

#endif
