/*
 * The game's use of libpdot. Each chain action is a job: its work function
 * runs on pdot_async's worker thread with blocking libpdot calls, and its
 * done callback updates chain_view inside chain_poll, on the game thread.
 *
 *   open    the player signs once: Proxy.add_proxy(session key, NonTransfer)
 *           and funds the key, in one transaction (pdot_proxy_add). The key's
 *           seed is saved, so a later run resumes the session with no
 *           signature, or cleans up one that was left half closed.
 *   record  reads the collection's "best" attribute: the score to beat
 *   quote   the fee of the score transaction (TransactionPaymentApi)
 *   submit  the session key, acting for the player, sends
 *           System.remark_with_event("pdot-asteroids score=N name=ABC replay=CID"),
 *           CID being the replay's (replay.h): the score commits to its game
 *   claim   asks asteroids-operator (HTTP) to honour that score, sending the
 *           replay: it re-runs the game, finds the remark on chain, then puts
 *           a top-10 score on the board and mints a trophy for a new record.
 *           The game holds no operator key.
 *   board   reads the leaderboard: the collection's "board" attribute holds
 *           the CID of a JSON document on the Bulletin chain (board.h),
 *           fetched and checked against the CID
 *   close   leftover funds back to the player, then the player removes the proxy
 *
 * Watches keep the newest final block and the session key's balance current.
 */
#include "chain.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "replay.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define PROXY_TYPE "NonTransfer"  /* the session key can do anything but move funds */
#define CLAIM_TIMEOUT_S 300       /* the operator may wait on several blocks */

struct chain {
    pdot_async  *a;
    pdot_client *bul;            /* Bulletin, used only inside jobs on a's worker */
    pdot_signer  player, session;
    char         operator_url[256];  /* "" for none */
    int          has_saved;      /* session holds the key saved by an earlier run */
    char         session_file[512];
    int          funds_returned; /* closing: leftover funds went back */
    uint32_t     collection;
    char         fund[32];
    char         checkpoint[512];
    int          close_pending;  /* chain_close came while the session was opening */
    int          has_balance;    /* session_free read, shown once decimals are known */
    uint32_t     score;          /* the finished game's, from chain_set_replay */
    uint8_t     *replay;         /* its encoded replay; NULL if it could not be kept */
    size_t       replay_len;
    char         replay_cid[BOARD_CID_MAX];
    pdot_u128    session_free;
    chain_view   v;
};

enum { J_OPEN, J_RECORD, J_QUOTE, J_SUBMIT, J_CLAIM, J_BOARD, J_CLOSE_FUNDS, J_CLOSE_PROXY };

struct job {
    chain         *ch;
    int            kind;
    uint32_t       score;
    char           name[4];     /* the player's initials, A-Z */
    pdot_tx_result tx;
    pdot_fee       fee;
    uint32_t       item;        /* claim: the trophy minted */
    int            new_record;  /* claim: a new record */
    char           symbol[16];
    uint8_t        decimals;
    int            has_record, record_verified;
    uint32_t       record;
    char           holder[PDOT_SS58_MAX_LEN];
    char           holder_name[4];
    int            resumed;     /* open: the saved session is still in place */
    int            cleaned;     /* open: a half-closed saved session was cleaned up */
    pdot_signer    fresh;       /* open: the new session key */
    int            save_failed; /* open: the session file could not be written */
    uint32_t       block;       /* claim: the block holding the score's remark */
    board          b;           /* board: the leaderboard read */
    char           board_cid[CHAIN_CID_STR_MAX];
    int            rank;        /* claim: the place taken, 0 for none */
    uint32_t       stored_at;   /* claim: the Bulletin block holding the new board */
    int            proven;      /* claim: the operator proved the score */
    char           replay_cid[BOARD_CID_MAX];  /* quote, submit, claim: "" for none */
    uint8_t       *replay;      /* submit, claim: owned, passed from submit to claim */
    size_t         replay_len;
    char           msg[256];    /* Bulletin and operator errors (the done callback
                                   only sees Asset Hub's) */
};

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void status(chain *ch, int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ch->v.status, sizeof ch->v.status, fmt, ap);
    va_end(ap);
    ch->v.status_error = error;
    fprintf(stderr, "[chain] %s\n", ch->v.status);
}

