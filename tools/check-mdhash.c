/*
 * Read-only check that live runtimes accept libpdot's metadata hash:
 *
 *   check_mdhash RPC_URL...
 *
 * For each chain, signs a store transaction with CheckMetadataHash on (as
 * //Alice) and asks TaggedTransactionQueue_validate_transaction about it;
 * nothing is submitted. A hash the runtime does not share fails signature
 * checks (InvalidTransaction::BadProof); a runtime built without one gives
 * UnknownTransaction::CannotLookup. Anything else, such as //Alice lacking
 * an authorization, means the signature verified.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"
#include "bulletin.h"
#include "client.h"
#include "hex.h"
#include "jsonrpc.h"
#include "tx.h"

static const char alice[] = "e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a";

static int check(const char *url, const pdot_signer *s)
{
    pdot_client *c = pdot_client_new(url, NULL);
    pdot_tx_params p;
    pdot_buf call, xt, args;
    uint8_t *out = NULL, h[32];
    size_t n = 0;
    char hex[80];
    int rc, ok = 0;

    pdot_buf_init(&call);
    pdot_buf_init(&xt);
    pdot_buf_init(&args);
    if (!c || pdot_client_set_metadata_hash(c, 1, NULL, -1) ||
        (rc = pdot_client_metadata_hash(c, h)) ||
        (rc = pdot_tx_params_fetch(c, s, &p)) ||
        (rc = pdot_bulletin_store_call(c, p.md, (const uint8_t *)"libpdot", 7, &call)) ||
        (rc = pdot_tx_build(c, &p, s, call.data, call.len, &xt))) {
        printf("FAIL %s: %s\n", url, c ? pdot_last_error(c) : "no client");
        goto done;
    }
    pdot_scale_u8(&args, 2);   /* TransactionSource::External */
    pdot_scale_raw(&args, xt.data, xt.len);
    pdot_scale_raw(&args, p.era_block_hash, 32);
    if ((rc = pdot_rpc_state_call(c, "TaggedTransactionQueue_validate_transaction",
                                  args.data, args.len, &out, &n))) {
        printf("FAIL %s: %s\n", url, pdot_last_error(c));
        goto done;
    }
    pdot_hex_encode(h, 32, hex);
    if (n >= 3 && out[0] == 1 && out[1] == 0 && out[2] == 4)
        printf("FAIL %s: bad signature: the runtime's hash is not %s\n", url, hex);
    else if (n >= 3 && out[0] == 1 && out[1] == 1 && out[2] == 0)
        printf("FAIL %s: the runtime was built without a metadata hash\n", url);
    else {
        printf("ok   %s: signature verifies with metadata hash %s\n", url, hex);
        ok = 1;
    }
done:
    free(out);
    pdot_buf_free(&call);
    pdot_buf_free(&xt);
    pdot_buf_free(&args);
    pdot_client_free(c);
    return ok;
}

int main(int argc, char **argv)
{
    uint8_t seed[32];
    size_t len;
    pdot_signer s;
    int i, failed = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s RPC_URL...\n", argv[0]);
        return 2;
    }
    if (pdot_hex_decode(alice, seed, sizeof seed, &len) || pdot_signer_sr25519_from_seed(&s, seed))
        return 1;
    for (i = 1; i < argc; i++)
        failed += !check(argv[i], &s);
    pdot_signer_wipe(&s);
    return failed ? 1 : 0;
}
