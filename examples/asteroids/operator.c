/*
 * asteroids-operator: the game's operator, and the only holder of its key.
 * Games record scores themselves (a remark from the player's account); the
 * operator checks each claimed score on chain before it touches the
 * leaderboard or mints a trophy.
 *
 *   GET  /health  {"ok": true, "operator": "5F...", "collection": 0, "proven": false}
 *   POST /claim   {"account": "5G...", "score": 4190, "name": "DAM", "block": 342,
 *                  "replay": "50444152..."}   (hex)
 *
 * A claim is honoured only if
 *   1. the replay (replay.h) re-runs, with the game's own sim.c, to a
 *      finished game scoring exactly `score`: the game was really played;
 *   2. Asset Hub block `block` holds a System.Remarked event from `account`
 *      over "pdot-asteroids score=N name=ABC replay=CID", CID being the
 *      replay's (pdot_remark_find; proven from relay-chain finality with
 *      --relay and --checkpoint): that player recorded that very game;
 *   3. the replay's seed is the hash of the block it names, one of the
 *      SEED_MAX_AGE blocks before the remark's (pdot_block_hash_get, proven
 *      too): the rocks were unknown until then, so the run was not prepared.
 * Then, if the score makes the top 10 or beats the record, the operator
 * stores the replay on Bulletin; a top-10 score goes on the merged board,
 * stored there too, and a new record mints a trophy. The mint, its metadata
 * and the "best" and "board" attributes go in one Utility.batch_all, all or
 * nothing. Each remark is honoured once (--claims file).
 *
 * Bulletin keeps data for a retention period. Every --renew-check seconds
 * the operator renews the board and every replay on it that is within
 * --renew-before blocks of expiring (default: a quarter of the period), so
 * what the board names stays fetchable.
 *
 * Claims are handled one at a time, in the order they arrive, so two games
 * can never overwrite each other's board update. The server is plain HTTP on
 * loopback for the sample; put TLS and rate limits in front of it before
 * exposing it.
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <pdot.h>

#include "board.h"
#include "replay.h"
#include "sim.h"

#define REQ_MAX  (2 * REPLAY_MAX_BYTES + 4096)  /* a replay as hex, and the rest */
#define MAX_CLAIMS 100000

/* //Bob: the dev chain's operator, as devnet.sh sets it up. */
static const char *DEV_OPERATOR = "398f0c28f98885e046333d4a41c19cee4c37368a9832c6502f6cfd182e2aef89";

typedef struct {
    pdot_client *ah, *bul;
    pdot_signer  op;
    char         op_addr[PDOT_SS58_MAX_LEN];
    uint32_t     collection;
    int          proven;
    const char  *claims_file;
    const char  *checkpoint;
    uint64_t    *claims;  /* block << 32 | extrinsic index of each honoured remark */
    size_t       nclaims;
    uint32_t     renew_before;  /* blocks; 0: a quarter of the retention period */
} operator_t;

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[operator] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- claims already honoured ----------------------------------------- */

static int claimed(const operator_t *o, uint64_t key)
{
    size_t i;
    for (i = 0; i < o->nclaims; i++)
        if (o->claims[i] == key)
            return 1;
    return 0;
}

static int add_claim(operator_t *o, uint64_t key)
{
    FILE *f;

    if (o->nclaims == MAX_CLAIMS)
        return -1;
    o->claims[o->nclaims++] = key;
    if (!(f = fopen(o->claims_file, "a")))
        return -1;
    fprintf(f, "%u %u\n", (unsigned)(key >> 32), (unsigned)(key & 0xffffffffu));
    return fclose(f) ? -1 : 0;
}

static int load_claims(operator_t *o)
{
    FILE *f = fopen(o->claims_file, "r");
    unsigned block, index;

    if (!(o->claims = calloc(MAX_CLAIMS, sizeof *o->claims)))
        return -1;
    if (!f)
        return 0;  /* none yet */
    while (o->nclaims < MAX_CLAIMS && fscanf(f, "%u %u", &block, &index) == 2)
        o->claims[o->nclaims++] = (uint64_t)block << 32 | index;
    fclose(f);
    return 0;
}

/* ---- the claim --------------------------------------------------------- */