/* At most 6 decimals: enough for fees, short enough for the HUD. */
static void amount(const chain *ch, pdot_u128 v, char *out, size_t cap)
{
    char *dot;

    if (pdot_u128_format(v, ch->v.decimals, out, cap)) {
        snprintf(out, cap, "?");
        return;
    }
    if ((dot = strchr(out, '.')) && strlen(dot) > 7)
        dot[7] = '\0';
}

/* "block #N, fee X WND, proven" */
static void describe_tx(const chain *ch, const pdot_tx_result *tx, char *out, size_t cap)
{
    char fee[48] = "";
    if (tx->has_fee) {
        char a[40];
        amount(ch, tx->fee, a, sizeof a);
        snprintf(fee, sizeof fee, ", fee %s %s", a, ch->v.symbol);
    }
    snprintf(out, cap, "block %u%s, %s", tx->block, fee,
             tx->verified ? "proven" : "trusted RPC");
}

/* The score remark; without a replay the operator will not honour it. */
static int remark_call(pdot_client *c, const struct job *j, uint8_t **call, size_t *len)
{
    char args[80 + BOARD_CID_MAX];
    if (j->replay_cid[0])
        snprintf(args, sizeof args, "[\"pdot-asteroids score=%u name=%s replay=%s\"]", j->score,
                 j->name, j->replay_cid);
    else
        snprintf(args, sizeof args, "[\"pdot-asteroids score=%u name=%s\"]", j->score, j->name);
    return pdot_call_from_json(c, "System", "remark_with_event", args, call, len);
}

/* ---- work: runs on the worker thread --------------------------------- */

/* "SEED PLAYER": the session key's seed (hex) and the account it acts for. */
static int save_session(const chain *ch, const uint8_t seed[32])
{
    FILE *f;
    int i;

#if !defined(_WIN32)
    mode_t old = umask(077);  /* a key: readable by this user only */
    f = fopen(ch->session_file, "w");
    umask(old);
#else
    f = fopen(ch->session_file, "w");
#endif
    if (!f)
        return PDOT_E_INVALID;
    for (i = 0; i < 32; i++)
        fprintf(f, "%02x", seed[i]);
    fprintf(f, " %s\n", ch->v.player);
    return fclose(f) ? PDOT_E_INVALID : 0;
}

static int load_session(chain *ch)
{
    FILE *f = fopen(ch->session_file, "r");
    char hex[65], who[PDOT_SS58_MAX_LEN];
    uint8_t seed[32];
    unsigned b;
    int i, ok;

    if (!f)
        return 0;
    ok = fscanf(f, "%64s %63s", hex, who) == 2 && strlen(hex) == 64 &&
         strcmp(who, ch->v.player) == 0;
    fclose(f);
    for (i = 0; ok && i < 32; i++) {
        ok = sscanf(hex + 2 * i, "%2x", &b) == 1;
        seed[i] = (uint8_t)b;
    }
    ok = ok && pdot_signer_sr25519_from_seed(&ch->session, seed) == 0;
    memset(seed, 0, sizeof seed);
    memset(hex, 0, sizeof hex);
    if (!ok)
        fprintf(stderr, "[chain] ignoring %s: not a session of this player\n", ch->session_file);
    return ok;
}

static int proxy_listed(const pdot_proxies *p, const uint8_t who[32])
{
    size_t i;
    for (i = 0; i < p->n; i++)
        if (memcmp(p->list[i].delegate, who, 32) == 0 && strcmp(p->list[i].type, PROXY_TYPE) == 0)
            return 1;
    return 0;
}

