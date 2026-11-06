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
 *           System.remark_with_event("pdot-asteroids score=N name=ABC")
 *   trophy  on a new record the operator mints an NFT to the player and
 *           updates the collection's "best" attribute: "SCORE ADDRESS ABC"
 *   board   reads the leaderboard: the collection's "board" attribute holds
 *           the CID of a JSON document on the Bulletin chain (board.h),
 *           fetched and checked against the CID
 *   board+  a score that makes the top 10: the operator merges it into the
 *           newest board, stores that on Bulletin and points "board" at it
 *   close   leftover funds back to the player, then the player removes the proxy
 *
 * Watches keep the newest final block and the session key's balance current.
 */
#include "chain.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define PROXY_TYPE "NonTransfer"  /* the session key can do anything but move funds */
#define RECORD_KEY "best"
#define BOARD_KEY  "board"

struct chain {
    pdot_async  *a;
    pdot_client *bul;            /* Bulletin, used only inside jobs on a's worker */
    pdot_signer  player, op, session;
    int          has_saved;      /* session holds the key saved by an earlier run */
    char         session_file[512];
    int          funds_returned; /* closing: leftover funds went back */
    uint32_t     collection;
    char         fund[32];
    char         checkpoint[512];
    int          close_pending;  /* chain_close came while the session was opening */
    int          has_balance;    /* session_free read, shown once decimals are known */
    pdot_u128    session_free;
    chain_view   v;
};

enum { J_OPEN, J_RECORD, J_QUOTE, J_SUBMIT, J_TROPHY, J_BOARD, J_BOARD_ADD, J_CLOSE_FUNDS,
       J_CLOSE_PROXY };