typedef struct {
    int      status;        /* HTTP status */
    char     error[256];
    int      remark_proven;
    int      seed_proven;   /* the seed's block hash was proven */
    uint32_t ticks;         /* the replay's length */
    char     replay_cid[BOARD_CID_MAX];
    int      rank;          /* 0: not in the top 10 */
    char     board_cid[BOARD_CID_MAX];
    uint32_t stored_at;     /* Bulletin block of the new board, 0 if unchanged */
    int      record;        /* a new record: trophy minted */
    uint32_t trophy;
    uint32_t tx_block;      /* Asset Hub block of the operator's batch, 0 if none */
    int      tx_proven;
    int      stores_proven; /* the Bulletin stores' results */
} claim_result;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void refuse(claim_result *r, int status, const char *fmt, ...)
{
    va_list ap;
    r->status = status;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof r->error, fmt, ap);
    va_end(ap);
}

#define REFUSE(...) do { refuse(r, __VA_ARGS__); return; } while (0)

static int valid_name(const char *s)
{
    int i;
    for (i = 0; i < 3; i++)
        if (s[i] < 'A' || s[i] > 'Z')
            return 0;
    return s[3] == '\0';
}

/* {"Nfts": {call: args}} appended to the batch. */
static void add_call(cJSON *calls, const char *call, cJSON *args)
{
    cJSON *outer = cJSON_CreateObject(), *inner = cJSON_CreateObject();
    cJSON_AddItemToObject(inner, call, args);
    cJSON_AddItemToObject(outer, "Nfts", inner);
    cJSON_AddItemToArray(calls, outer);
}

static cJSON *attribute_args(uint32_t collection, const char *key, const char *value)
{
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(collection));
    cJSON_AddItemToArray(a, cJSON_CreateNull());
    cJSON_AddItemToArray(a, cJSON_CreateString("CollectionOwner"));
    cJSON_AddItemToArray(a, cJSON_CreateString(key));
    cJSON_AddItemToArray(a, cJSON_CreateString(value));
    return a;
}

/* Stores bytes on Bulletin, signed by the operator; cid gets the CID text. */
static int store(operator_t *o, const uint8_t *data, size_t len, char *cid, size_t cid_len,
                 uint32_t *block, int *proven, char *msg, size_t msg_len)
{
    pdot_store_result sr;
    int rc = pdot_bulletin_store(o->bul, &o->op, data, len, &sr);

    if (rc) {
        snprintf(msg, msg_len, "%s", rc == PDOT_E_DISPATCH ? sr.tx.error : pdot_last_error(o->bul));
        return rc;
    }
    *block = sr.tx.block;
    *proven = sr.tx.verified;
    return pdot_cid_to_string(&sr.cid, cid, cid_len);
}

/*
 * A replay's seed must come from the hash of a block before its score's
 * remark, and a recent one: the rocks were unknown before that block, so a
 * run could not be found or practised in advance. An hour of play at
 * Asset Hub's 6 s blocks is 600 blocks; this leaves room for slower ones.
 */
#define SEED_MAX_AGE 1200  /* blocks between the seed's block and the score's */

/* 0 when seed is block's; PDOT_E_NOTFOUND when the chain no longer keeps its hash. */
static int seed_check(operator_t *o, uint32_t block, uint32_t seed, int *proven, char *msg,
                      size_t msg_len)
{
    uint8_t hash[32];
    pdot_final_block at;
    int rc = pdot_block_hash_get(o->ah, block, hash, &at);

    if (rc) {
        snprintf(msg, msg_len, "%s", pdot_last_error(o->ah));
        return rc;
    }
    *proven = at.verified;
    if (replay_seed(hash) != seed) {
        snprintf(msg, msg_len, "seed %08x is not block %u's (%08x)", seed, block,
                 replay_seed(hash));
        return PDOT_E_INVALID;
    }
    return 0;
}

