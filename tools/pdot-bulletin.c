/*
 * pdot-bulletin: store and retrieve data on the Polkadot Bulletin Chain.
 * SPDX-License-Identifier: Apache-2.0
 */
/* nanosleep is POSIX; strict -std=c99 hides it on glibc without this. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "pdot.h"

/* Larger than the library limit, so the library reports oversize files. */
#define MAX_FILE (2 * PDOT_FILE_SIZE_MAX)

static const char usage[] =
    "usage: pdot-bulletin [options] <command> [args]\n"
    "\n"
    "commands:\n"
    "  address                 print the signer's account\n"
    "  authorized [ADDRESS]    check storage authorization (default: signer)\n"
    "  store FILE              store FILE, wait for finalization, print CID;\n"
    "                          over 2 MiB it is chunked with a DAG-PB manifest\n"
    "  renew CID               renew stored data, every chunk of a chunked file\n"
    "  fetch CID [OUT]         fetch and verify data (to OUT or stdout)\n"
    "  roundtrip FILE          authorized + store + fetch + verify + renew\n"
    "  verify CID              prove CID is stored and retained (needs --relay ...)\n"
    "  checkpoint [--at N]     create --checkpoint FILE from --relay, trusting it once\n"
    "  authorize ADDRESS TXS BYTES\n"
    "                          grant storage (signer must be a registered authorizer)\n"
    "  metadata-hash           print the RFC-0078 metadata hash transactions are signed with\n"
    "\n"
    "chain state at the newest final block (Asset Hub, or any chain with these pallets):\n"
    "  balance ADDRESS         native token balance\n"
    "  asset ID [ADDRESS]      a fungible asset, and ADDRESS's holding of it\n"
    "  nft COLLECTION ITEM     an NFT's owner and metadata\n"
    "  nft-attr COLLECTION ITEM|- KEY\n"
    "                          an attribute set by the collection owner (- for the collection)\n"
    "  nfts ADDRESS COLLECTION [MAX]\n"
    "                          items of a collection ADDRESS owns (default MAX 100)\n"
    "  watch head | balance ADDRESS | asset ID ADDRESS | nft COLLECTION ITEM\n"
    "                          report the value at each change (through pdot_async)\n"
    "  proxies ADDRESS         accounts that may act for ADDRESS\n"
    "\n"
    "transactions (Asset Hub, or any chain with these pallets; --fee only estimates):\n"
    "  transfer ADDRESS AMOUNT native token, AMOUNT in whole tokens (\"1.5\")\n"
    "  transfer-all ADDRESS    all of the native token, closing the signer's account\n"
    "  transfer-asset ID ADDRESS AMOUNT\n"
    "                          a pallet-assets asset, AMOUNT in its decimals\n"
    "  transfer-nft COLLECTION ITEM ADDRESS\n"
    "  call PALLET CALL [JSON] any call, arguments as a JSON array or object\n"
    "  session-key             print a new random sr25519 seed and its address\n"
    "  proxy-add ADDRESS TYPE [FUND]\n"
    "                          let ADDRESS act for the signer as TYPE (Any, NonTransfer,\n"
    "                          Assets, ...), sending it FUND tokens for fees in the same tx\n"
    "  proxy-remove ADDRESS TYPE\n"
    "\n"
    "options:\n"
    "  --rpc URL               node RPC (default http://127.0.0.1:9944)\n"
    "  --gateway URL           IPFS HTTP gateway (default: bitswap_v1_get on the node)\n"
    "  --scheme sr25519|ecdsa  signature scheme (default sr25519)\n"
    "  --seed-file PATH        file holding the 32-byte hex seed\n"
    "  --seed HEX              hex seed; PDOT_SEED in the environment also works\n"
    "  --prefix N              SS58 prefix for printed addresses (default 42)\n"
    "  --timeout MS            per-request timeout (default 60000)\n"
    "  --chunk-size BYTES      always chunk, with this chunk size (max 2 MiB)\n"
    "  --metadata-hash MODE    on (default): sign with CheckMetadataHash, so a node serving\n"
    "                          false metadata gets the transaction rejected; off; or\n"
    "                          SYMBOL:DECIMALS when the node's token properties differ\n"
    "                          from the runtime build (local dev chains)\n"
    "\n"
    "verification (results are proven, not taken from the RPC node):\n"
    "  --relay URL             relay-chain RPC (Westend, Paseo, Polkadot)\n"
    "  --para-id N             Bulletin parachain id: Westend 2487, Paseo 1501, Polkadot 1010\n"
    "  --checkpoint FILE       trusted GRANDPA checkpoint; updated after each run\n"
    "                          (Asset Hub is --para-id 1000 under every relay chain)\n"
    "  --count N               watch: stop after N reports (default: run until killed)\n"
    "\n"
    "transaction options:\n"
    "  --tip AMOUNT            tip in whole tokens, for priority\n"
    "  --as ADDRESS            sign as a proxy of ADDRESS: the call runs as ADDRESS\n"
    "  --fee                   print what the transaction would pay; submit nothing\n"
    "  --async                 submit through pdot_async, polling as a game loop would\n";

struct opts {
    const char *rpc, *gateway, *scheme, *seed_hex, *seed_file;
    unsigned prefix;
    long timeout;
    size_t chunk_size;   /* 0: chunk only above PDOT_CHUNK_SIZE_MAX */
    const char *relay, *checkpoint;
    unsigned para_id;
    const char *mdhash;  /* "on", "off" or SYMBOL:DECIMALS */
    unsigned count;      /* watch: stop after this many reports; 0: never */
    const char *tip, *as;
    int fee_only, async;
};

static int die(const char *what, int rc, const pdot_client *c)
{
    fprintf(stderr, "error: %s: %s", what, pdot_strerror(rc));
    if (c && strcmp(pdot_last_error(c), "no error") != 0)
        fprintf(stderr, " (%s)", pdot_last_error(c));
    fputc('\n', stderr);
    return 1;
}

static void print_hex(const char *label, const uint8_t *p, size_t n)
{
    size_t i;

    printf("%s0x", label);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    putchar('\n');
}