struct job {
    chain         *ch;
    int            kind;
    uint32_t       score;
    char           name[4];     /* the player's initials, A-Z */
    pdot_tx_result tx;
    pdot_fee       fee;
    uint32_t       item;
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
    uint32_t       block;       /* board+: the block holding the score's remark */
    board          b;           /* board, board+: the leaderboard read (and updated) */
    char           board_cid[CHAIN_CID_STR_MAX];
    int            rank;        /* board+: the place taken, 0 for none */
    uint32_t       stored_at;   /* board+: the Bulletin block holding the new board */
    char           msg[192];    /* a Bulletin error (the done callback only sees Asset Hub's) */
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

static int remark_call(pdot_client *c, uint32_t score, const char *name, uint8_t **call,
                       size_t *len)
{
    char args[80];
    snprintf(args, sizeof args, "[\"pdot-asteroids score=%u name=%s\"]", score, name);
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
    pdot_nft_attribute attr;
    char text[PDOT_NFT_DATA_MAX + 1];
    unsigned long score;
    int rc;

    rc = pdot_nft_attribute_get(c, j->ch->collection, 0, 0, PDOT_NFT_NS_COLLECTION_OWNER, NULL,
                                (const uint8_t *)RECORD_KEY, strlen(RECORD_KEY), &attr);
    if (rc || !attr.exists)
        return rc;
    memcpy(text, attr.value, attr.len);
    text[attr.len] = '\0';
    /* "SCORE ADDRESS NAME", as work_trophy writes it; older records have no NAME */
    if (sscanf(text, "%lu %63s %3[A-Z]", &score, j->holder, j->holder_name) >= 2) {
        j->has_record = 1;
        j->record = (uint32_t)score;
        j->record_verified = attr.at.verified;
    }
    return 0;
}

static int work_quote(pdot_client *c, void *ud)
{
    struct job *j = ud;
    pdot_tx_options o = { { 0, 0 }, j->ch->player.account_id };
    uint8_t *call;
    size_t len;
    int rc;

    if ((rc = remark_call(c, j->score, j->name, &call, &len)))
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

    if ((rc = remark_call(c, j->score, j->name, &call, &len)))
        return rc;
    rc = pdot_tx_submit(c, &j->ch->session, call, len, &o, &j->tx);
    pdot_free(call);
    return rc;
}

/* Encodes pallet.call from JSON and submits it signed by the operator. */
static int op_submit(pdot_client *c, chain *ch, const char *call_name, const char *args,
                     pdot_tx_result *tx)
{
    uint8_t *call;
    size_t len;
    int rc;

    if ((rc = pdot_call_from_json(c, "Nfts", call_name, args, &call, &len)))
        return rc;
    rc = pdot_tx_submit(c, &ch->op, call, len, NULL, tx);
    pdot_free(call);
    return rc;
}

static int work_trophy(pdot_client *c, void *ud)
{
    struct job *j = ud;
    chain *ch = j->ch;
    pdot_nft_item it;
    char args[256];
    int rc;

    /* The first free item id. */
    for (j->item = 0;; j->item++) {
        if ((rc = pdot_nft_item_get(c, ch->collection, j->item, &it)))
            return rc;
        if (!it.exists)
            break;
    }
    snprintf(args, sizeof args, "[%u, %u, \"%s\", null]", ch->collection, j->item, ch->v.player);
    if ((rc = op_submit(c, ch, "mint", args, &j->tx)))
        return rc;
    snprintf(args, sizeof args, "[%u, %u, \"pdot-asteroids record %u by %s\"]", ch->collection,
             j->item, j->score, j->name);
    if ((rc = op_submit(c, ch, "set_metadata", args, &j->tx)))
        return rc;
    snprintf(args, sizeof args, "[%u, null, \"CollectionOwner\", \"" RECORD_KEY "\", \"%u %s %s\"]",
             ch->collection, j->score, ch->v.player, j->name);
    return op_submit(c, ch, "set_attribute", args, &j->tx);
}

/* The leaderboard behind the "board" attribute; an empty one if there is none yet. */
static int read_board(pdot_client *c, struct job *j)
{
    chain *ch = j->ch;
    pdot_nft_attribute attr;
    pdot_cid cid;
    uint8_t *data;
    size_t len;
    int rc;

    memset(&j->b, 0, sizeof j->b);
    j->board_cid[0] = '\0';
    rc = pdot_nft_attribute_get(c, ch->collection, 0, 0, PDOT_NFT_NS_COLLECTION_OWNER, NULL,
                                (const uint8_t *)BOARD_KEY, strlen(BOARD_KEY), &attr);
    if (rc || !attr.exists)
        return rc;
    if (attr.len >= sizeof j->board_cid) {
        snprintf(j->msg, sizeof j->msg, "the board attribute is not a CID");
        return PDOT_E_DECODE;
    }
    memcpy(j->board_cid, attr.value, attr.len);
    j->board_cid[attr.len] = '\0';
    if ((rc = pdot_cid_from_string(&cid, j->board_cid))) {
        snprintf(j->msg, sizeof j->msg, "the board attribute is not a CID");
        return rc;
    }
    /* Checked against the CID: a node can withhold the board, not change it. */
    if ((rc = pdot_bulletin_fetch(ch->bul, &cid, &data, &len))) {
        snprintf(j->msg, sizeof j->msg, "fetching the board from Bulletin: %s",
                 pdot_last_error(ch->bul));
        return rc;
    }
    rc = board_parse(data, len, &j->b) ? PDOT_E_DECODE : 0;
    if (rc)
        snprintf(j->msg, sizeof j->msg, "%.60s is not a leaderboard", j->board_cid);
    pdot_free(data);
    return rc;
}

static int work_board(pdot_client *c, void *ud)
{
    return read_board(c, ud);
}

static int work_board_add(pdot_client *c, void *ud)
{
    struct job *j = ud;
    chain *ch = j->ch;
    pdot_store_result sr;
    board_entry e;
    char args[256], *json;
    int rc;

    /* Merged into the newest board, which another game may have changed. */
    if ((rc = read_board(c, j)))
        return rc;
    memset(&e, 0, sizeof e);
    e.score = j->score;
    memcpy(e.name, j->name, sizeof e.name);
    memcpy(e.account, ch->v.player, sizeof e.account);
    e.block = j->block;
    if (!(j->rank = board_insert(&j->b, &e)))
        return 0;
    if (!(json = board_json(&j->b)))
        return PDOT_E_NOMEM;
    rc = pdot_bulletin_store(ch->bul, &ch->op, (const uint8_t *)json, strlen(json), &sr);
    free(json);
    if (rc) {
        snprintf(j->msg, sizeof j->msg, "storing the board on Bulletin: %s",
                 rc == PDOT_E_DISPATCH ? sr.tx.error : pdot_last_error(ch->bul));
        return rc;
    }
    j->stored_at = sr.tx.block;
    if ((rc = pdot_cid_to_string(&sr.cid, j->board_cid, sizeof j->board_cid)))
        return rc;
    snprintf(args, sizeof args, "[%u, null, \"CollectionOwner\", \"" BOARD_KEY "\", \"%s\"]",
             ch->collection, j->board_cid);
    return op_submit(c, ch, "set_attribute", args, &j->tx);
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
                            uint32_t block);
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
        pdot_signer_wipe(&j->fresh);
        free(j);
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
        /* The worker merges into the newest board; a stale one only gates the try. */
        if (ch->bul && (ch->v.board_state != 1 || board_rank(&ch->v.board, j->score)))
            queue_at(ch, J_BOARD_ADD, j->score, j->name, j->tx.block);
        if (!ch->v.has_record || j->score > ch->v.record) {
            ch->v.has_record = 1;  /* shown at once; reread after the trophy */
            ch->v.record = j->score;
            memcpy(ch->v.record_holder, ch->v.player, sizeof ch->v.record_holder);
            memcpy(ch->v.record_name, j->name, sizeof ch->v.record_name);
            ch->v.record_verified = 0;
            queue(ch, J_TROPHY, j->score, j->name);
        }
        break;
    case J_TROPHY:
        if (rc)
            status(ch, 1, "trophy not minted: %s", rc == PDOT_E_DISPATCH ? j->tx.error : error);
        else
            status(ch, 0, "new record! trophy %u minted (%s)", j->item, tx);
        queue(ch, J_RECORD, 0, NULL);
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
    case J_BOARD_ADD:
        if (rc) {
            status(ch, 1, "leaderboard not updated: %s",
                   j->msg[0] ? j->msg : rc == PDOT_E_DISPATCH ? j->tx.error : error);
            break;
        }
        ch->v.board = j->b;
        ch->v.board_state = 1;
        memcpy(ch->v.board_cid, j->board_cid, sizeof ch->v.board_cid);
        if (j->rank)
            status(ch, 0, "%s is number %d on the leaderboard (stored on Bulletin block %u)",
                   j->name, j->rank, j->stored_at);
        else
            status(ch, 0, "the top 10 moved on: %u no longer makes it", j->score);
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
    free(j);
}

