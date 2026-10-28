#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "hex.h"
#include "http.h"
#include "rpc.h"

int pdot_rpc(pdot_client *c, const char *method, cJSON *params, cJSON **result)
{
    cJSON *req, *resp = NULL, *err, *res;
    char *body;
    pdot_buf out;
    int rc;

    *result = NULL;
    if (!params)
        params = cJSON_CreateArray();
    req = cJSON_CreateObject();
    if (!req || !params) {
        cJSON_Delete(req);
        cJSON_Delete(params);
        return pdot_fail(c, PDOT_E_NOMEM, "out of memory");
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", ++c->next_id);
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddItemToObject(req, "params", params);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body)
        return pdot_fail(c, PDOT_E_NOMEM, "out of memory");

    pdot_buf_init(&out);
    rc = pdot_http_request(c, c->rpc_url, body, strlen(body), &out);
    cJSON_free(body);
    if (!rc) {
        resp = cJSON_ParseWithLength((const char *)out.data, out.len);
        if (!resp)
            rc = pdot_fail(c, PDOT_E_RPC, "%s: response is not JSON", method);
    }
    pdot_buf_free(&out);
    if (rc)
        return rc;

    if ((err = cJSON_GetObjectItemCaseSensitive(resp, "error")) != NULL) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(err, "data");
        rc = pdot_fail(c, PDOT_E_RPC, "%s: %s%s%s", method,
                       cJSON_IsString(msg) ? msg->valuestring : "error",
                       cJSON_IsString(data) ? ": " : "",
                       cJSON_IsString(data) ? data->valuestring : "");
    } else if ((res = cJSON_DetachItemFromObjectCaseSensitive(resp, "result")) == NULL) {
        rc = pdot_fail(c, PDOT_E_RPC, "%s: response has no result", method);
    } else {
        *result = res;
    }
    cJSON_Delete(resp);
    return rc;
}

int pdot_rpc_bytes(pdot_client *c, const char *method, cJSON *params,
                   uint8_t **data, size_t *len)
{
    cJSON *res;
    int rc;

    *data = NULL;
    *len = 0;
    if ((rc = pdot_rpc(c, method, params, &res)))
        return rc;
    if (cJSON_IsNull(res))
        rc = PDOT_E_NOTFOUND;
    else if (!cJSON_IsString(res))
        rc = pdot_fail(c, PDOT_E_RPC, "%s: result is not a string", method);
    else if ((rc = pdot_hex_decode_alloc(res->valuestring, data, len)))
        rc = pdot_fail(c, rc, "%s: result is not hex", method);
    cJSON_Delete(res);
    return rc;
}

int pdot_rpc_hash(pdot_client *c, const char *method, cJSON *params, uint8_t out[32])
{
    uint8_t *p;
    size_t n;
    int rc;

    if ((rc = pdot_rpc_bytes(c, method, params, &p, &n)))
        return rc == PDOT_E_NOTFOUND ? pdot_fail(c, rc, "%s: null result", method) : rc;
    if (n == 32)
        memcpy(out, p, 32);
    else
        rc = pdot_fail(c, PDOT_E_RPC, "%s: expected a 32-byte hash", method);
    free(p);
    return rc;
}

cJSON *pdot_rpc_hex_param(const uint8_t *p, size_t len)
{
    cJSON *arr = cJSON_CreateArray();
    char *hex = malloc(2 * len + 3);

    if (!arr || !hex) {
        cJSON_Delete(arr);
        free(hex);
        return NULL;
    }
    pdot_hex_encode(p, len, hex);
    cJSON_AddItemToArray(arr, cJSON_CreateString(hex));
    free(hex);
    return arr;
}

/* Parses a JSON number or "0x.." hex string as u32. */
static int json_u32(const cJSON *v, uint32_t *out)
{
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 4294967295.0) {
        *out = (uint32_t)v->valuedouble;
        return 0;
    }
    if (cJSON_IsString(v)) {
        char *end;
        unsigned long n = strtoul(v->valuestring, &end, 16);
        if (*end == '\0' && end != v->valuestring && n <= 0xffffffffUL) {
            *out = (uint32_t)n;
            return 0;
        }
    }
    return PDOT_E_DECODE;
}