static void handle_claim(operator_t *o, const char *account, uint32_t score, const char *name,
                         uint32_t block, const uint8_t *replay, size_t replay_len,
                         claim_result *r)
{
    uint8_t who[32];
    char remark[80 + BOARD_CID_MAX], msg[192] = "", text[PDOT_NFT_DATA_MAX];
    pdot_cid cid;
    uint32_t replayed, replay_block, seed_block, seed;
    int proven = 0;
    pdot_remark rm;
    board b;
    board_record rec;
    pdot_nft_item item;
    pdot_tx_result tx;
    cJSON *calls, *args;
    uint64_t key;
    int rc;

    memset(r, 0, sizeof *r);
    r->status = 200;
    if (pdot_ss58_decode(account, who, NULL) || !valid_name(name))
        REFUSE(400, "bad account or initials");

    /* 1. The game was played: its replay re-runs to the score. */
    if (replay_check(replay, replay_len, &replayed, &r->ticks, &seed_block, &seed, msg,
                     sizeof msg))
        REFUSE(422, "the replay does not check: %s", msg);
    if (replayed != score)
        REFUSE(422, "the replay scores %u, not %u", replayed, score);
    if (!seed_block)
        REFUSE(422, "the game was not seeded from a block (played offline, or an old version)");
    if (seed_block >= block || block - seed_block > SEED_MAX_AGE)
        REFUSE(422, "the seed's block %u must be one of the %u before the score's block %u",
               seed_block, SEED_MAX_AGE, block);
    if (pdot_cid_compute(&cid, replay, replay_len) ||
        pdot_cid_to_string(&cid, r->replay_cid, sizeof r->replay_cid))
        REFUSE(500, "computing the replay's CID");

    /* 2. That player recorded that very game on chain. */
    snprintf(remark, sizeof remark, "pdot-asteroids score=%u name=%s replay=%s", score, name,
             r->replay_cid);
    rc = pdot_remark_find(o->ah, block, who, (const uint8_t *)remark, strlen(remark), &rm);
    if (rc == PDOT_E_NOTFOUND)
        REFUSE(403, "no such score on chain: %s", pdot_last_error(o->ah));
    if (rc == PDOT_E_INVALID)
        REFUSE(409, "%s; try again", pdot_last_error(o->ah));
    if (rc)
        REFUSE(502, "checking the score: %s", pdot_last_error(o->ah));
    r->remark_proven = rm.at.verified;
    key = (uint64_t)block << 32 | rm.index;
    if (claimed(o, key))
        REFUSE(409, "this score was already claimed");

    /* 3. The rocks came from that block's hash: the run could not be prepared. */
    rc = seed_check(o, seed_block, seed, &r->seed_proven, msg, sizeof msg);
    if (rc == PDOT_E_INVALID)
        REFUSE(422, "%s", msg);
    if (rc)
        REFUSE(502, "checking the seed: %s", msg);

    /* 4. The newest board and record. */
    if ((rc = board_read(o->ah, o->bul, o->collection, &b, r->board_cid, msg, sizeof msg)))
        REFUSE(502, "reading the board: %s", msg);
    if ((rc = board_record_read(o->ah, o->collection, &rec)))
        REFUSE(502, "reading the record: %s", pdot_last_error(o->ah));

    r->record = !rec.exists || score > rec.score;
    r->rank = board_find(&b, account, block);  /* already on: a retried claim */
    if (!r->rank && !r->record && !board_rank(&b, score)) {
        if (add_claim(o, key))
            logf_("could not save the claim to %s", o->claims_file);
        return;  /* checked, but nothing to change */
    }

    /* 5. The replay on Bulletin, where anyone can fetch and re-run it. */
    if ((rc = store(o, replay, replay_len, text, sizeof text, &replay_block, &r->stores_proven,
                    msg, sizeof msg)))
        REFUSE(502, "storing the replay on Bulletin: %s", msg);
    if (strcmp(text, r->replay_cid) != 0)
        REFUSE(502, "Bulletin stored the replay as %s, expected %s", text, r->replay_cid);

    calls = cJSON_CreateArray();

    /* 6. The board: unless this very score is on it already. */
    if (!r->rank) {
        board_entry e;
        memset(&e, 0, sizeof e);
        e.score = score;
        memcpy(e.name, name, 4);
        snprintf(e.account, sizeof e.account, "%s", account);
        e.block = block;
        memcpy(e.replay, r->replay_cid, sizeof e.replay);
        if ((r->rank = board_insert(&b, &e))) {
            char *json = board_json(&b);
            if (!json) {
                cJSON_Delete(calls);
                REFUSE(500, "out of memory");
            }
            rc = store(o, (const uint8_t *)json, strlen(json), r->board_cid, sizeof r->board_cid,
                       &r->stored_at, &proven, msg, sizeof msg);
            r->stores_proven = r->stores_proven && proven;
            free(json);
            if (rc) {
                cJSON_Delete(calls);
                REFUSE(502, "storing the board on Bulletin: %s", msg);
            }
            add_call(calls, "set_attribute", attribute_args(o->collection, BOARD_KEY,
                                                            r->board_cid));
        }
    }

    /* 7. The record: a trophy for a new best score, naming its replay. */
    if (r->record) {
        for (r->trophy = 0;; r->trophy++) {
            if ((rc = pdot_nft_item_get(o->ah, o->collection, r->trophy, &item))) {
                cJSON_Delete(calls);
                REFUSE(502, "finding a free item: %s", pdot_last_error(o->ah));
            }
            if (!item.exists)
                break;
        }
        args = cJSON_CreateArray();
        cJSON_AddItemToArray(args, cJSON_CreateNumber(o->collection));
        cJSON_AddItemToArray(args, cJSON_CreateNumber(r->trophy));
        cJSON_AddItemToArray(args, cJSON_CreateString(account));
        cJSON_AddItemToArray(args, cJSON_CreateNull());
        add_call(calls, "mint", args);
        args = cJSON_CreateArray();
        snprintf(text, sizeof text, "pdot-asteroids record %u by %s, replay %s", score, name,
                 r->replay_cid);
        cJSON_AddItemToArray(args, cJSON_CreateNumber(o->collection));
        cJSON_AddItemToArray(args, cJSON_CreateNumber(r->trophy));
        cJSON_AddItemToArray(args, cJSON_CreateString(text));
        add_call(calls, "set_metadata", args);
        snprintf(text, sizeof text, "%u %s %s", score, account, name);
        add_call(calls, "set_attribute", attribute_args(o->collection, RECORD_KEY, text));
    }

    /* 8. All Asset Hub changes in one transaction. */
    if (cJSON_GetArraySize(calls) > 0) {
        cJSON *wrap = cJSON_CreateArray();
        char *json;
        uint8_t *call;
        size_t len;

        cJSON_AddItemToArray(wrap, calls);
        json = cJSON_PrintUnformatted(wrap);
        cJSON_Delete(wrap);
        if (!json)
            REFUSE(500, "out of memory");
        rc = pdot_call_from_json(o->ah, "Utility", "batch_all", json, &call, &len);
        cJSON_free(json);
        if (rc)
            REFUSE(500, "encoding the batch: %s", pdot_last_error(o->ah));
        rc = pdot_tx_submit(o->ah, &o->op, call, len, NULL, &tx);
        pdot_free(call);
        if (rc)
            REFUSE(502, "updating the collection: %s",
                          rc == PDOT_E_DISPATCH ? tx.error : pdot_last_error(o->ah));
        r->tx_block = tx.block;
        r->tx_proven = tx.verified;
    } else {
        cJSON_Delete(calls);
    }

    if (add_claim(o, key))
        logf_("could not save the claim to %s", o->claims_file);
}

