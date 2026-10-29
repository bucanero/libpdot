/* Bulletin call and storage-key encoding, separate from the network for tests. */
#ifndef PDOT_BULLETIN_H
#define PDOT_BULLETIN_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"
#include "metadata.h"
#include "scale.h"

int pdot_bulletin_store_call(pdot_client *c, const pdot_metadata *md,
                             const uint8_t *data, size_t len, pdot_buf *out);
int pdot_bulletin_store_cid_call(pdot_client *c, const pdot_metadata *md, uint64_t codec,
                                 const uint8_t *data, size_t len, pdot_buf *out);
int pdot_bulletin_renew_call(pdot_client *c, const pdot_metadata *md,
                             const uint8_t content_hash[32], pdot_buf *out);
int pdot_bulletin_authorize_call(pdot_client *c, const pdot_metadata *md,
                                 const uint8_t who[32], uint32_t transactions,
                                 uint64_t bytes, pdot_buf *out);
int pdot_bulletin_auth_key(pdot_client *c, const pdot_metadata *md,
                           const uint8_t account_id[32], pdot_buf *key);

#endif