int pdot_rpc_block_hash(pdot_client *c, uint32_t number, uint8_t hash[32])
{
    cJSON *p = cJSON_CreateArray();

    cJSON_AddItemToArray(p, cJSON_CreateNumber(number));
    return pdot_rpc_hash(c, "chain_getBlockHash", p, hash);
}

int pdot_rpc_finalized_head(pdot_client *c, uint8_t hash[32])
{
    return pdot_rpc_hash(c, "chain_getFinalizedHead", NULL, hash);
}

int pdot_rpc_block_number(pdot_client *c, const uint8_t hash[32], uint32_t *number)
{
    cJSON *res;
    int rc;

    if ((rc = pdot_rpc(c, "chain_getHeader", pdot_rpc_hex_param(hash, 32), &res)))
        return rc;
    if (json_u32(cJSON_GetObjectItemCaseSensitive(res, "number"), number))
        rc = pdot_fail(c, PDOT_E_RPC, "chain_getHeader: no block number");
    cJSON_Delete(res);
    return rc;
}

int pdot_rpc_runtime_version(pdot_client *c, uint32_t *spec, uint32_t *tx)
{
    cJSON *res;
    int rc;

    if ((rc = pdot_rpc(c, "state_getRuntimeVersion", NULL, &res)))
        return rc;
    if (json_u32(cJSON_GetObjectItemCaseSensitive(res, "specVersion"), spec) ||
        json_u32(cJSON_GetObjectItemCaseSensitive(res, "transactionVersion"), tx))
        rc = pdot_fail(c, PDOT_E_RPC, "state_getRuntimeVersion: missing versions");
    cJSON_Delete(res);
    return rc;
}

int pdot_rpc_account_nonce(pdot_client *c, const uint8_t account_id[32], uint32_t *nonce)
{
    char addr[PDOT_SS58_MAX_LEN];
    cJSON *p, *res;
    int rc;

    pdot_ss58_encode(account_id, 42, addr, sizeof addr);
    p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateString(addr));
    if ((rc = pdot_rpc(c, "system_accountNextIndex", p, &res)))
        return rc;
    if (json_u32(res, nonce))
        rc = pdot_fail(c, PDOT_E_RPC, "system_accountNextIndex: bad result");
    cJSON_Delete(res);
    return rc;
}

int pdot_rpc_storage(pdot_client *c, const uint8_t *key, size_t key_len,
                     uint8_t **value, size_t *value_len)
{
    return pdot_rpc_bytes(c, "state_getStorage", pdot_rpc_hex_param(key, key_len),
                          value, value_len);
}

int pdot_rpc_state_call(pdot_client *c, const char *fn, const uint8_t *args,
                        size_t args_len, uint8_t **out, size_t *out_len)
{
    cJSON *p = pdot_rpc_hex_param(args, args_len);

    cJSON_InsertItemInArray(p, 0, cJSON_CreateString(fn));
    return pdot_rpc_bytes(c, "state_call", p, out, out_len);
}

int pdot_rpc_block_extrinsics(pdot_client *c, const uint8_t hash[32], cJSON **extrinsics)
{
    cJSON *res, *block, *xts;
    int rc;

    *extrinsics = NULL;
    if ((rc = pdot_rpc(c, "chain_getBlock", pdot_rpc_hex_param(hash, 32), &res)))
        return rc;
    block = cJSON_GetObjectItemCaseSensitive(res, "block");
    xts = cJSON_DetachItemFromObjectCaseSensitive(block, "extrinsics");
    if (!cJSON_IsArray(xts)) {
        cJSON_Delete(xts);
        rc = pdot_fail(c, PDOT_E_RPC, "chain_getBlock: no extrinsics");
    } else {
        *extrinsics = xts;
    }
    cJSON_Delete(res);
    return rc;
}

int pdot_rpc_submit(pdot_client *c, const uint8_t *xt, size_t len, uint8_t tx_hash[32])
{
    return pdot_rpc_hash(c, "author_submitExtrinsic", pdot_rpc_hex_param(xt, len), tx_hash);
}