/* ---- HTTP ---------------------------------------------------------------- */

static void reply(int fd, int status, cJSON *body)
{
    char head[256], *json = cJSON_PrintUnformatted(body);
    const char *why = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                      status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
                      status == 409 ? "Conflict" : status == 422 ? "Unprocessable Content" :
                      status == 502 ? "Bad Gateway" : "Error";
    size_t len = json ? strlen(json) : 0;
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, why, len);

    if (send(fd, head, (size_t)n, 0) == n && len)
        send(fd, json, len, 0);
    cJSON_free(json);
}

static void reply_error(int fd, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg);
    reply(fd, status, o);
    cJSON_Delete(o);
}

#define REPLY_ERROR(fd, status, msg) do { reply_error(fd, status, msg); return; } while (0)

/* The request, NUL-terminated: headers, then a body of Content-Length bytes. */
static int read_request(int fd, char *buf, size_t cap, char **body, size_t *body_len)
{
    size_t got = 0, want = 0;
    char *end = NULL;

    while (got < cap - 1) {
        ssize_t n = recv(fd, buf + got, cap - 1 - got, 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
        buf[got] = '\0';
        if (!end && (end = strstr(buf, "\r\n\r\n"))) {
            const char *cl = buf;
            *body = end + 4;
            while ((cl = strchr(cl, '\n')) && cl < end) {
                cl++;
                if (strncasecmp(cl, "Content-Length:", 15) == 0)
                    want = strtoul(cl + 15, NULL, 10);
            }
            if (want > cap - 1 - (size_t)(*body - buf))
                return -1;
        }
        if (end && got - (size_t)(*body - buf) >= want) {
            *body_len = want;
            (*body)[want] = '\0';
            return 0;
        }
    }
    return -1;
}

static int unhex(const char *hex, uint8_t **out, size_t *len)
{
    size_t n = strlen(hex), i;
    unsigned b;

    if (n % 2 || n / 2 > REPLAY_MAX_BYTES || !(*out = malloc(n / 2 + 1)))
        return -1;
    for (i = 0; i < n / 2; i++) {
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &b) != 1) {
            free(*out);
            return -1;
        }
        (*out)[i] = (uint8_t)b;
    }
    *len = n / 2;
    return 0;
}