static int work_open(pdot_client *c, void *ud)
{
    struct job *j = ud;
    chain *ch = j->ch;
    pdot_u128 fund;
    uint8_t seed[32];
    int rc;

    if ((rc = pdot_client_token(c, j->symbol, sizeof j->symbol, &j->decimals)) ||
        (rc = pdot_u128_parse(ch->fund, j->decimals, &fund)))
        return rc;

    if (ch->has_saved) {
        /* The saved session: resume it if it is whole, else clean it up. */
        pdot_proxies px;
        pdot_balance bal;
        pdot_transfer t;
        int listed;

        if ((rc = pdot_proxies_get(c, ch->player.account_id, &px)) ||
            (rc = pdot_balance_get(c, ch->session.account_id, &bal)))
            return rc;
        listed = proxy_listed(&px, ch->session.account_id);
        if (listed && bal.exists) {
            j->resumed = 1;
            return 0;
        }
        j->cleaned = 1;
        if (bal.exists) {
            memset(&t, 0, sizeof t);
            t.kind = PDOT_TRANSFER_ALL;
            memcpy(t.dest, ch->player.account_id, 32);
            if ((rc = pdot_transfer_submit(c, &ch->session, &t, NULL, &j->tx)))
                return rc;
        }
        if (listed && (rc = pdot_proxy_remove(c, &ch->player, ch->session.account_id,
                                              PROXY_TYPE, &j->tx)))
            return rc;
    }

    /* Saved before it goes on chain, so a crash cannot strand it. */
    if ((rc = pdot_signer_sr25519_random(&j->fresh, seed)))
        return rc;
    rc = save_session(ch, seed);
    memset(seed, 0, sizeof seed);
    if (rc) {
        j->save_failed = 1;
        return rc;
    }
    return pdot_proxy_add(c, &ch->player, j->fresh.account_id, PROXY_TYPE, fund, &j->tx);
}

static int work_record(pdot_client *c, void *ud)
{
    struct job *j = ud;
    board_record r;
    int rc;

    if ((rc = board_record_read(c, j->ch->collection, &r)) || !r.exists)
        return rc;
    j->has_record = 1;
    j->record = r.score;
    j->record_verified = r.verified;
    memcpy(j->holder, r.holder, sizeof j->holder);
    memcpy(j->holder_name, r.name, sizeof j->holder_name);
    return 0;
}

static int work_quote(pdot_client *c, void *ud)
{
    struct job *j = ud;
    pdot_tx_options o = { { 0, 0 }, j->ch->player.account_id };
    uint8_t *call;
    size_t len;
    int rc;

    if ((rc = remark_call(c, j, &call, &len)))
        return rc;
    rc = pdot_tx_fee(c, &j->ch->session, call, len, &o, &j->fee);
    pdot_free(call);
    return rc;
}

static int work_submit(pdot_client *c, void *ud)
{
    struct job *j = ud;
    pdot_tx_options o = { { 0, 0 }, j->ch->player.account_id };
    uint8_t *call;
    size_t len;
    int rc;

    if ((rc = remark_call(c, j, &call, &len)))
        return rc;
    rc = pdot_tx_submit(c, &j->ch->session, call, len, &o, &j->tx);
    pdot_free(call);
    return rc;
}

/* ---- claim: HTTP to the operator, with libcurl ---------------------- */

typedef struct {
    char  *data;
    size_t len;
} http_body;

static size_t on_body(char *p, size_t size, size_t n, void *ud)
{
    http_body *b = ud;
    size_t add = size * n;
    char *grown;

    if (b->len + add > 65536 || !(grown = realloc(b->data, b->len + add + 1)))
        return 0;  /* aborts the transfer */
    memcpy(grown + b->len, p, add);
    b->data = grown;
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

/* Gives up as soon as the game quits (pdot_async_free cancels the job). */
static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    return pdot_client_cancelled(ud);
}

