/*
 * Store a message on the Bulletin Chain, then read it back and verify it.
 *
 *   PDOT_SEED=<32-byte hex seed> ./store_and_fetch [rpc-url]
 *
 * The seed's account needs a storage authorization. On a dev node, //Alice
 * has one: PDOT_SEED=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdot.h>

static int parse_seed(const char *hex, uint8_t seed[32])
{
    int i;
    unsigned v;

    if (!hex || strlen(hex) != 64)
        return -1;
    for (i = 0; i < 32; i++) {
        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
            return -1;
        seed[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *rpc = argc > 1 ? argv[1] : "http://127.0.0.1:9944";
    const char *msg = "Hello from C!";
    char addr[PDOT_SS58_MAX_LEN], cid[128];
    uint8_t seed[32], *data;
    size_t len;
    pdot_client *c;
    pdot_signer signer;
    pdot_store_result res;
    int authorized, rc;

    if (parse_seed(getenv("PDOT_SEED"), seed)) {
        fprintf(stderr, "set PDOT_SEED to a 32-byte hex seed\n");
        return 1;
    }

    /* 1. A client (no IPFS gateway: data comes back from the node) and a key. */
    c = pdot_client_new(rpc, NULL);
    if (!c || pdot_signer_sr25519_from_seed(&signer, seed)) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    memset(seed, 0, sizeof seed);
    pdot_ss58_encode(signer.account_id, 42, addr, sizeof addr);
    printf("account  %s\n", addr);

    /* 2. Storing needs an authorization on chain. */
    if ((rc = pdot_bulletin_is_authorized(c, signer.account_id, &authorized)) || !authorized) {
        fprintf(stderr, "not authorized: %s\n", rc ? pdot_last_error(c) : "no grant");
        goto out;
    }

    /* 3. Store. Blocks until the transaction is in a finalized block. */
    rc = pdot_bulletin_store(c, &signer, (const uint8_t *)msg, strlen(msg), &res);
    if (rc) {
        fprintf(stderr, "store: %s (%s)\n", pdot_strerror(rc), pdot_last_error(c));
        goto out;
    }
    pdot_cid_to_string(&res.cid, cid, sizeof cid);
    printf("stored   %s in block #%u\n", cid, res.tx.block);

    /* 4. Fetch by CID. The bytes are checked against the CID's hash. */
    if ((rc = pdot_bulletin_fetch(c, &res.cid, &data, &len))) {
        fprintf(stderr, "fetch: %s (%s)\n", pdot_strerror(rc), pdot_last_error(c));
        goto out;
    }
    printf("fetched  \"%.*s\"\n", (int)len, (const char *)data);
    pdot_free(data);

out:
    pdot_signer_wipe(&signer);
    pdot_client_free(c);
    return rc ? 1 : 0;
}