static void serve(operator_t *o, int fd, char *buf, size_t cap)
{
    char *body = NULL;
    size_t body_len = 0, replay_len = 0;
    uint8_t *replay = NULL;
    cJSON *req, *out;
    const cJSON *acct, *score, *name, *block, *rp;
    claim_result r;

    if (read_request(fd, buf, cap, &body, &body_len))
        REPLY_ERROR(fd, 400, "bad request");
    if (strncmp(buf, "GET /health ", 12) == 0) {
        out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", 1);
        cJSON_AddStringToObject(out, "operator", o->op_addr);
        cJSON_AddNumberToObject(out, "collection", o->collection);
        cJSON_AddBoolToObject(out, "proven", o->proven);
        reply(fd, 200, out);
        cJSON_Delete(out);
        return;
    }
    if (strncmp(buf, "POST /claim ", 12) != 0)
        REPLY_ERROR(fd, 404, "GET /health or POST /claim");

    req = cJSON_ParseWithLength(body, body_len);
    acct = cJSON_GetObjectItemCaseSensitive(req, "account");
    score = cJSON_GetObjectItemCaseSensitive(req, "score");
    name = cJSON_GetObjectItemCaseSensitive(req, "name");
    block = cJSON_GetObjectItemCaseSensitive(req, "block");
    rp = cJSON_GetObjectItemCaseSensitive(req, "replay");
    if (!cJSON_IsString(rp) || unhex(rp->valuestring, &replay, &replay_len)) {
        cJSON_Delete(req);
        REPLY_ERROR(fd, 400, "expected the game's replay, as hex");
    }
    if (!cJSON_IsString(acct) || !cJSON_IsString(name) || !cJSON_IsNumber(score) ||
        !cJSON_IsNumber(block) || score->valuedouble < 0 || score->valuedouble > 4294967295.0 ||
        block->valuedouble < 1 || block->valuedouble > 4294967295.0) {
        cJSON_Delete(req);
        free(replay);
        REPLY_ERROR(fd, 400, "expected {account, score, name, block, replay}");
    }
    logf_("claim: %s %s score %u at block %u, replay of %zu bytes", name->valuestring,
          acct->valuestring, (uint32_t)score->valuedouble, (uint32_t)block->valuedouble,
          replay_len);
    handle_claim(o, acct->valuestring, (uint32_t)score->valuedouble, name->valuestring,
                 (uint32_t)block->valuedouble, replay, replay_len, &r);
    cJSON_Delete(req);
    free(replay);

    if (r.status != 200) {
        logf_("  refused (%d): %s", r.status, r.error);
        REPLY_ERROR(fd, r.status, r.error);
    }
    logf_("  ok: replay of %u ticks re-ran to the score, remark and seed %s, rank %d%s%s; "
          "results %s", r.ticks,
          r.remark_proven && r.seed_proven ? "proven" : "checked (trusted RPC)", r.rank,
          r.stored_at ? ", board stored" : "", r.record ? ", new record" : "",
          r.tx_block || r.stored_at ? (r.tx_proven && r.stores_proven ? "proven" : "trusted RPC")
                                    : "none");
    out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddNumberToObject(out, "ticks", r.ticks);
    cJSON_AddStringToObject(out, "replay", r.replay_cid);
    cJSON_AddBoolToObject(out, "score_proven", r.remark_proven && r.seed_proven);
    cJSON_AddNumberToObject(out, "rank", r.rank);
    cJSON_AddStringToObject(out, "board", r.board_cid);
    cJSON_AddNumberToObject(out, "stored_at", r.stored_at);
    cJSON_AddBoolToObject(out, "record", r.record);
    if (r.record)
        cJSON_AddNumberToObject(out, "trophy", r.trophy);
    else
        cJSON_AddNullToObject(out, "trophy");
    cJSON_AddNumberToObject(out, "block", r.tx_block);
    cJSON_AddBoolToObject(out, "block_proven", r.tx_proven);
    cJSON_AddBoolToObject(out, "stores_proven", r.stores_proven);
    reply(fd, 200, out);
    cJSON_Delete(out);
}

/* ---- setup ---------------------------------------------------------------- */

static int hex32(const char *hex, uint8_t out[32])
{
    unsigned b;
    int i;

    if (strncmp(hex, "0x", 2) == 0)
        hex += 2;
    if (strlen(hex) != 64)
        return -1;
    for (i = 0; i < 32; i++) {
        if (sscanf(hex + 2 * i, "%2x", &b) != 1)
            return -1;
        out[i] = (uint8_t)b;
    }
    return 0;
}

static int set_metadata_hash(pdot_client *c, const char *mode)
{
    char symbol[32];
    const char *colon = strchr(mode, ':');

    if (strcmp(mode, "off") == 0)
        return 0;
    if (strcmp(mode, "on") == 0)
        return pdot_client_set_metadata_hash(c, 1, NULL, -1);
    if (!colon || colon == mode || (size_t)(colon - mode) >= sizeof symbol)
        return PDOT_E_INVALID;
    memcpy(symbol, mode, (size_t)(colon - mode));
    symbol[colon - mode] = '\0';
    return pdot_client_set_metadata_hash(c, 1, symbol, atoi(colon + 1));
}