static int work_claim(pdot_client *c, void *ud)
{
    struct job *j = ud;
    chain *ch = j->ch;
    char url[300], *req;
    size_t req_cap, i, at;
    http_body body = { NULL, 0 };
    struct curl_slist *hdr = NULL;
    cJSON *res = NULL;
    const cJSON *ok, *v;
    CURL *h;
    CURLcode cc;
    long status = 0;
    int rc = PDOT_E_RPC;

    snprintf(url, sizeof url, "%s/claim", ch->operator_url);
    /* {"account", "score", "name", "block", "replay": hex} */
    req_cap = 256 + 2 * j->replay_len;
    if (!(req = malloc(req_cap)))
        return PDOT_E_NOMEM;
    at = (size_t)snprintf(req, req_cap,
                          "{\"account\":\"%s\",\"score\":%u,\"name\":\"%s\",\"block\":%u,"
                          "\"replay\":\"", ch->v.player, j->score, j->name, j->block);
    for (i = 0; i < j->replay_len; i++)
        at += (size_t)snprintf(req + at, req_cap - at, "%02x", j->replay[i]);
    snprintf(req + at, req_cap - at, "\"}");
    if (!(h = curl_easy_init())) {
        free(req);
        return PDOT_E_NOMEM;
    }
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, req);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)CLAIM_TIMEOUT_S);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
    cc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(h);

    if (cc == CURLE_ABORTED_BY_CALLBACK && pdot_client_cancelled(c)) {
        rc = PDOT_E_CANCELLED;
        goto out;
    }
    if (cc != CURLE_OK) {
        snprintf(j->msg, sizeof j->msg, "operator at %s: %s", ch->operator_url,
                 curl_easy_strerror(cc));
        goto out;
    }
    res = body.data ? cJSON_Parse(body.data) : NULL;
    ok = cJSON_GetObjectItemCaseSensitive(res, "ok");
    if (!cJSON_IsBool(ok)) {
        snprintf(j->msg, sizeof j->msg, "operator: HTTP %ld, not a claim answer", status);
        goto out;
    }
    if (!cJSON_IsTrue(ok)) {
        v = cJSON_GetObjectItemCaseSensitive(res, "error");
        snprintf(j->msg, sizeof j->msg, "operator refused: %s",
                 cJSON_IsString(v) ? v->valuestring : "no reason given");
        goto out;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(res, "rank")))
        j->rank = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(res, "stored_at")))
        j->stored_at = (uint32_t)v->valuedouble;
    j->new_record = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "record"));
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(res, "trophy")))
        j->item = (uint32_t)v->valuedouble;
    j->proven = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "score_proven"));
    rc = 0;
out:
    cJSON_Delete(res);
    free(body.data);
    free(req);
    return rc;
}

static int work_board(pdot_client *c, void *ud)
{
    struct job *j = ud;
    return board_read(c, j->ch->bul, j->ch->collection, &j->b, j->board_cid, j->msg,
                      sizeof j->msg);
}

static int work_close_funds(pdot_client *c, void *ud)
{
    struct job *j = ud;
    pdot_transfer t;

    memset(&t, 0, sizeof t);
    t.kind = PDOT_TRANSFER_ALL;
    memcpy(t.dest, j->ch->player.account_id, 32);
    return pdot_transfer_submit(c, &j->ch->session, &t, NULL, &j->tx);
}

static int work_close_proxy(pdot_client *c, void *ud)
{
    struct job *j = ud;
    return pdot_proxy_remove(c, &j->ch->player, j->ch->session.account_id, PROXY_TYPE, &j->tx);
}

/* ---- done: runs inside chain_poll ------------------------------------ */

static void show_balance(chain *ch)
{
    char a[40];

    if (!ch->has_balance || !ch->v.symbol[0])
        return;  /* the token is known once the session is open */
    amount(ch, ch->session_free, a, sizeof a);
    snprintf(ch->v.session_balance, sizeof ch->v.session_balance, "%s %s", a, ch->v.symbol);
}