static int parse_hex32(const char *s, uint8_t out[32])
{
    size_t i, n;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    n = strspn(s, "0123456789abcdefABCDEF");
    if (n != 64 || (s[n] != '\0' && s[n] != '\n' && s[n] != '\r'))
        return -1;
    for (i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 65536, n = 0, r;
    uint8_t *buf;

    if (!f) {
        perror(path);
        return -1;
    }
    buf = malloc(cap);
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            uint8_t *nb;
            if (cap >= MAX_FILE) {
                fprintf(stderr, "error: %s is larger than %u bytes\n", path, MAX_FILE);
                free(buf);
                fclose(f);
                return -1;
            }
            nb = realloc(buf, cap * 2);
            if (!nb)
                free(buf);
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (!buf)
        return -1;
    *data = buf;
    *len = n;
    return 0;
}

static int load_signer(const struct opts *o, pdot_signer *s)
{
    char line[256] = "";
    const char *hex = o->seed_hex ? o->seed_hex : getenv("PDOT_SEED");
    uint8_t seed[32];
    int rc;

    if (o->seed_file) {
        FILE *f = fopen(o->seed_file, "r");
        if (!f || !fgets(line, sizeof line, f)) {
            perror(o->seed_file);
            if (f)
                fclose(f);
            return -1;
        }
        fclose(f);
        hex = line;
    }
    if (!hex) {
        fprintf(stderr, "error: no seed; use --seed-file, --seed or PDOT_SEED\n");
        return -1;
    }
    if (parse_hex32(hex, seed)) {
        fprintf(stderr, "error: seed must be 32 bytes of hex\n");
        return -1;
    }
    if (strcmp(o->scheme, "sr25519") == 0)
        rc = pdot_signer_sr25519_from_seed(s, seed);
    else if (strcmp(o->scheme, "ecdsa") == 0)
        rc = pdot_signer_ecdsa_from_seed(s, seed);
    else {
        fprintf(stderr, "error: unknown scheme %s\n", o->scheme);
        rc = -1;
    }
    memset(seed, 0, sizeof seed);
    memset(line, 0, sizeof line);
    return rc ? (rc == -1 ? -1 : die("loading key", rc, NULL)) : 0;
}

static void print_account(const char *label, const uint8_t id[32], unsigned prefix)
{
    char addr[PDOT_SS58_MAX_LEN];

    pdot_ss58_encode(id, (uint16_t)prefix, addr, sizeof addr);
    printf("%s%s\n", label, addr);
}

static void print_tx(const pdot_tx_result *tx)
{
    print_hex("  tx hash:    ", tx->tx_hash, 32);
    printf("  block:      #%u\n", tx->block);
    print_hex("  block hash: ", tx->block_hash, 32);
    printf("  index:      %u\n", tx->index);
    if (tx->dispatch == PDOT_DISPATCH_OK)
        printf("  dispatch:   ok\n");
    else if (tx->dispatch == PDOT_DISPATCH_FAILED)
        printf("  dispatch:   FAILED: %s\n", tx->error);
    else
        printf("  dispatch:   unknown (block state unavailable)\n");
    if (tx->verified)
        printf("  verified:   yes, proven from relay-chain finality\n");
    if (tx->has_fee) {
        char text[64];
        pdot_u128_format(tx->fee, 0, text, sizeof text);
        printf("  fee:        %s (smallest units, tip included)\n", text);
    }
}

static int cmd_authorized(pdot_client *c, const uint8_t id[32], unsigned prefix, int *ok)
{
    pdot_authorization a;
    int rc = pdot_bulletin_authorization(c, id, &a);

    if (rc)
        return die("authorization check", rc, c);
    *ok = a.exists && a.now < a.expiration;
    print_account(*ok ? "authorized:     " : "NOT authorized: ", id, prefix);
    if (a.exists) {
        printf("  transactions: %u of %u used\n", a.transactions, a.transactions_allowance);
        printf("  bytes:        %llu of %llu used\n", (unsigned long long)a.bytes,
               (unsigned long long)a.bytes_allowance);
        printf("  expires:      block #%u (now #%u)%s\n", a.expiration, a.now,
               a.now >= a.expiration ? ", EXPIRED" : "");
    }
    if (a.verified)
        printf("  verified:     yes, proven at final Bulletin #%u\n", a.now);
    return 0;
}

static void on_progress(void *ud, size_t index, size_t total, const pdot_cid *cid,
                        const pdot_tx_result *tx, int rc)
{
    char str[128];

    pdot_cid_to_string(cid, str, sizeof str);
    fprintf(stderr, "  [%zu/%zu] %s %s %s block #%u\n", index + 1, total, (const char *)ud,
            pdot_cid_codec(cid) == PDOT_CODEC_DAG_PB ? "manifest" : total > 1 ? "chunk   " : "data    ",
            str, tx->block);
    if (rc)
        fprintf(stderr, "  [%zu/%zu] failed: %s\n", index + 1, total, pdot_strerror(rc));
}

/* Stores a file whole, or chunked when it is large or chunk_size is set. */
static int cmd_store(pdot_client *c, const pdot_signer *s, const char *path,
                     size_t chunk_size, pdot_cid *root)
{
    char cid[128];
    uint8_t *data;
    size_t len;
    int rc;

    if (read_file(path, &data, &len))
        return 1;
    if (chunk_size || len > PDOT_CHUNK_SIZE_MAX) {
        pdot_chunked_result res;
        size_t cs = chunk_size ? chunk_size : PDOT_CHUNK_SIZE_DEFAULT;

        fprintf(stderr, "storing %zu bytes in %zu chunks of %zu, waiting for each...\n",
                len, (len + cs - 1) / cs, cs);
        rc = pdot_bulletin_store_chunked(c, s, data, len, cs, on_progress, "stored ", &res);
        free(data);
        if (rc)
            return die("chunked store", rc, c);
        pdot_cid_to_string(&res.root, cid, sizeof cid);
        printf("stored %zu bytes as %zu chunks + manifest\n  cid:        %s\n",
               len, res.nchunks, cid);
        printf("  manifest:   #%u/%u\n", res.manifest.tx.block, res.manifest.tx.index);
        if (res.manifest.tx.verified)
            printf("  verified:   yes, every chunk and the manifest proven stored\n");
        *root = res.root;
        return 0;
    } else {
        pdot_store_result res;

        fprintf(stderr, "storing %zu bytes, waiting for finalization...\n", len);
        rc = pdot_bulletin_store(c, s, data, len, &res);
        free(data);
        if (rc == PDOT_E_DISPATCH)
            print_tx(&res.tx);
        if (rc)
            return die("store", rc, c);
        pdot_cid_to_string(&res.cid, cid, sizeof cid);
        printf("stored %zu bytes\n  cid:        %s\n", len, cid);
        if (res.has_storage_index)
            printf("  stored as:  #%u/%u (TransactionRef::Position)\n", res.tx.block,
                   res.storage_index);
        print_tx(&res.tx);
        *root = res.cid;
        return 0;
    }
}

static int cmd_renew(pdot_client *c, const pdot_signer *s, const pdot_cid *cid)
{
    char str[128];
    int rc;

    pdot_cid_to_string(cid, str, sizeof str);
    fprintf(stderr, "renewing %s, waiting for finalization...\n", str);
    if ((rc = pdot_bulletin_renew_tree(c, s, cid, on_progress, "renewed")))
        return die("renew", rc, c);
    printf("renewed %s\n", str);
    return 0;
}

static int cmd_fetch(pdot_client *c, const pdot_cid *cid, const char *out, size_t *got)
{
    uint8_t *data;
    size_t len;
    int rc;

    if ((rc = pdot_bulletin_fetch(c, cid, &data, &len)))
        return die("fetch", rc, c);
    if (got)
        *got = len;
    if (out) {
        FILE *f = strcmp(out, "-") == 0 ? stdout : fopen(out, "wb");
        if (!f || fwrite(data, 1, len, f) != len) {
            perror(out);
            pdot_free(data);
            return 1;
        }
        if (f != stdout)
            fclose(f);
    }
    pdot_free(data);
    fprintf(stderr, "fetched %zu bytes, Blake2b-256 matches the CID\n", len);
    return 0;
}

static int cmd_verify(pdot_client *c, const pdot_cid *cid)
{
    pdot_storage_proof p;
    char str[128];
    int rc;

    pdot_cid_to_string(cid, str, sizeof str);
    if ((rc = pdot_bulletin_verify_cid(c, cid, &p)))
        return die("verify", rc, c);
    printf("stored:       %s\n", str);
    printf("  block:        #%u, entry %u\n", p.block, p.index);
    printf("  retained to:  #%u\n", p.expires);
    printf("  proven at:    Bulletin #%u, final by relay #%u\n", p.proven_at, p.relay_block);
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;

    if (f && fclose(f) != 0)
        ok = 0;
    if (!ok)
        perror(path);
    return ok ? 0 : 1;
}

/* Loads --checkpoint and turns verification on. */
static int setup_verifier(pdot_client *c, const struct opts *o)
{
    uint8_t *ck = NULL;
    size_t len = 0;
    int rc;

    if (!o->checkpoint || !o->para_id) {
        fprintf(stderr, "error: --relay needs --para-id and --checkpoint\n");
        return 2;
    }
    if (read_file(o->checkpoint, &ck, &len)) {
        fprintf(stderr, "hint: create one with: pdot-bulletin --relay %s --checkpoint %s checkpoint\n",
                o->relay, o->checkpoint);
        return 1;
    }
    rc = pdot_client_set_verifier(c, o->relay, o->para_id, ck, len);
    free(ck);
    return rc ? die("checkpoint", rc, c) : 0;
}

/*
 * Writes ck to --checkpoint if it differs from what is there, through a
 * temporary file so that a run killed mid-write keeps the old one.
 */
static void store_checkpoint(const struct opts *o, const uint8_t *ck, size_t len)
{
    uint8_t *old = NULL;
    size_t old_len = 0;
    char tmp[1024];

    if (!o->relay || !o->checkpoint)
        return;
    if (read_file(o->checkpoint, &old, &old_len) == 0 && old_len == len &&
        memcmp(old, ck, len) == 0) {
        free(old);
        return;
    }
    free(old);
    snprintf(tmp, sizeof tmp, "%s.tmp", o->checkpoint);
    if (write_file(tmp, ck, len) == 0) {
#if defined(_WIN32)
        remove(o->checkpoint);
#endif
        if (rename(tmp, o->checkpoint) != 0)
            perror(o->checkpoint);
    }
}

/* Saves the checkpoint back if verification advanced it. */
static void save_checkpoint(pdot_client *c, const struct opts *o)
{
    uint8_t *ck;
    size_t len;

    if (!o->relay || pdot_client_checkpoint(c, &ck, &len))
        return;
    store_checkpoint(o, ck, len);
    pdot_free(ck);
}

/* The same for a client handed to pdot_async: its jobs and watches combined. */
static void save_async_checkpoint(pdot_async *a, const struct opts *o)
{
    uint8_t *ck;
    size_t len;

    if (!o->relay || pdot_async_checkpoint(a, &ck, &len))
        return;
    store_checkpoint(o, ck, len);
    pdot_free(ck);
}

/* pdot_async_set_checkpoint_fn: saved as soon as a watch moves it forward. */
static void on_checkpoint(void *ud, const uint8_t *ck, size_t len)
{
    store_checkpoint(ud, ck, len);
    fprintf(stderr, "checkpoint advanced: saved\n");
}

/* --metadata-hash on | off | SYMBOL:DECIMALS */
static int setup_metadata_hash(pdot_client *c, const char *mode)
{
    char symbol[32];
    const char *colon = strchr(mode, ':');
    char *end;
    long dec;

    if (strcmp(mode, "off") == 0)
        return 0;
    if (strcmp(mode, "on") == 0)
        return pdot_client_set_metadata_hash(c, 1, NULL, -1) ? 2 : 0;
    if (colon && colon > mode && (size_t)(colon - mode) < sizeof symbol) {
        dec = strtol(colon + 1, &end, 10);
        if (colon[1] && *end == '\0' && dec >= 0 && dec <= 255) {
            memcpy(symbol, mode, (size_t)(colon - mode));
            symbol[colon - mode] = '\0';
            return pdot_client_set_metadata_hash(c, 1, symbol, (int)dec) ? 2 : 0;
        }
    }
    fprintf(stderr, "error: --metadata-hash takes on, off or SYMBOL:DECIMALS\n");
    return 2;
}

static int parse_u32(const char *s, uint32_t *v)
{
    char *end;
    unsigned long n = strtoul(s, &end, 10);

    if (*s == '\0' || *end != '\0' || n > 0xffffffffUL)
        return -1;
    *v = (uint32_t)n;
    return 0;
}

/* ---- Chain state ---------------------------------------------------------- */

static void print_at(const pdot_final_block *at)
{
    if (at->verified)
        printf("  at:         final #%u, proven from relay #%u\n", at->number, at->relay_block);
    else
        printf("  at:         final #%u (the RPC node's finalized head)\n", at->number);
}

static void print_amount(const char *label, pdot_u128 v, uint8_t decimals, const char *symbol)
{
    char text[96];

    if (pdot_u128_format(v, decimals, text, sizeof text))
        strcpy(text, "?");
    printf("%s%s %s\n", label, text, symbol);
}

static void print_balance(const pdot_balance *b, uint8_t decimals, const char *symbol)
{
    if (!b->exists) {
        printf("  no account\n");
    } else {
        print_amount("  free:       ", b->free, decimals, symbol);
        print_amount("  reserved:   ", b->reserved, decimals, symbol);
        print_amount("  frozen:     ", b->frozen, decimals, symbol);
        printf("  nonce:      %u\n", b->nonce);
    }
    print_at(&b->at);
}

static void print_nft(const pdot_nft_item *n, unsigned prefix)
{
    if (!n->exists) {
        printf("  no such item\n");
    } else {
        print_account("  owner:      ", n->owner, prefix);
        if (n->has_metadata)
            printf("  metadata:   %.*s\n", (int)n->metadata_len, (const char *)n->metadata);
    }
    print_at(&n->at);
}

static int cmd_balance(pdot_client *c, const uint8_t id[32], unsigned prefix)
{
    pdot_balance b;
    char symbol[32];
    uint8_t decimals;
    int rc;

    if ((rc = pdot_client_token(c, symbol, sizeof symbol, &decimals)) ||
        (rc = pdot_balance_get(c, id, &b)))
        return die("balance", rc, c);
    print_account("account:    ", id, prefix);
    print_balance(&b, decimals, symbol);
    return 0;
}

static int cmd_asset(pdot_client *c, uint32_t asset, const uint8_t *who, unsigned prefix)
{
    pdot_asset_info info;
    pdot_asset_balance bal;
    char supply[96];
    int rc;

    if ((rc = pdot_asset_info_get(c, asset, &info)))
        return die("asset", rc, c);
    printf("asset:      %u\n", asset);
    if (!info.exists) {
        printf("  no such asset\n");
        print_at(&info.at);
        return 1;
    }
    printf("  name:       %s\n  symbol:     %s\n  decimals:   %u\n", info.name, info.symbol,
           info.decimals);
    if (pdot_u128_format(info.supply, info.decimals, supply, sizeof supply))
        strcpy(supply, "?");
    printf("  supply:     %s %s\n  holders:    %u\n", supply, info.symbol, info.accounts);
    print_account("  owner:      ", info.owner, prefix);
    printf("  status:     %s%s\n", info.live ? "live" : "frozen or being destroyed",
           info.sufficient ? ", sufficient" : "");
    print_at(&info.at);
    if (!who)
        return 0;
    if ((rc = pdot_asset_balance_get(c, asset, who, &bal)))
        return die("asset balance", rc, c);
    print_account("holder:     ", who, prefix);
    if (bal.exists)
        print_amount(bal.frozen ? "  balance:    (frozen) " : "  balance:    ", bal.balance,
                     info.decimals, info.symbol);
    else
        printf("  holds none\n");
    print_at(&bal.at);
    return 0;
}

static int cmd_nft(pdot_client *c, uint32_t collection, uint32_t item, unsigned prefix)
{
    pdot_nft_item n;
    int rc;

    if ((rc = pdot_nft_item_get(c, collection, item, &n)))
        return die("nft", rc, c);
    printf("nft:        collection %u, item %u\n", collection, item);
    print_nft(&n, prefix);
    return n.exists ? 0 : 1;
}

static int cmd_nft_attr(pdot_client *c, uint32_t collection, const char *item, const char *key)
{
    pdot_nft_attribute a;
    uint32_t id = 0;
    int has_item = strcmp(item, "-") != 0, rc;

    if (has_item && parse_u32(item, &id)) {
        fprintf(stderr, "error: ITEM is a number or -\n");
        return 2;
    }
    if ((rc = pdot_nft_attribute_get(c, collection, has_item, id, PDOT_NFT_NS_COLLECTION_OWNER,
                                     NULL, (const uint8_t *)key, strlen(key), &a)))
        return die("nft attribute", rc, c);
    if (a.exists)
        printf("%s = %.*s\n", key, (int)a.len, (const char *)a.value);
    else
        printf("%s is not set\n", key);
    print_at(&a.at);
    return a.exists ? 0 : 1;
}

static int cmd_nfts(pdot_client *c, const uint8_t who[32], uint32_t collection, size_t max,
                    unsigned prefix)
{
    pdot_final_block at;
    uint32_t *items;
    size_t n, i;
    int rc;

    if ((rc = pdot_nft_owned(c, who, collection, max, &items, &n, &at)))
        return die("owned nfts", rc, c);
    print_account("owner:      ", who, prefix);
    printf("  collection %u: %zu item%s%s\n", collection, n, n == 1 ? "" : "s",
           n == max ? " (limit reached)" : "");
    for (i = 0; i < n; i++)
        printf("  item %u\n", items[i]);
    if (at.verified)
        printf("  each proven owned; the node could still leave items out\n");
    print_at(&at);
    pdot_free(items);
    return 0;
}

/* ---- watch: pdot_async ---------------------------------------------------- */

struct watch_ctx {
    unsigned seen, count;
    unsigned prefix;
    int      kind;  /* 0 head, 1 balance, 2 asset, 3 nft */
    uint8_t  decimals;
    char     symbol[32];
};

static void on_watch(void *ud, int rc, const char *error, const void *result)
{
    struct watch_ctx *w = ud;

    if (rc == PDOT_E_CANCELLED)
        return;
    if (rc) {
        fprintf(stderr, "watch: %s (retrying)\n", error);
        return;
    }
    w->seen++;
    switch (w->kind) {
    case 0: {
        const pdot_final_block *b = result;
        if (b->verified)
            printf("final #%u, proven from relay #%u\n", b->number, b->relay_block);
        else
            printf("final #%u\n", b->number);
        break;
    }
    case 1:
        printf("balance:\n");
        print_balance(result, w->decimals, w->symbol);
        break;
    case 2: {
        const pdot_asset_balance *b = result;
        printf("asset holding:\n");
        if (b->exists)
            print_amount("  balance:    ", b->balance, w->decimals, w->symbol);
        else
            printf("  holds none\n");
        print_at(&b->at);
        break;
    }
    case 3:
        printf("nft:\n");
        print_nft(result, w->prefix);
        break;
    }
    fflush(stdout);
}

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Picks the watch from argv; 0, or an exit code. */
static int parse_watch(pdot_client *c, int argc, char **argv, int i, struct watch_ctx *w,
                       uint8_t who[32], uint32_t *x, uint32_t *y)
{
    int rc;

    if (i < argc && strcmp(argv[i], "head") == 0) {
        w->kind = 0;
    } else if (i + 1 < argc && strcmp(argv[i], "balance") == 0) {
        w->kind = 1;
        if (pdot_ss58_decode(argv[i + 1], who, NULL))
            return die("parsing address", PDOT_E_INVALID, NULL);
        if ((rc = pdot_client_token(c, w->symbol, sizeof w->symbol, &w->decimals)))
            return die("token", rc, c);
    } else if (i + 2 < argc && strcmp(argv[i], "asset") == 0) {
        pdot_asset_info info;
        w->kind = 2;
        if (parse_u32(argv[i + 1], x) || pdot_ss58_decode(argv[i + 2], who, NULL))
            return die("parsing ID ADDRESS", PDOT_E_INVALID, NULL);
        if ((rc = pdot_asset_info_get(c, *x, &info)))
            return die("asset", rc, c);
        w->decimals = info.decimals;
        strcpy(w->symbol, info.symbol);
    } else if (i + 2 < argc && strcmp(argv[i], "nft") == 0) {
        w->kind = 3;
        if (parse_u32(argv[i + 1], x) || parse_u32(argv[i + 2], y))
            return die("parsing COLLECTION ITEM", PDOT_E_INVALID, NULL);
    } else {
        fputs(usage, stderr);
        return 2;
    }
    return 0;
}

/* Watches until --count reports. Takes over the client. */
static int cmd_watch(pdot_client *c, int argc, char **argv, int i, const struct opts *o)
{
    struct watch_ctx w;
    pdot_async *a;
    uint8_t who[32];
    uint32_t x = 0, y = 0;
    uint64_t id = 0;
    int rc;

    memset(&w, 0, sizeof w);
    w.count = o->count;
    w.prefix = o->prefix;
    if ((rc = parse_watch(c, argc, argv, i, &w, who, &x, &y)) != 0 ||
        !(a = pdot_async_new(c))) {
        pdot_client_free(c);
        return rc ? rc : die("async", PDOT_E_NOMEM, NULL);
    }
    if (o->relay)
        pdot_async_set_checkpoint_fn(a, on_checkpoint, (void *)o);
    switch (w.kind) {
    case 0: id = pdot_async_watch_head(a, on_watch, &w); break;
    case 1: id = pdot_async_watch_balance(a, who, on_watch, &w); break;
    case 2: id = pdot_async_watch_asset_balance(a, x, who, on_watch, &w); break;
    case 3: id = pdot_async_watch_nft_item(a, x, y, on_watch, &w); break;
    }
    /* A stand-in for a game loop: poll each frame, render, sleep. */
    while (id && (!w.count || w.seen < w.count)) {
        pdot_async_poll(a);
        sleep_ms(16);
    }
    save_async_checkpoint(a, o);
    pdot_async_free(a);
    return id ? 0 : die("watch", PDOT_E_NOMEM, NULL);
}

/* ---- Transactions --------------------------------------------------------- */

struct tx_wait {
    int            done, rc;
    char           err[256];
    pdot_tx_result tx;
};

static void on_tx(void *ud, int rc, const char *error, const void *result)
{
    struct tx_wait *w = ud;

    w->done = 1;
    w->rc = rc;
    snprintf(w->err, sizeof w->err, "%s", error ? error : "");
    if (result)
        w->tx = *(const pdot_tx_result *)result;
}

/* Through pdot_async: submit, then poll once per "frame" until the callback ran. */
static int submit_async(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                        const pdot_tx_options *to, const struct opts *o, pdot_tx_result *tx,
                        char *err, size_t cap)
{
    struct tx_wait w;
    pdot_async *a = pdot_async_new(c);
    unsigned frames = 0;

    memset(&w, 0, sizeof w);
    if (!a || !pdot_async_submit(a, s, call, len, to, on_tx, &w)) {
        pdot_async_free(a);
        snprintf(err, cap, "cannot queue the transaction");
        return PDOT_E_NOMEM;
    }
    while (!w.done) {
        pdot_async_poll(a);
        if (!w.done) {
            sleep_ms(16);
            frames++;
        }
    }
    save_async_checkpoint(a, o);
    pdot_async_free(a);
    fprintf(stderr, "async: done after %u frames\n", frames);
    *tx = w.tx;
    snprintf(err, cap, "%s", w.err);
    return w.rc;
}

/*
 * Estimates or submits call. With async the client is handed to pdot_async
 * and freed; *client_gone tells the caller.
 */
static int run_tx(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                  const struct opts *o, int *client_gone)
{
    pdot_tx_options to;
    uint8_t real[32];
    char symbol[32], text[64], err[256];
    uint8_t decimals;
    pdot_tx_result tx;
    int rc;

    memset(&to, 0, sizeof to);
    if ((rc = pdot_client_token(c, symbol, sizeof symbol, &decimals)))
        return die("token", rc, c);
    if (o->tip && pdot_u128_parse(o->tip, decimals, &to.tip)) {
        fprintf(stderr, "error: --tip %s is not an amount of %s\n", o->tip, symbol);
        return 2;
    }
    if (o->as) {
        if (pdot_ss58_decode(o->as, real, NULL)) {
            fprintf(stderr, "error: --as needs an SS58 address\n");
            return 2;
        }
        to.proxy_for = real;
        print_account("acting for: ", real, o->prefix);
    }
    if (o->fee_only) {
        pdot_fee fee;
        if ((rc = pdot_tx_fee(c, s, call, len, &to, &fee)))
            return die("fee", rc, c);
        pdot_u128_format(fee.partial_fee, decimals, text, sizeof text);
        printf("fee:        %s %s (+ tip)\n", text, symbol);
        pdot_u128_format(fee.partial_fee, 0, text, sizeof text);
        printf("  units:      %s\n", text);
        printf("  weight:     ref_time %llu, proof_size %llu, class %u\n",
               (unsigned long long)fee.ref_time, (unsigned long long)fee.proof_size,
               fee.dispatch_class);
        return 0;
    }
    if (o->async) {
        *client_gone = 1;
        rc = submit_async(c, s, call, len, &to, o, &tx, err, sizeof err);
    } else {
        rc = pdot_tx_submit(c, s, call, len, &to, &tx);
        snprintf(err, sizeof err, "%s", rc ? pdot_last_error(c) : "");
    }
    if (rc == 0 || rc == PDOT_E_DISPATCH)
        print_tx(&tx);
    if (rc) {
        fprintf(stderr, "error: transaction: %s (%s)\n", pdot_strerror(rc), err);
        return 1;
    }
    if (tx.has_fee) {
        pdot_u128_format(tx.fee, decimals, text, sizeof text);
        printf("paid:       %s %s\n", text, symbol);
    }
    return 0;
}

/* Builds the call for a transaction command; 2 on bad arguments. */
static int tx_call(pdot_client *c, const char *cmd, int argc, char **argv, int i,
                   uint8_t **call, size_t *len)
{
    pdot_transfer t;
    char symbol[32];
    uint8_t decimals;
    int rc;

    memset(&t, 0, sizeof t);
    if (strcmp(cmd, "call") == 0 && i + 1 < argc) {
        if ((rc = pdot_call_from_json(c, argv[i], argv[i + 1], i + 2 < argc ? argv[i + 2] : NULL,
                                      call, len)))
            return die("call", rc, c);
        return 0;
    }
    if (strcmp(cmd, "transfer") == 0 && i + 1 < argc) {
        t.kind = PDOT_TRANSFER_NATIVE;
        if ((rc = pdot_client_token(c, symbol, sizeof symbol, &decimals)))
            return die("token", rc, c);
        if (pdot_ss58_decode(argv[i], t.dest, NULL) ||
            pdot_u128_parse(argv[i + 1], decimals, &t.amount)) {
            fprintf(stderr, "error: expected ADDRESS AMOUNT (in %s, up to %u decimals)\n",
                    symbol, decimals);
            return 2;
        }
    } else if (strcmp(cmd, "transfer-all") == 0 && i < argc) {
        t.kind = PDOT_TRANSFER_ALL;
        if (pdot_ss58_decode(argv[i], t.dest, NULL)) {
            fprintf(stderr, "error: expected ADDRESS\n");
            return 2;
        }
    } else if (strcmp(cmd, "transfer-asset") == 0 && i + 2 < argc) {
        pdot_asset_info info;
        t.kind = PDOT_TRANSFER_ASSET;
        if (parse_u32(argv[i], &t.id) || pdot_ss58_decode(argv[i + 1], t.dest, NULL)) {
            fprintf(stderr, "error: expected ID ADDRESS AMOUNT\n");
            return 2;
        }
        if ((rc = pdot_asset_info_get(c, t.id, &info)))
            return die("asset", rc, c);
        if (!info.exists) {
            fprintf(stderr, "error: asset %u does not exist\n", t.id);
            return 1;
        }
        if (pdot_u128_parse(argv[i + 2], info.decimals, &t.amount)) {
            fprintf(stderr, "error: AMOUNT must have up to %u decimals\n", info.decimals);
            return 2;
        }
    } else if (strcmp(cmd, "transfer-nft") == 0 && i + 2 < argc) {
        t.kind = PDOT_TRANSFER_NFT;
        if (parse_u32(argv[i], &t.id) || parse_u32(argv[i + 1], &t.item) ||
            pdot_ss58_decode(argv[i + 2], t.dest, NULL)) {
            fprintf(stderr, "error: expected COLLECTION ITEM ADDRESS\n");
            return 2;
        }
    } else {
        return -1;  /* not a transaction command */
    }
    if ((rc = pdot_transfer_call(c, &t, call, len)))
        return die(cmd, rc, c);
    return 0;
}

static int cmd_proxies(pdot_client *c, const uint8_t who[32], unsigned prefix)
{
    pdot_proxies p;
    char text[64];
    size_t i;
    int rc;

    if ((rc = pdot_proxies_get(c, who, &p)))
        return die("proxies", rc, c);
    print_account("account:    ", who, prefix);
    if (!p.n)
        printf("  no proxies\n");
    for (i = 0; i < p.n; i++) {
        char addr[PDOT_SS58_MAX_LEN];
        pdot_ss58_encode(p.list[i].delegate, (uint16_t)prefix, addr, sizeof addr);
        printf("  proxy:      %s %s delay %u\n", addr, p.list[i].type, p.list[i].delay);
    }
    if (p.n) {
        pdot_u128_format(p.deposit, 0, text, sizeof text);
        printf("  deposit:    %s (smallest units)\n", text);
    }
    print_at(&p.at);
    return 0;
}

static int cmd_proxy(pdot_client *c, const pdot_signer *s, int add, int argc, char **argv,
                     int i, unsigned prefix)
{
    uint8_t who[32], decimals;
    pdot_u128 fund = { 0, 0 };
    char symbol[32];
    pdot_tx_result tx;
    int rc;

    if ((rc = pdot_client_token(c, symbol, sizeof symbol, &decimals)))
        return die("token", rc, c);
    if (pdot_ss58_decode(argv[i], who, NULL) ||
        (add && i + 2 < argc && pdot_u128_parse(argv[i + 2], decimals, &fund))) {
        fprintf(stderr, "error: expected ADDRESS TYPE%s\n", add ? " [FUND]" : "");
        return 2;
    }
    rc = add ? pdot_proxy_add(c, s, who, argv[i + 1], fund, &tx)
             : pdot_proxy_remove(c, s, who, argv[i + 1], &tx);
    if (rc == 0 || rc == PDOT_E_DISPATCH)
        print_tx(&tx);
    if (rc)
        return die(add ? "proxy-add" : "proxy-remove", rc, c);
    print_account(add ? "proxy added:   " : "proxy removed: ", who, prefix);
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o = { "http://127.0.0.1:9944", NULL, "sr25519", NULL, NULL, 42, 0, 0,
                      NULL, NULL, 0, "on", 0, NULL, NULL, 0, 0 };
    pdot_client *c;
    pdot_signer s;
    const char *cmd;
    uint8_t *call = NULL;
    size_t call_len = 0;
    int i, rc = 1, ok;

    for (i = 1; i < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(k, "--help") == 0) {
            fputs(usage, stdout);
            return 0;
        }
        if (strcmp(k, "--fee") == 0 || strcmp(k, "--async") == 0) {  /* flags */
            *(k[2] == 'f' ? &o.fee_only : &o.async) = 1;
            i--;
            continue;
        }
        if (!v) {
            fprintf(stderr, "error: %s needs a value\n", k);
            return 2;
        }
        if (strcmp(k, "--rpc") == 0) o.rpc = v;
        else if (strcmp(k, "--gateway") == 0) o.gateway = v;
        else if (strcmp(k, "--scheme") == 0) o.scheme = v;
        else if (strcmp(k, "--seed") == 0) o.seed_hex = v;
        else if (strcmp(k, "--seed-file") == 0) o.seed_file = v;
        else if (strcmp(k, "--prefix") == 0) o.prefix = (unsigned)strtoul(v, NULL, 10);
        else if (strcmp(k, "--timeout") == 0) o.timeout = strtol(v, NULL, 10);
        else if (strcmp(k, "--chunk-size") == 0) o.chunk_size = strtoul(v, NULL, 10);
        else if (strcmp(k, "--relay") == 0) o.relay = v;
        else if (strcmp(k, "--para-id") == 0) o.para_id = (unsigned)strtoul(v, NULL, 10);
        else if (strcmp(k, "--checkpoint") == 0) o.checkpoint = v;
        else if (strcmp(k, "--metadata-hash") == 0) o.mdhash = v;
        else if (strcmp(k, "--count") == 0) o.count = (unsigned)strtoul(v, NULL, 10);
        else if (strcmp(k, "--tip") == 0) o.tip = v;
        else if (strcmp(k, "--as") == 0) o.as = v;
        else {
            fprintf(stderr, "error: unknown option %s\n%s", k, usage);
            return 2;
        }
    }
    if (i >= argc) {
        fputs(usage, stderr);
        return 2;
    }
    cmd = argv[i++];

    if (strcmp(cmd, "checkpoint") == 0) {
        uint8_t *ck;
        size_t len;
        if (!o.relay || !o.checkpoint) {
            fprintf(stderr, "error: checkpoint needs --relay and --checkpoint FILE\n");
            return 2;
        }
        uint32_t at = 0;
        if (i + 1 < argc && strcmp(argv[i], "--at") == 0 && parse_u32(argv[i + 1], &at)) {
            fprintf(stderr, "error: --at needs a relay block number\n");
            return 2;
        }
        fprintf(stderr, "trusting %s for the initial GRANDPA authority set\n", o.relay);
        if ((rc = pdot_checkpoint_fetch_at(o.relay, at, &ck, &len)))
            return die("checkpoint", rc, NULL);
        rc = write_file(o.checkpoint, ck, len);
        pdot_free(ck);
        if (!rc)
            printf("checkpoint written to %s (%zu bytes)\n", o.checkpoint, len);
        return rc;
    }

    if (strcmp(cmd, "session-key") == 0) {
        uint8_t seed[32];
        if ((rc = pdot_signer_sr25519_random(&s, seed)))
            return die("session key", rc, NULL);
        print_hex("seed:       ", seed, 32);
        print_account("address:    ", s.account_id, o.prefix);
        memset(seed, 0, sizeof seed);
        pdot_signer_wipe(&s);
        return 0;
    }

    if (!(c = pdot_client_new(o.rpc, o.gateway))) {
        fprintf(stderr, "error: cannot create client for %s\n", o.rpc);
        return 1;
    }
    pdot_client_set_timeout(c, o.timeout);
    if ((rc = setup_metadata_hash(c, o.mdhash)) != 0) {
        pdot_client_free(c);
        return rc;
    }
    if (o.relay && (rc = setup_verifier(c, &o)) != 0) {
        pdot_client_free(c);
        return rc;
    }

    if (strcmp(cmd, "metadata-hash") == 0) {
        uint8_t h[32];
        if ((rc = pdot_client_metadata_hash(c, h)))
            rc = die("metadata hash", rc, c);
        else
            print_hex("metadata hash: ", h, 32);
    } else if (strcmp(cmd, "verify") == 0 && i < argc) {
        pdot_cid cid;
        if (!o.relay) {
            fprintf(stderr, "error: verify needs --relay, --para-id and --checkpoint\n");
            rc = 2;
        } else if ((rc = pdot_cid_from_string(&cid, argv[i]))) {
            rc = die("parsing CID", rc, NULL);
        } else {
            rc = cmd_verify(c, &cid);
        }
    } else if (strcmp(cmd, "fetch") == 0 && i < argc) {
        pdot_cid cid;
        if ((rc = pdot_cid_from_string(&cid, argv[i])))
            rc = die("parsing CID", rc, NULL);
        else
            rc = cmd_fetch(c, &cid, i + 1 < argc ? argv[i + 1] : "-", NULL);
    } else if (strcmp(cmd, "balance") == 0 && i < argc) {
        uint8_t id[32];
        if ((rc = pdot_ss58_decode(argv[i], id, NULL)))
            rc = die("parsing address", rc, NULL);
        else
            rc = cmd_balance(c, id, o.prefix);
    } else if (strcmp(cmd, "asset") == 0 && i < argc) {
        uint8_t id[32];
        uint32_t asset;
        if (parse_u32(argv[i], &asset) ||
            (i + 1 < argc && pdot_ss58_decode(argv[i + 1], id, NULL))) {
            fprintf(stderr, "error: expected ID [ADDRESS]\n");
            rc = 2;
        } else {
            rc = cmd_asset(c, asset, i + 1 < argc ? id : NULL, o.prefix);
        }
    } else if (strcmp(cmd, "nft") == 0 && i + 1 < argc) {
        uint32_t coll, item;
        if (parse_u32(argv[i], &coll) || parse_u32(argv[i + 1], &item)) {
            fprintf(stderr, "error: expected COLLECTION ITEM\n");
            rc = 2;
        } else {
            rc = cmd_nft(c, coll, item, o.prefix);
        }
    } else if (strcmp(cmd, "nft-attr") == 0 && i + 2 < argc) {
        uint32_t coll;
        if (parse_u32(argv[i], &coll)) {
            fprintf(stderr, "error: expected COLLECTION ITEM|- KEY\n");
            rc = 2;
        } else {
            rc = cmd_nft_attr(c, coll, argv[i + 1], argv[i + 2]);
        }
    } else if (strcmp(cmd, "nfts") == 0 && i + 1 < argc) {
        uint8_t id[32];
        uint32_t coll, max = 100;
        if (pdot_ss58_decode(argv[i], id, NULL) || parse_u32(argv[i + 1], &coll) ||
            (i + 2 < argc && parse_u32(argv[i + 2], &max))) {
            fprintf(stderr, "error: expected ADDRESS COLLECTION [MAX]\n");
            rc = 2;
        } else {
            rc = cmd_nfts(c, id, coll, max, o.prefix);
        }
    } else if (strcmp(cmd, "watch") == 0) {
        rc = cmd_watch(c, argc, argv, i, &o);
        return rc;  /* pdot_async_free freed the client */
    } else if (strcmp(cmd, "proxies") == 0 && i < argc) {
        uint8_t id[32];
        if ((rc = pdot_ss58_decode(argv[i], id, NULL)))
            rc = die("parsing address", rc, NULL);
        else
            rc = cmd_proxies(c, id, o.prefix);
    } else if (strcmp(cmd, "authorized") == 0 && i < argc) {
        uint8_t id[32];
        if ((rc = pdot_ss58_decode(argv[i], id, NULL)))
            rc = die("parsing address", rc, NULL);
        else
            rc = cmd_authorized(c, id, o.prefix, &ok) || !ok;
    } else if (load_signer(&o, &s) != 0) {
        rc = 1;
    } else {
        if (strcmp(cmd, "address") == 0) {
            printf("scheme:     %s\n", o.scheme);
            print_account("address:    ", s.account_id, o.prefix);
            print_hex("account id: ", s.account_id, 32);
            print_hex("public key: ", s.public_key, s.public_key_len);
            rc = 0;
        } else if (strcmp(cmd, "authorized") == 0) {
            rc = cmd_authorized(c, s.account_id, o.prefix, &ok) || !ok;
        } else if (strcmp(cmd, "store") == 0 && i < argc) {
            pdot_cid root;
            rc = cmd_store(c, &s, argv[i], o.chunk_size, &root);
        } else if (strcmp(cmd, "renew") == 0 && i < argc) {
            pdot_cid cid;
            if ((rc = pdot_cid_from_string(&cid, argv[i])))
                rc = die("parsing CID", rc, NULL);
            else
                rc = cmd_renew(c, &s, &cid);
        } else if (strcmp(cmd, "authorize") == 0 && i + 2 < argc) {
            uint8_t who[32];
            uint32_t txs;
            unsigned long long bytes = strtoull(argv[i + 2], NULL, 10);
            pdot_tx_result tx;
            if (pdot_ss58_decode(argv[i], who, NULL) || parse_u32(argv[i + 1], &txs)) {
                fprintf(stderr, "error: expected ADDRESS TXS BYTES\n");
                rc = 2;
            } else if ((rc = pdot_bulletin_authorize_account(c, &s, who, txs, bytes, &tx))) {
                if (rc == PDOT_E_DISPATCH)
                    print_tx(&tx);
                rc = die("authorize", rc, c);
            } else {
                print_account("authorized:     ", who, o.prefix);
                print_tx(&tx);
            }
        } else if (strcmp(cmd, "proxy-add") == 0 && i + 1 < argc) {
            rc = cmd_proxy(c, &s, 1, argc, argv, i, o.prefix);
        } else if (strcmp(cmd, "proxy-remove") == 0 && i + 1 < argc) {
            rc = cmd_proxy(c, &s, 0, argc, argv, i, o.prefix);
        } else if ((rc = tx_call(c, cmd, argc, argv, i, &call, &call_len)) != -1) {
            if (rc == 0) {
                int gone = 0;
                print_account("signer:     ", s.account_id, o.prefix);
                rc = run_tx(c, &s, call, call_len, &o, &gone);
                free(call);
                if (gone) {
                    pdot_signer_wipe(&s);
                    return rc;  /* pdot_async_free freed the client */
                }
            }
        } else if (strcmp(cmd, "roundtrip") == 0 && i < argc) {
            pdot_cid root;
            size_t got;
            print_account("account:        ", s.account_id, o.prefix);
            rc = cmd_authorized(c, s.account_id, o.prefix, &ok);
            if (!rc && !ok)
                rc = 1;
            if (!rc)
                rc = cmd_store(c, &s, argv[i], o.chunk_size, &root);
            if (!rc)
                rc = cmd_fetch(c, &root, NULL, &got);
            if (!rc)
                rc = cmd_renew(c, &s, &root);
            if (!rc)
                printf("roundtrip ok\n");
        } else {
            fputs(usage, stderr);
            rc = 2;
        }
        pdot_signer_wipe(&s);
    }
    save_checkpoint(c, &o);
    pdot_client_free(c);
    return rc;
}