static int set_verifier(pdot_client *c, const char *relay, uint32_t para_id, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[1 << 16];
    size_t len;

    if (!f)
        return PDOT_E_NOTFOUND;
    len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return pdot_client_set_verifier(c, relay, para_id, buf, len);
}

/* The verifier's progress, so the next start skips the authority-set changes seen. */
static void save_checkpoint(operator_t *o)
{
    uint8_t *blob;
    size_t len;
    FILE *f;

    if (!o->proven || pdot_client_checkpoint(o->ah, &blob, &len))
        return;
    if ((f = fopen(o->checkpoint, "wb"))) {
        fwrite(blob, 1, len, f);
        fclose(f);
    }
    pdot_free(blob);
}

/* ---- retention --------------------------------------------------------- */

/* "expires at #N, in M blocks" for one stored item; 0 when it is still stored. */
static int expiry(operator_t *o, const char *cid_text, pdot_storage_proof *p, char *out,
                  size_t cap)
{
    pdot_cid cid;
    int rc;

    if (pdot_cid_from_string(&cid, cid_text)) {
        snprintf(out, cap, "not a CID");
        return PDOT_E_INVALID;
    }
    if ((rc = pdot_bulletin_stored(o->bul, &cid, p))) {
        snprintf(out, cap, "%s", rc == PDOT_E_NOTFOUND ? "EXPIRED: no longer stored"
                                                        : pdot_last_error(o->bul));
        return rc;
    }
    snprintf(out, cap, "expires at #%u, in %d blocks%s", p->expires,
             (int)(p->expires - p->proven_at), p->verified ? " (proven)" : "");
    return 0;
}

/* Renews one item if it expires within the margin. 1 when renewed. */
static int renew_one(operator_t *o, const char *what, const char *cid_text)
{
    pdot_storage_proof p;
    pdot_tx_result tx;
    pdot_cid cid;
    char line[160];
    uint32_t margin;
    int rc;

    if (expiry(o, cid_text, &p, line, sizeof line)) {
        logf_("retention: %s %.24s...: %s", what, cid_text, line);
        return 0;
    }
    margin = o->renew_before ? o->renew_before : (p.expires - p.block) / 4;
    if (p.expires > p.proven_at + margin)
        return 0;
    pdot_cid_from_string(&cid, cid_text);
    rc = pdot_bulletin_renew(o->bul, &o->op, &cid, &tx);
    if (rc) {
        logf_("retention: renewing %s %.24s... failed: %s", what, cid_text,
              rc == PDOT_E_DISPATCH ? tx.error : pdot_last_error(o->bul));
        return 0;
    }
    expiry(o, cid_text, &p, line, sizeof line);
    logf_("retention: renewed %s %.24s... in block %u%s: %s", what, cid_text, tx.block,
          tx.verified ? " (proven)" : "", line);
    return 1;
}

/* The board and every replay it names. */
static void renew_due(operator_t *o)
{
    board b;
    char cid[BOARD_CID_MAX], msg[192] = "";
    size_t i;
    int n = 0;

    if (board_read(o->ah, o->bul, o->collection, &b, cid, msg, sizeof msg)) {
        logf_("retention: reading the board: %s", msg);
        return;
    }
    if (!cid[0])
        return;  /* no board yet */
    n += renew_one(o, "board", cid);
    for (i = 0; i < b.n; i++)
        if (b.e[i].replay[0])
            n += renew_one(o, "replay", b.e[i].replay);
    if (n)
        logf_("retention: %d item(s) renewed", n);
}

/*
 * --audit: what anyone can do with no key at all. Reads the board, fetches
 * each entry's replay from Bulletin (checked against its CID) and re-runs it.
 */