static struct job *queue(chain *ch, int kind, uint32_t score, const char *name);
static struct job *queue_at(chain *ch, int kind, uint32_t score, const char *name,
                            uint32_t block, const char *cid, uint8_t *replay, size_t replay_len);

static void job_free(struct job *j)
{
    pdot_signer_wipe(&j->fresh);
    free(j->replay);
    free(j);
}
static void on_session_balance(void *ud, int rc, const char *error, const void *result);

static void on_owned(void *ud, int rc, const char *error, const void *result)
{
    chain *ch = ud;
    const pdot_nft_list *l = result;

    ch->v.busy--;
    if (rc == 0)
        ch->v.trophies = (int)l->n;
    else if (rc != PDOT_E_CANCELLED)
        fprintf(stderr, "[chain] trophies: %s\n", error);
}

static void refresh_trophies(chain *ch)
{
    if (pdot_async_nft_owned(ch->a, ch->player.account_id, ch->collection, 256, on_owned, ch))
        ch->v.busy++;
}

static void job_done(void *ud, int rc, const char *error, const void *result)
{
    struct job *j = ud;
    chain *ch = j->ch;
    char tx[128];

    (void)result;
    ch->v.busy--;
    if (rc == PDOT_E_CANCELLED) {
        /* Quit before the session closed: say what may be left on chain. */
        if (j->kind == J_OPEN || j->kind == J_CLOSE_FUNDS || j->kind == J_CLOSE_PROXY)
            fprintf(stderr, "[chain] quit before the session closed: the next run resumes "
                    "or cleans it up from %s\n", ch->session_file);
        job_free(j);
        return;
    }
    if (j->kind == J_OPEN && rc == 0) {
        memcpy(ch->v.symbol, j->symbol, sizeof ch->v.symbol);
        ch->v.decimals = j->decimals;
        if (!j->resumed)  /* the worker is idle: nothing else runs until the session is open */
            ch->session = j->fresh;
        pdot_ss58_encode(ch->session.account_id, 42, ch->v.session, sizeof ch->v.session);
        pdot_async_watch_balance(ch->a, ch->session.account_id, on_session_balance, ch);
    }
    pdot_signer_wipe(&j->fresh);
    describe_tx(ch, &j->tx, tx, sizeof tx);
    switch (j->kind) {
    case J_OPEN:
        if (rc) {
            ch->v.state = ch->close_pending ? CHAIN_CLOSED : CHAIN_FAILED;
            if (j->save_failed)
                status(ch, 1, "cannot write the session file %s", ch->session_file);
            else
                status(ch, 1, "session key not added: %s",
                       rc == PDOT_E_DISPATCH ? j->tx.error : error);
            break;
        }
        ch->v.state = CHAIN_READY;
        show_balance(ch);
        if (j->resumed)
            status(ch, 0, "session resumed from the last run: no signature needed");
        else if (j->cleaned)
            status(ch, 0, "last session cleaned up; new session key ready (%s)", tx);
        else
            status(ch, 0, "session key ready (%s)", tx);
        if (ch->close_pending) {
            chain_close(ch);
            break;
        }
        queue(ch, J_RECORD, 0, NULL);
        if (ch->bul)
            queue(ch, J_BOARD, 0, NULL);
        refresh_trophies(ch);
        break;
    case J_RECORD:
        if (rc) {
            status(ch, 1, "record not read: %s", error);
            break;
        }
        ch->v.has_record = j->has_record;
        ch->v.record = j->record;
        ch->v.record_verified = j->record_verified;
        memcpy(ch->v.record_holder, j->holder, sizeof ch->v.record_holder);
        memcpy(ch->v.record_name, j->holder_name, sizeof ch->v.record_name);
        break;
    case J_QUOTE:
        if (rc) {
            snprintf(ch->v.fee, sizeof ch->v.fee, "unknown");
            status(ch, 1, "fee: %s", error);
        } else {
            char a[40];
            amount(ch, j->fee.partial_fee, a, sizeof a);
            snprintf(ch->v.fee, sizeof ch->v.fee, "%s %s", a, ch->v.symbol);
        }
        break;
    case J_SUBMIT:
        if (rc) {
            status(ch, 1, "score not recorded: %s", rc == PDOT_E_DISPATCH ? j->tx.error : error);
            break;
        }
        status(ch, 0, "%s's score %u recorded: %s", j->name, j->score, tx);
        /* The operator decides on the newest board; a stale one only gates the claim. */
        if (ch->operator_url[0] &&
            (ch->v.board_state != 1 || board_rank(&ch->v.board, j->score) ||
             !ch->v.has_record || j->score > ch->v.record)) {
            status(ch, 0, "%s's score %u recorded (%s); claiming it with the operator...",
                   j->name, j->score, tx);
            queue_at(ch, J_CLAIM, j->score, j->name, j->tx.block, j->replay_cid, j->replay,
                     j->replay_len);
            j->replay = NULL;  /* the claim owns it now */
        }
        break;
    case J_CLAIM:
        if (rc) {
            status(ch, 1, "score not claimed: %s", j->msg[0] ? j->msg : error);
            break;
        }
        if (j->new_record)
            status(ch, 0, "new record! trophy %u minted%s%s", j->item,
                   j->rank ? ", and on the leaderboard" : "",
                   j->proven ? " (score proven by the operator)" : "");
        else if (j->rank)
            status(ch, 0, "%s is number %d on the leaderboard (Bulletin block %u)%s", j->name,
                   j->rank, j->stored_at, j->proven ? ", score proven by the operator" : "");
        else
            status(ch, 0, "the top 10 moved on: %u no longer makes it", j->score);
        /* Read back from the chain, not from the operator's answer. */
        queue(ch, J_RECORD, 0, NULL);
        if (ch->bul)
            queue(ch, J_BOARD, 0, NULL);
        refresh_trophies(ch);
        break;
    case J_BOARD:
        if (rc) {
            ch->v.board_state = -1;
            status(ch, 1, "leaderboard not read: %s", j->msg[0] ? j->msg : error);
            break;
        }
        ch->v.board = j->b;
        ch->v.board_state = 1;
        memcpy(ch->v.board_cid, j->board_cid, sizeof ch->v.board_cid);
        break;
    case J_CLOSE_FUNDS:
        ch->funds_returned = rc == 0;
        if (rc)
            status(ch, 1, "funds not returned: %s", rc == PDOT_E_DISPATCH ? j->tx.error : error);
        else
            status(ch, 0, "leftover funds returned (%s)", tx);
        queue(ch, J_CLOSE_PROXY, 0, NULL);
        break;
    case J_CLOSE_PROXY:
        if (rc) {
            status(ch, 1, "proxy not removed: %s", rc == PDOT_E_DISPATCH ? j->tx.error : error);
        } else {
            status(ch, 0, "session closed (%s)", tx);
            if (ch->funds_returned)  /* else keep the key: the next run returns the funds */
                remove(ch->session_file);
        }
        ch->v.state = CHAIN_CLOSED;
        break;
    }
    job_free(j);
}

