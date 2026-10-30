/*
 * Upload a file of any size up to 64 MiB as 1 MiB chunks plus a DAG-PB
 * manifest, the layout the Bulletin SDKs use. The printed CID opens the whole
 * file in any IPFS gateway peered with the chain, and pdot_bulletin_fetch
 * reassembles and verifies it.
 *
 *   PDOT_SEED=<hex seed> ./chunked_upload FILE [rpc-url]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdot.h>

static void progress(void *ud, size_t index, size_t total, const pdot_cid *cid,
                     const pdot_tx_result *tx, int rc)
{
    char str[128];

    (void)ud;
    pdot_cid_to_string(cid, str, sizeof str);
    printf("  %zu/%zu %s %s (block #%u)\n", index + 1, total,
           rc ? "FAILED" : "stored", str, tx->block);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n)) != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n)
        *len = (size_t)n;
    else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *seed_hex = getenv("PDOT_SEED");
    pdot_chunked_result res;
    pdot_signer signer;
    pdot_client *c;
    uint8_t seed[32], *data, *back;
    size_t len, back_len, i;
    char cid[128];
    unsigned v;
    int rc;

    if (argc < 2 || !seed_hex || strlen(seed_hex) != 64) {
        fprintf(stderr, "usage: PDOT_SEED=<hex seed> %s FILE [rpc-url]\n", argv[0]);
        return 2;
    }
    for (i = 0; i < 32; i++) {
        if (sscanf(seed_hex + 2 * i, "%2x", &v) != 1)
            return 2;
        seed[i] = (uint8_t)v;
    }
    if (!(data = read_file(argv[1], &len))) {
        perror(argv[1]);
        return 1;
    }

    c = pdot_client_new(argc > 2 ? argv[2] : "http://127.0.0.1:9944", NULL);
    if (!c || pdot_signer_sr25519_from_seed(&signer, seed))
        return 1;

    printf("uploading %zu bytes\n", len);
    rc = pdot_bulletin_store_chunked(c, &signer, data, len, PDOT_CHUNK_SIZE_DEFAULT,
                                     progress, NULL, &res);
    if (rc) {
        fprintf(stderr, "upload stopped after %zu chunks: %s\n", res.nchunks,
                pdot_last_error(c));
        goto out;
    }
    pdot_cid_to_string(&res.root, cid, sizeof cid);
    printf("file CID  %s\n", cid);

    /* Fetching the root resolves the manifest and verifies every chunk. */
    if (!(rc = pdot_bulletin_fetch(c, &res.root, &back, &back_len))) {
        printf("verified  %zu bytes, %s\n", back_len,
               back_len == len && memcmp(back, data, len) == 0 ? "identical" : "DIFFERENT");
        pdot_free(back);
    }

out:
    free(data);
    pdot_signer_wipe(&signer);
    pdot_client_free(c);
    return rc ? 1 : 0;
}