static int audit(operator_t *o)
{
    board b;
    char cid_text[BOARD_CID_MAX], msg[192] = "";
    size_t i;
    int bad = 0;

    if (board_read(o->ah, o->bul, o->collection, &b, cid_text, msg, sizeof msg)) {
        fprintf(stderr, "reading the board: %s\n", msg);
        return 1;
    }
    printf("board %s: %zu entries\n", cid_text[0] ? cid_text : "(none yet)", b.n);
    if (cid_text[0]) {
        pdot_storage_proof p;
        char line[160];
        expiry(o, cid_text, &p, line, sizeof line);
        printf("    the board %s\n", line);
    }
    for (i = 0; i < b.n; i++) {
        const board_entry *e = &b.e[i];
        pdot_cid cid;
        uint8_t *data;
        size_t len;
        uint32_t score, ticks, seed_block, seed;

        printf("%2zu. %s %7u  block %-6u ", i + 1, e->name, e->score, e->block);
        if (!e->replay[0]) {
            printf("no replay (from before replays)\n");
            continue;
        }
        if (pdot_cid_from_string(&cid, e->replay) ||
            pdot_bulletin_fetch(o->bul, &cid, &data, &len)) {
            printf("replay %s: cannot fetch: %s\n", e->replay, pdot_last_error(o->bul));
            bad++;
            continue;
        }
        if (replay_check(data, len, &score, &ticks, &seed_block, &seed, msg, sizeof msg)) {
            printf("replay does not check: %s\n", msg);
            bad++;
        } else if (score != e->score) {
            printf("replay scores %u: WRONG\n", score);
            bad++;
        } else {
            pdot_storage_proof p;
            char line[160], seeded[200];
            int seed_proven = 0, rc;
            expiry(o, e->replay, &p, line, sizeof line);
            if (!seed_block)
                snprintf(seeded, sizeof seeded, "not seeded from a block (an older replay)");
            else if (!(rc = seed_check(o, seed_block, seed, &seed_proven, msg, sizeof msg)))
                snprintf(seeded, sizeof seeded, "seeded from block %u%s", seed_block,
                         seed_proven ? ", proven" : "");
            else if (rc == PDOT_E_NOTFOUND)
                snprintf(seeded, sizeof seeded, "seeded from block %u, no longer checkable: %s",
                         seed_block, msg);
            else {
                snprintf(seeded, sizeof seeded, "SEED WRONG: %s", msg);
                bad++;
            }
            printf("replay re-runs to %u in %u ticks (%.0f s of play)\n"
                   "                                %s\n"
                   "                                its replay %s\n", score, ticks,
                   (double)ticks / SIM_HZ, seeded, line);
        }
        pdot_free(data);
    }
    return bad != 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: asteroids-operator [options]\n"
            "  --rpc URL            Asset Hub RPC (http://127.0.0.1:9944)\n"
            "  --bulletin URL       Bulletin RPC (http://127.0.0.1:9945)\n"
            "  --operator HEX       the operator's sr25519 seed (//Bob)\n"
            "  --collection N       trophy collection, owned by the operator (0)\n"
            "  --listen ADDR:PORT   where to serve (127.0.0.1:8090)\n"
            "  --claims FILE        honoured claims (asteroids-claims.txt)\n"
            "  --metadata-hash M    on, off or SYMBOL:DECIMALS (on)\n"
            "  --relay URL --checkpoint FILE [--para-id N] [--bulletin-para-id N]\n"
            "                       prove scores and results from relay-chain finality\n"
            "                       (Asset Hub 1000, Bulletin 2487)\n"
            "  --renew-check S      check retention every S seconds (300; 0: never)\n"
            "  --renew-before N     renew what expires within N blocks (a quarter of\n"
            "                       the retention period)\n"
            "  --audit              no server: re-run every replay on the board, show\n"
            "                       when each expires, then exit\n");
}