static struct job *queue(chain *ch, int kind, uint32_t score, const char *name)
{
    return queue_at(ch, kind, score, name, 0, NULL, NULL, 0);
}

/* replay, when given, is owned by the job from here on, queued or not. */
static struct job *queue_at(chain *ch, int kind, uint32_t score, const char *name,
                            uint32_t block, const char *cid, uint8_t *replay, size_t replay_len)
{
    static int (*const work[])(pdot_client *, void *) = {
        work_open, work_record, work_quote, work_submit, work_claim, work_board,
        work_close_funds, work_close_proxy,
    };
    struct job *j = calloc(1, sizeof *j);

    if (!j) {
        free(replay);
        status(ch, 1, "out of memory");
        return NULL;
    }
    j->ch = ch;
    j->kind = kind;
    j->score = score;
    j->block = block;
    snprintf(j->name, sizeof j->name, "%s", name ? name : "");
    snprintf(j->replay_cid, sizeof j->replay_cid, "%s", cid ? cid : "");
    j->replay = replay;
    j->replay_len = replay_len;
    if (!pdot_async_run(ch->a, work[kind], job_done, j)) {
        status(ch, 1, "could not queue a job");
        job_free(j);
        return NULL;
    }
    ch->v.busy++;
    return j;
}

static void on_head(void *ud, int rc, const char *error, const void *result)
{
    chain *ch = ud;
    const pdot_final_block *b = result;

    (void)error;
    if (rc == 0) {
        ch->v.head = b->number;
        ch->v.head_verified = b->verified;
    }
}

