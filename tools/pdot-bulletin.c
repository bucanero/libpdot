/*
 * pdot-bulletin: store and retrieve data on the Polkadot Bulletin Chain.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdot.h"

#define MAX_FILE (8u * 1024 * 1024)

static const char usage[] =
    "usage: pdot-bulletin [options] <command> [args]\n"
    "\n"
    "commands:\n"
    "  address                 print the signer's account\n"
    "  authorized [ADDRESS]    check storage authorization (default: signer)\n"
    "  store FILE              store FILE, wait for finalization, print CID\n"
    "  renew CID               renew stored data (DataRenewal.force_renew)\n"
    "  fetch CID [OUT]         fetch and verify data (to OUT or stdout)\n"
    "  roundtrip FILE          authorized + store + fetch + verify + renew\n"
    "  authorize ADDRESS TXS BYTES\n"
    "                          grant storage (signer must be a registered authorizer)\n"
    "\n"
    "options:\n"
    "  --rpc URL               node RPC (default http://127.0.0.1:9944)\n"
    "  --gateway URL           IPFS HTTP gateway (default: bitswap_v1_get on the node)\n"
    "  --scheme sr25519|ecdsa  signature scheme (default sr25519)\n"
    "  --seed-file PATH        file holding the 32-byte hex seed\n"
    "  --seed HEX              hex seed; PDOT_SEED in the environment also works\n"
    "  --prefix N              SS58 prefix for printed addresses (default 42)\n"
    "  --timeout MS            per-request timeout (default 60000)\n";

struct opts {
    const char *rpc, *gateway, *scheme, *seed_hex, *seed_file;
    unsigned prefix;
    long timeout;
};

static int die(const char *what, int rc, const pdot_client *c)
{
    fprintf(stderr, "error: %s: %s", what, pdot_strerror(rc));
    if (c && rc != PDOT_E_INVALID)
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
}

static int cmd_authorized(pdot_client *c, const uint8_t id[32], unsigned prefix, int *ok)
{
    int rc = pdot_bulletin_is_authorized(c, id, ok);

    if (rc)
        return die("authorization check", rc, c);
    print_account(*ok ? "authorized:     " : "NOT authorized: ", id, prefix);
    return 0;
}

static int cmd_store(pdot_client *c, const pdot_signer *s, const char *path,
                     pdot_store_result *res)
{
    char cid[128];
    uint8_t *data;
    size_t len;
    int rc;

    if (read_file(path, &data, &len))
        return 1;
    fprintf(stderr, "storing %zu bytes, waiting for finalization...\n", len);
    rc = pdot_bulletin_store(c, s, data, len, res);
    free(data);
    if (rc)
        return die("store", rc, c);
    pdot_cid_to_string(&res->cid, cid, sizeof cid);
    printf("stored %zu bytes\n  cid:        %s\n", len, cid);
    print_tx(&res->tx);
    return 0;
}

static int cmd_renew(pdot_client *c, const pdot_signer *s, const pdot_cid *cid)
{
    pdot_tx_result tx;
    char str[128];
    int rc;

    pdot_cid_to_string(cid, str, sizeof str);
    fprintf(stderr, "renewing %s, waiting for finalization...\n", str);
    if ((rc = pdot_bulletin_renew(c, s, cid, &tx)))
        return die("renew", rc, c);
    printf("renewed %s\n", str);
    print_tx(&tx);
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

static int parse_u32(const char *s, uint32_t *v)
{
    char *end;
    unsigned long n = strtoul(s, &end, 10);

    if (*s == '\0' || *end != '\0' || n > 0xffffffffUL)
        return -1;
    *v = (uint32_t)n;
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o = { "http://127.0.0.1:9944", NULL, "sr25519", NULL, NULL, 42, 0 };
    pdot_client *c;
    pdot_signer s;
    const char *cmd;
    int i, rc = 1, ok;

    for (i = 1; i < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(k, "--help") == 0) {
            fputs(usage, stdout);
            return 0;
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

    if (!(c = pdot_client_new(o.rpc, o.gateway))) {
        fprintf(stderr, "error: cannot create client for %s\n", o.rpc);
        return 1;
    }
    pdot_client_set_timeout(c, o.timeout);

    if (strcmp(cmd, "fetch") == 0 && i < argc) {
        pdot_cid cid;
        if ((rc = pdot_cid_from_string(&cid, argv[i])))
            rc = die("parsing CID", rc, NULL);
        else
            rc = cmd_fetch(c, &cid, i + 1 < argc ? argv[i + 1] : "-", NULL);
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
            pdot_store_result res;
            rc = cmd_store(c, &s, argv[i], &res);
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
            if (pdot_ss58_decode(argv[i], who, NULL) || parse_u32(argv[i + 1], &txs) || !bytes) {
                fprintf(stderr, "error: expected ADDRESS TXS BYTES\n");
                rc = 2;
            } else if ((rc = pdot_bulletin_authorize_account(c, &s, who, txs, bytes, &tx))) {
                rc = die("authorize", rc, c);
            } else {
                print_account("authorized:     ", who, o.prefix);
                print_tx(&tx);
            }
        } else if (strcmp(cmd, "roundtrip") == 0 && i < argc) {
            pdot_store_result res;
            size_t got;
            print_account("account:        ", s.account_id, o.prefix);
            rc = cmd_authorized(c, s.account_id, o.prefix, &ok);
            if (!rc && !ok)
                rc = 1;
            if (!rc)
                rc = cmd_store(c, &s, argv[i], &res);
            if (!rc)
                rc = cmd_fetch(c, &res.cid, NULL, &got);
            if (!rc)
                rc = cmd_renew(c, &s, &res.cid);
            if (!rc)
                printf("roundtrip ok\n");
        } else {
            fputs(usage, stderr);
            rc = 2;
        }
        pdot_signer_wipe(&s);
    }
    pdot_client_free(c);
    return rc;
}
