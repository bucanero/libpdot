/* JSON-RPC 2.0 over HTTP POST, plus the legacy Substrate methods libpdot uses. */
#ifndef PDOT_RPC_H
#define PDOT_RPC_H

#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "client.h"

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
int pdot_rpc_runtime_version(pdot_client *c, uint32_t *spec, uint32_t *tx);
int pdot_rpc_account_nonce(pdot_client *c, const uint8_t account_id[32], uint32_t *nonce);
int pdot_rpc_storage(pdot_client *c, const uint8_t *key, size_t key_len,
                     uint8_t **value, size_t *value_len);
int pdot_rpc_state_call(pdot_client *c, const char *fn, const uint8_t *args,
                        size_t args_len, uint8_t **out, size_t *out_len);
/* Extrinsics of a block, as returned by chain_getBlock (cJSON array of hex). */
int pdot_rpc_block_extrinsics(pdot_client *c, const uint8_t hash[32], cJSON **extrinsics);
int pdot_rpc_submit(pdot_client *c, const uint8_t *xt, size_t len, uint8_t tx_hash[32]);

#endif