static struct job *queue(chain *ch, int kind, uint32_t score, const char *name)
{
    return queue_at(ch, kind, score, name, 0);
}

static struct job *queue_at(chain *ch, int kind, uint32_t score, const char *name,
                            uint32_t block)
{
    static int (*const work[])(pdot_client *, void *) = {
        work_open, work_record, work_quote, work_submit, work_trophy, work_board,
        work_board_add, work_close_funds, work_close_proxy,
    };
    struct job *j = calloc(1, sizeof *j);

    if (!j) {
        status(ch, 1, "out of memory");
        return NULL;
    }
    j->ch = ch;
    j->kind = kind;
    j->score = score;
    j->block = block;
    snprintf(j->name, sizeof j->name, "%s", name ? name : "");
    if (!pdot_async_run(ch->a, work[kind], job_done, j)) {
        status(ch, 1, "could not queue a job");
        free(j);
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
    if ((rc = pdot_signer_sr25519_from_seed(&ch->player, cfg->player_seed)) ||
        (rc = pdot_signer_sr25519_from_seed(&ch->op, cfg->operator_seed))) {
        snprintf(err, err_len, "keys: %s", pdot_strerror(rc));
        goto fail;
    }
    pdot_ss58_encode(ch->player.account_id, 42, ch->v.player, sizeof ch->v.player);
    snprintf(ch->session_file, sizeof ch->session_file, "%s", cfg->session_file);
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
    pdot_signer_wipe(&ch->op);
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

void chain_quote(chain *ch, uint32_t score)
{
    if (ch->v.state != CHAIN_READY)
        return;
    ch->v.fee[0] = '\0';
    queue(ch, J_QUOTE, score, "AAA");  /* any initials: the same length, the same fee */
}

void chain_submit(chain *ch, uint32_t score, const char *name)
{
    if (ch->v.state != CHAIN_READY || !valid_name(name))
        return;
    status(ch, 0, "submitting %s's score %u...", name, score);
    queue(ch, J_SUBMIT, score, name);
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
    if (ch->bul)
        pdot_client_free(ch->bul);
    pdot_signer_wipe(&ch->player);
    pdot_signer_wipe(&ch->op);
    pdot_signer_wipe(&ch->session);
    free(ch);
}