int main(int argc, char **argv)
{
    operator_t o;
    const char *rpc = "http://127.0.0.1:9944", *bulletin = "http://127.0.0.1:9945";
    const char *seed = DEV_OPERATOR, *listen_at = "127.0.0.1:8090", *mdhash = "on";
    const char *relay = NULL;
    uint32_t para_id = 1000, bulletin_para_id = 2487;
    uint8_t seed_bytes[32];
    char host[64];
    const char *colon;
    struct sockaddr_in sa;
    struct sigaction sig;
    char *reqbuf;
    int audit_only = 0, renew_check = 300;
    time_t next_renew = 0;
    int i, lfd, one = 1, rc;

    memset(&o, 0, sizeof o);
    o.claims_file = "asteroids-claims.txt";
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--audit")) {
            audit_only = 1;
            continue;
        }
        if (!v) {
            usage();
            return 2;
        }
        i++;
        if (!strcmp(a, "--rpc")) rpc = v;
        else if (!strcmp(a, "--bulletin")) bulletin = v;
        else if (!strcmp(a, "--operator")) seed = v;
        else if (!strcmp(a, "--collection")) o.collection = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--listen")) listen_at = v;
        else if (!strcmp(a, "--claims")) o.claims_file = v;
        else if (!strcmp(a, "--metadata-hash")) mdhash = v;
        else if (!strcmp(a, "--relay")) relay = v;
        else if (!strcmp(a, "--checkpoint")) o.checkpoint = v;
        else if (!strcmp(a, "--para-id")) para_id = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--bulletin-para-id")) bulletin_para_id = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--renew-check")) renew_check = atoi(v);
        else if (!strcmp(a, "--renew-before")) o.renew_before = (uint32_t)strtoul(v, NULL, 10);
        else {
            usage();
            return 2;
        }
    }
    if (!!relay != !!o.checkpoint) {
        fprintf(stderr, "--relay and --checkpoint go together\n");
        return 2;
    }
    if (hex32(seed, seed_bytes) || pdot_signer_sr25519_from_seed(&o.op, seed_bytes)) {
        fprintf(stderr, "--operator: expected a 32-byte hex seed\n");
        return 2;
    }
    memset(seed_bytes, 0, sizeof seed_bytes);
    pdot_ss58_encode(o.op.account_id, 42, o.op_addr, sizeof o.op_addr);
    if (load_claims(&o)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    o.ah = pdot_client_new(rpc, NULL);
    o.bul = pdot_client_new(bulletin, NULL);
    if (!o.ah || !o.bul) {
        fprintf(stderr, "cannot create the clients\n");
        return 1;
    }
    if (set_metadata_hash(o.ah, mdhash) || set_metadata_hash(o.bul, mdhash)) {
        fprintf(stderr, "--metadata-hash: expected on, off or SYMBOL:DECIMALS\n");
        return 2;
    }
    if (relay) {
        if ((rc = set_verifier(o.ah, relay, para_id, o.checkpoint))) {
            fprintf(stderr, "verifier: %s\n",
                    rc == PDOT_E_NOTFOUND ? "no checkpoint file" : pdot_last_error(o.ah));
            return 1;
        }
        /* Bulletin too: store results and the board's attribute reads are proven. */
        if ((rc = set_verifier(o.bul, relay, bulletin_para_id, o.checkpoint))) {
            fprintf(stderr, "verifier (Bulletin): %s\n", pdot_last_error(o.bul));
            return 1;
        }
        o.proven = 1;
    }

    if (audit_only) {
        rc = audit(&o);
        pdot_client_free(o.ah);
        pdot_client_free(o.bul);
        pdot_signer_wipe(&o.op);
        free(o.claims);
        return rc;
    }

    colon = strrchr(listen_at, ':');
    if (!colon || (size_t)(colon - listen_at) >= sizeof host) {
        fprintf(stderr, "--listen: expected ADDR:PORT\n");
        return 2;
    }
    memcpy(host, listen_at, (size_t)(colon - listen_at));
    host[colon - listen_at] = '\0';
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "--listen: %s is not an IPv4 address\n", host);
        return 2;
    }
    if ((lfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ||
        bind(lfd, (struct sockaddr *)&sa, sizeof sa) || listen(lfd, 16)) {
        fprintf(stderr, "cannot listen on %s: %s\n", listen_at, strerror(errno));
        return 1;
    }

    memset(&sig, 0, sizeof sig);
    sig.sa_handler = on_signal;  /* no SA_RESTART: accept returns on Ctrl-C */
    sigaction(SIGINT, &sig, NULL);
    sigaction(SIGTERM, &sig, NULL);
    signal(SIGPIPE, SIG_IGN);

    logf_("%s operating collection %u%s, listening on http://%s (%zu claims honoured so far)",
          o.op_addr, o.collection, o.proven ? ", proving from relay-chain finality" : "",
          listen_at, o.nclaims);
    if (!(reqbuf = malloc(REQ_MAX))) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    while (!stopping) {
        struct timeval tv = { 10, 0 };
        struct pollfd pfd = { 0, POLLIN, 0 };
        int fd;

        if (renew_check > 0 && time(NULL) >= next_renew) {
            renew_due(&o);
            save_checkpoint(&o);
            next_renew = time(NULL) + renew_check;
        }
        pfd.fd = lfd;
        if (poll(&pfd, 1, 5000) <= 0)
            continue;  /* timeout, or EINTR on Ctrl-C */
        if ((fd = accept(lfd, NULL, NULL)) < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#if defined(SO_NOSIGPIPE)
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        serve(&o, fd, reqbuf, REQ_MAX);
        close(fd);
        save_checkpoint(&o);
    }
    logf_("stopping");
    close(lfd);
    pdot_client_free(o.ah);
    pdot_client_free(o.bul);
    pdot_signer_wipe(&o.op);
    free(o.claims);
    free(reqbuf);
    return 0;
}