static void on_session_balance(void *ud, int rc, const char *error, const void *result)
{
    chain *ch = ud;
    const pdot_balance *b = result;

    (void)error;
    if (rc == 0) {
        ch->session_free = b->free;
        ch->has_balance = 1;
        show_balance(ch);
    }
}

static void save_checkpoint(void *ud, const uint8_t *blob, size_t len)
{
    chain *ch = ud;
    FILE *f = fopen(ch->checkpoint, "wb");

    if (f) {
        fwrite(blob, 1, len, f);
        fclose(f);
    }
}

static int set_verifier(pdot_client *c, const chain_config *cfg)
{
    FILE *f = fopen(cfg->checkpoint, "rb");
    uint8_t buf[1 << 16];
    size_t len;
    int rc;

    if (!f)
        return PDOT_E_NOTFOUND;
    len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    rc = pdot_client_set_verifier(c, cfg->relay, cfg->para_id, buf, len);
    return rc;
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

/* ---- public ---------------------------------------------------------- */

chain *chain_open(const chain_config *cfg, char *err, size_t err_len)
{
    chain *ch = calloc(1, sizeof *ch);
    pdot_client *c = NULL;
    int rc;

    if (!ch) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    ch->collection = cfg->collection;
    snprintf(ch->fund, sizeof ch->fund, "%s", cfg->fund);
    ch->v.trophies = -1;
    snprintf(ch->v.session_balance, sizeof ch->v.session_balance, "...");
    if ((rc = pdot_signer_sr25519_from_seed(&ch->player, cfg->player_seed))) {
        snprintf(err, err_len, "keys: %s", pdot_strerror(rc));
        goto fail;
    }
    pdot_ss58_encode(ch->player.account_id, 42, ch->v.player, sizeof ch->v.player);
    snprintf(ch->session_file, sizeof ch->session_file, "%s", cfg->session_file);
    if (cfg->operator_url) {
        size_t n = strlen(cfg->operator_url);
        while (n && cfg->operator_url[n - 1] == '/')
            n--;  /* "/claim" is appended */
        snprintf(ch->operator_url, sizeof ch->operator_url, "%.*s", (int)n, cfg->operator_url);
        ch->v.has_operator = 1;
    }
    ch->has_saved = load_session(ch);

    if (!(c = pdot_client_new(cfg->rpc, NULL))) {
        snprintf(err, err_len, "cannot create a client for %s", cfg->rpc);
        goto fail;
    }
    if ((rc = set_metadata_hash(c, cfg->metadata_hash))) {
        snprintf(err, err_len, "metadata hash: %s", pdot_last_error(c));
        goto fail;
    }
    if (cfg->relay && cfg->checkpoint) {
        if ((rc = set_verifier(c, cfg))) {
            snprintf(err, err_len, "verifier: %s", pdot_last_error(c));
            goto fail;
        }
        snprintf(ch->checkpoint, sizeof ch->checkpoint, "%s", cfg->checkpoint);
        ch->v.proven = 1;
    }
    if (cfg->bulletin_rpc) {
        if (!(ch->bul = pdot_client_new(cfg->bulletin_rpc, NULL))) {
            snprintf(err, err_len, "cannot create a client for %s", cfg->bulletin_rpc);
            goto fail;
        }
        pdot_client_set_timeout(ch->bul, 30000);  /* chain_free waits for a running call */
        if (set_metadata_hash(ch->bul, cfg->metadata_hash)) {
            snprintf(err, err_len, "metadata hash (Bulletin): %s", pdot_last_error(ch->bul));
            goto fail;
        }
        ch->v.has_bulletin = 1;
    }
    if (!(ch->a = pdot_async_new(c))) {
        snprintf(err, err_len, "out of memory");
        goto fail;
    }
    c = NULL;  /* owned by ch->a */
    if (ch->v.proven)
        pdot_async_set_checkpoint_fn(ch->a, save_checkpoint, ch);
    pdot_async_watch_head(ch->a, on_head, ch);
    status(ch, 0, ch->has_saved ? "checking the session saved by the last run"
                                : "opening a session: the player adds a session key");
    queue(ch, J_OPEN, 0, NULL);
    return ch;

fail:
    if (c)
        pdot_client_free(c);
    if (ch->bul)
        pdot_client_free(ch->bul);
    pdot_signer_wipe(&ch->player);
    pdot_signer_wipe(&ch->session);
    free(ch);
    return NULL;
}

void chain_poll(chain *ch)
{
    pdot_async_poll(ch->a);
}

const chain_view *chain_view_get(const chain *ch)
{
    return &ch->v;
}

/* 3 letters A-Z: they go into JSON and on chain as they are. */
static int valid_name(const char *name)
{
    int i;
    for (i = 0; i < 3; i++)
        if (name[i] < 'A' || name[i] > 'Z')
            return 0;
    return name[3] == '\0';
}

void chain_set_replay(chain *ch, const replay *r, uint32_t score)
{
    pdot_cid cid;

    free(ch->replay);
    ch->replay = replay_encode(r, &ch->replay_len);
    ch->replay_cid[0] = '\0';
    ch->score = score;
    if (!ch->replay)
        status(ch, 1, "this game is too long to replay: its score cannot be claimed");
    else if (pdot_cid_compute(&cid, ch->replay, ch->replay_len) ||
             pdot_cid_to_string(&cid, ch->replay_cid, sizeof ch->replay_cid))
        ch->replay_cid[0] = '\0';
    if (ch->v.state != CHAIN_READY)
        return;
    ch->v.fee[0] = '\0';
    /* any initials: the same length, the same fee */
    queue_at(ch, J_QUOTE, score, "AAA", 0, ch->replay_cid, NULL, 0);
}

void chain_submit(chain *ch, const char *name)
{
    uint8_t *copy = NULL;

    if (ch->v.state != CHAIN_READY || !valid_name(name))
        return;
    if (ch->replay && (copy = malloc(ch->replay_len)))
        memcpy(copy, ch->replay, ch->replay_len);
    status(ch, 0, "submitting %s's score %u...", name, ch->score);
    queue_at(ch, J_SUBMIT, ch->score, name, 0, copy ? ch->replay_cid : "", copy,
             copy ? ch->replay_len : 0);
}

void chain_close(chain *ch)
{
    if (ch->v.state == CHAIN_OPENING) {
        ch->close_pending = 1;  /* closes once the session key is in */
        status(ch, 0, "closing once the session key is in...");
        return;
    }
    if (ch->v.state != CHAIN_READY) {
        ch->v.state = CHAIN_CLOSED;  /* no session key on chain */
        return;
    }
    ch->v.state = CHAIN_CLOSING;
    status(ch, 0, "closing the session...");
    queue(ch, J_CLOSE_FUNDS, 0, NULL);
}

void chain_free(chain *ch)
{
    if (!ch)
        return;
    pdot_async_free(ch->a);  /* joins the worker, the only user of ch->bul */
    free(ch->replay);
    if (ch->bul)
        pdot_client_free(ch->bul);
    pdot_signer_wipe(&ch->player);
    pdot_signer_wipe(&ch->session);
    free(ch);
}
