/* JSON-RPC 2.0 over HTTP POST, plus the legacy Substrate methods libpdot uses. */
#ifndef PDOT_RPC_H
#define PDOT_RPC_H

#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "client.h"
#include "scale.h"
#include "trie.h"

/*
 * Calls method with params (a JSON array, consumed; NULL means []). On
 * success *result is the detached "result" member, freed with cJSON_Delete.
 */
int pdot_rpc(pdot_client *c, const char *method, cJSON *params, cJSON **result);

/* Hex-string result decoded into a fresh allocation; null result is NOTFOUND. */
int pdot_rpc_bytes(pdot_client *c, const char *method, cJSON *params,
                   uint8_t **data, size_t *len);
/* Hex-string result that must be exactly 32 bytes. */
int pdot_rpc_hash(pdot_client *c, const char *method, cJSON *params, uint8_t out[32]);

/* Params helpers: one hex-encoded byte string, or one hash. */
cJSON *pdot_rpc_hex_param(const uint8_t *p, size_t len);

/* Typed wrappers for the legacy RPC methods. */
int pdot_rpc_block_hash(pdot_client *c, uint32_t number, uint8_t hash[32]);
int pdot_rpc_finalized_head(pdot_client *c, uint8_t hash[32]);
int pdot_rpc_block_number(pdot_client *c, const uint8_t hash[32], uint32_t *number);
/* spec_name may be NULL; otherwise it gets the NUL-terminated specName. */
int pdot_rpc_runtime_version(pdot_client *c, uint32_t *spec, uint32_t *tx,
                             char *spec_name, size_t cap);
/* The first token's symbol and decimals from system_properties. */
int pdot_rpc_token(pdot_client *c, char *symbol, size_t cap, uint8_t *decimals);
int pdot_rpc_account_nonce(pdot_client *c, const uint8_t account_id[32], uint32_t *nonce);
int pdot_rpc_storage(pdot_client *c, const uint8_t *key, size_t key_len,
                     uint8_t **value, size_t *value_len);
/* state_getStorage at a given block. */
int pdot_rpc_storage_at(pdot_client *c, const uint8_t *key, size_t key_len,
                        const uint8_t at[32], uint8_t **value, size_t *value_len);
/* Number of the best (not necessarily finalized) block. */
int pdot_rpc_best_number(pdot_client *c, uint32_t *number);
/* state_call at a given block. */
int pdot_rpc_state_call_at(pdot_client *c, const char *fn, const uint8_t *args, size_t args_len,
                           const uint8_t at[32], uint8_t **out, size_t *out_len);
/* chain_getHeader(hash), re-encoded to SCALE so its hash can be checked. */
int pdot_rpc_header(pdot_client *c, const uint8_t hash[32], pdot_buf *encoded);
/* state_getReadProof([key], at). Node hashes are computed. */
int pdot_rpc_read_proof(pdot_client *c, const uint8_t *key, size_t key_len,
                        const uint8_t at[32], pdot_trie_proof *proof);
/* One proof covering several keys. */
int pdot_rpc_read_proof_keys(pdot_client *c, const uint8_t *const *keys, const size_t *lens,
                             size_t nkeys, const uint8_t at[32], pdot_trie_proof *proof);

/* A storage value read by key; data is NULL and present 0 when absent. */
typedef struct {
    uint8_t *data;
    size_t   len;
    int      present;
} pdot_value;

void pdot_values_free(pdot_value *values, size_t n);

/* state_queryStorageAt(keys, at): values[i] for keys[i]. */
int pdot_rpc_query_storage_at(pdot_client *c, const uint8_t *const *keys, const size_t *lens,
                              size_t nkeys, const uint8_t at[32], pdot_value *values);

/* state_getKeysPaged(prefix, count, start, at): a cJSON array of hex keys. */
int pdot_rpc_keys_paged(pdot_client *c, const uint8_t *prefix, size_t prefix_len,
                        uint32_t count, const uint8_t *start, size_t start_len,
                        const uint8_t at[32], cJSON **keys);

/* grandpa_proveFinality(number): an encoded FinalityProof, or PDOT_E_NOTFOUND. */
int pdot_rpc_prove_finality(pdot_client *c, uint32_t number, uint8_t **proof, size_t *len);
int pdot_rpc_state_call(pdot_client *c, const char *fn, const uint8_t *args,
                        size_t args_len, uint8_t **out, size_t *out_len);
/* Extrinsics of a block, as returned by chain_getBlock (cJSON array of hex). */
int pdot_rpc_block_extrinsics(pdot_client *c, const uint8_t hash[32], cJSON **extrinsics);
int pdot_rpc_submit(pdot_client *c, const uint8_t *xt, size_t len, uint8_t tx_hash[32]);

#endif
