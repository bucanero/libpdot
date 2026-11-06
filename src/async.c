/*
 * pdot_async: a worker thread running blocking calls in order, a watch
 * thread reading values at each new final block, and a queue of finished
 * work that pdot_async_poll hands to callbacks on the caller's thread.
 *
 * Locking: everything in struct pdot_async below `lock` is guarded by it.
 * The worker owns `c`, the watch thread owns `sc` and the per-watch reading
 * state; neither client is touched by any other thread.
 *
 * Checkpoints: each client verifies on its own connection, so each advances
 * its own verifier. Around every job and every watch round the thread
 * publishes its progress to `ck` if it got further, and first catches up
 * from `ck` if the other thread did, so authority-set changes are walked
 * once and the app can save one checkpoint.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pdot.h"
#include "assets.h"
#include "client.h"
#include "storage.h"
#include "thread.h"
#include "verify.h"
#include "ws.h"

#define SLICE_MS        50     /* how often waits check for new work or shutdown */
#define WS_HEARTBEAT_MS 30000  /* read anyway this often while subscribed */
#define WS_RETRY_S      30     /* after a WebSocket failure, poll this long first */

enum { IT_JOB, IT_PROGRESS, IT_EVENT };

union result {
    pdot_store_result   store;
    pdot_chunked_result chunked;
    pdot_tx_result      tx;
    pdot_storage_proof  proof;
    pdot_authorization  auth;
    pdot_bytes          bytes;
    pdot_balance        balance;
    pdot_asset_balance  asset_balance;
    pdot_asset_info     asset_info;
    pdot_nft_item       nft;
    pdot_nft_list       nfts;
    pdot_final_block    head;
    pdot_storage_value  storage;
    pdot_fee            fee;
    pdot_proxies        proxies;
};

struct item;
typedef int (*work_fn)(pdot_client *c, struct item *it);

/* A job, a progress report or a watch event, on its way to a callback. */
struct item {
    struct item      *next;
    int               kind;
    uint64_t          id;
    pdot_result_fn    done;
    pdot_progress_fn  progress;
    void             *ud;
    int               rc;
    char              err[256];
    int               has_result;
    union result      res;
    void             *owned;      /* result memory, freed after the callback */
    struct pdot_async *a;

    /* Job input. */
    work_fn           work;
    int             (*user_work)(pdot_client *c, void *ud);
    pdot_signer       signer;
    uint8_t          *data;
    size_t            len, chunk_size, max;
    pdot_cid          cid;
    uint8_t           account[32];
    pdot_query        q;
    pdot_tx_options   opts;       /* proxy_for points at proxy_for below */
    uint8_t           proxy_for[32];
    pdot_transfer     transfer;

    /* Progress report. */
    size_t            index, total;
    pdot_tx_result    tx;
};

struct watch {
    struct watch  *next;
    uint64_t       id;
    int            head;     /* reports final blocks rather than a value */
    pdot_query     q;
    uint8_t       *bytes;    /* the query's key bytes, owned */
    pdot_result_fn fn;
    void          *ud;
    int            removed;  /* under lock */

    /* Watch thread only. */
    pdot_buf       keys[PDOT_Q_MAX_KEYS];
    size_t         nkeys;
    int            have_keys;
    uint32_t       keys_spec;  /* runtime the keys were built for */
    int            reported;   /* a value was reported since the last failure */
    pdot_buf       last;       /* what was reported: presence | length | bytes per key */
    int            failing;
};

struct pdot_async {
    pdot_client  *c;   /* worker thread */
    pdot_client  *sc;  /* watch thread */
    pdot_thread   worker, watcher;
    int           worker_on, watcher_on;

    pdot_mutex    lock;
    pdot_cond     cond;  /* the worker waits on it for jobs */
    int           shutdown;
    struct item  *queue, *queue_tail;
    uint64_t      running;
    int           cancel_running;
    struct item  *done, *done_tail;
    struct watch *watches;
    int           dirty;  /* a watch was added or removed */
    uint64_t      next_id;
    void        (*wake)(void *ud);
    void         *wake_ud;

    /* The furthest verifier checkpoint either thread reached; NULL: no verifier. */
    uint8_t      *ck;
    size_t        ck_len;
    uint64_t      ck_set;
    uint32_t      ck_anchor;
    int           ck_new;  /* advanced since ck_fn last ran */
    void        (*ck_fn)(void *ud, const uint8_t *blob, size_t len);
    void         *ck_ud;
};

/* ---- Items ---------------------------------------------------------------- */

static struct item *new_item(pdot_async *a, int kind, pdot_result_fn done, void *ud)
{
    struct item *it = calloc(1, sizeof *it);

    if (it) {
        it->a = a;
        it->kind = kind;
        it->done = done;
        it->ud = ud;
    }
    return it;
}

static void free_item(struct item *it)
{
    pdot_signer_wipe(&it->signer);
    free(it->data);
    free(it->owned);
    free(it);
}

/* Queues it for pdot_async_poll and wakes the caller's loop. */
static void finish(pdot_async *a, struct item *it)
{
    void (*wake)(void *);
    void *wake_ud;

    pdot_mutex_lock(&a->lock);
    if (a->done_tail)
        a->done_tail->next = it;
    else
        a->done = it;
    a->done_tail = it;
    wake = a->wake;
    wake_ud = a->wake_ud;
    pdot_mutex_unlock(&a->lock);
    if (wake)
        wake(wake_ud);
}

static void set_error(struct item *it, int rc, const pdot_client *c)
{
    it->rc = rc;
    if (rc == PDOT_E_CANCELLED)
        strcpy(it->err, "cancelled");
    else if (c && c->err[0])
        snprintf(it->err, sizeof it->err, "%s", c->err);
    else
        snprintf(it->err, sizeof it->err, "%s", pdot_strerror(rc));
}

static void deliver(struct item *it)
{
    if (it->kind == IT_PROGRESS) {
        it->progress(it->ud, it->index, it->total, &it->cid, &it->tx, it->rc);
        return;
    }
    if (it->done)
        it->done(it->ud, it->rc, it->rc ? it->err : "", it->has_result ? &it->res : NULL);
}

/* ---- Worker --------------------------------------------------------------- */

/* ---- Checkpoints ---------------------------------------------------------- */

/* Moves c's verifier up to the shared checkpoint if the other thread got further. */
static void ck_catch_up(pdot_async *a, pdot_client *c)
{
    uint8_t *ck = NULL;
    size_t len = 0;

    if (!c->verifier)
        return;
    pdot_mutex_lock(&a->lock);
    if (a->ck && pdot_verifier_newer(a->ck_set, a->ck_anchor, c->verifier->set.set_id,
                                     c->verifier->anchor_number) &&
        (ck = malloc(a->ck_len)) != NULL) {
        memcpy(ck, a->ck, a->ck_len);
        len = a->ck_len;
    }
    pdot_mutex_unlock(&a->lock);
    if (ck) {
        pdot_verifier_adopt(c, ck, len);
        free(ck);
    }
}

/* Publishes c's verifier progress as the shared checkpoint if it is further along. */
static void ck_publish(pdot_async *a, pdot_client *c)
{
    const pdot_verifier *v = c->verifier;
    void (*wake)(void *) = NULL;
    void *wake_ud = NULL;
    uint8_t *ck;
    size_t len;
    int newer;

    if (!v)
        return;
    pdot_mutex_lock(&a->lock);
    newer = !a->ck || pdot_verifier_newer(v->set.set_id, v->anchor_number, a->ck_set, a->ck_anchor);
    pdot_mutex_unlock(&a->lock);
    if (!newer || pdot_client_checkpoint(c, &ck, &len))
        return;
    pdot_mutex_lock(&a->lock);
    if (!a->ck || pdot_verifier_newer(v->set.set_id, v->anchor_number, a->ck_set, a->ck_anchor)) {
        a->ck_new = a->ck != NULL;  /* an advance, not the starting point */
        free(a->ck);
        a->ck = ck;
        a->ck_len = len;
        a->ck_set = v->set.set_id;
        a->ck_anchor = v->anchor_number;
        wake = a->ck_new && a->ck_fn ? a->wake : NULL;
        wake_ud = a->wake_ud;
        ck = NULL;
    }
    pdot_mutex_unlock(&a->lock);
    free(ck);
    if (wake)
        wake(wake_ud);
}

/* ---- Worker thread --------------------------------------------------------- */

static int worker_stop(void *ud)
{
    pdot_async *a = ud;
    int stop;

    pdot_mutex_lock(&a->lock);
    stop = a->shutdown || a->cancel_running;
    pdot_mutex_unlock(&a->lock);
    return stop;
}

static void worker_main(void *arg)
{
    pdot_async *a = arg;

    pdot_mutex_lock(&a->lock);
    for (;;) {
        struct item *it;
        int rc;

        while (!a->queue && !a->shutdown)
            pdot_cond_wait(&a->cond, &a->lock);
        if (a->shutdown)
            break;
        it = a->queue;
        a->queue = it->next;
        if (!a->queue)
            a->queue_tail = NULL;
        it->next = NULL;
        a->running = it->id;
        a->cancel_running = 0;
        pdot_mutex_unlock(&a->lock);

        a->c->err[0] = '\0';
        ck_catch_up(a, a->c);
        rc = it->work(a->c, it);
        ck_publish(a, a->c);
        if (rc)
            set_error(it, rc, a->c);
        /* Jobs that may report a failed dispatch set has_result themselves. */
        if (!rc)
            it->has_result = 1;
        pdot_signer_wipe(&it->signer);
        free(it->data);
        it->data = NULL;

        pdot_mutex_lock(&a->lock);
        a->running = 0;
        pdot_mutex_unlock(&a->lock);
        finish(a, it);
        pdot_mutex_lock(&a->lock);
    }
    pdot_mutex_unlock(&a->lock);
}

static uint64_t submit(pdot_async *a, struct item *it)
{
    uint64_t id = 0;

    if (!it)
        return 0;
    pdot_mutex_lock(&a->lock);
    if (!a->shutdown && (a->worker_on || pdot_thread_start(&a->worker, worker_main, a) == 0)) {
        a->worker_on = 1;
        it->id = id = ++a->next_id;
        if (a->queue_tail)
            a->queue_tail->next = it;
        else
            a->queue = it;
        a->queue_tail = it;
        pdot_cond_broadcast(&a->cond);
    }
    pdot_mutex_unlock(&a->lock);
    if (!id)
        free_item(it);
    return id;
}

/* ---- Watch thread ---------------------------------------------------------- */

static int watcher_stop(void *ud)
{
    pdot_async *a = ud;
    int stop;

    pdot_mutex_lock(&a->lock);
    stop = a->shutdown;
    pdot_mutex_unlock(&a->lock);
    return stop;
}

static void free_watch(struct watch *w)
{
    size_t i;

    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_free(&w->keys[i]);
    pdot_buf_free(&w->last);
    free(w->bytes);
    free(w);
}

/* The final callback of a watch, then the watch itself. */
static void end_watch(pdot_async *a, struct watch *w)
{
    struct item *it = new_item(a, IT_EVENT, w->fn, w->ud);

    if (it) {
        set_error(it, PDOT_E_CANCELLED, NULL);
        finish(a, it);
    }
    free_watch(w);
}

static void report(pdot_async *a, struct watch *w, int rc, const char *err,
                   const union result *res, void *owned)
{
    struct item *it = new_item(a, IT_EVENT, w->fn, w->ud);

    if (!it) {
        free(owned);
        return;
    }
    if (rc) {
        it->rc = rc;
        snprintf(it->err, sizeof it->err, "%s", err && *err ? err : pdot_strerror(rc));
    } else {
        it->res = *res;
        it->has_result = 1;
        it->owned = owned;
    }
    finish(a, it);
}

/* One failure report per streak; the value is reported again afterwards. */
static void watch_failed(pdot_async *a, struct watch *w, int rc, const char *err)
{
    if (w->failing)
        return;
    w->failing = 1;
    w->reported = 0;
    report(a, w, rc, err, NULL, NULL);
}

/* Unlinks removed watches and gives them their final callback. */
static void reap(pdot_async *a)
{
    struct watch **pp, *gone = NULL, *w;

    pdot_mutex_lock(&a->lock);
    for (pp = &a->watches; *pp;) {
        w = *pp;
        if (w->removed) {
            *pp = w->next;
            w->next = gone;
            gone = w;
        } else {
            pp = &w->next;
        }
    }
    pdot_mutex_unlock(&a->lock);
    while ((w = gone)) {
        gone = w->next;
        end_watch(a, w);
    }
}

/* Snapshot of live watches; only this thread frees them, so they stay valid. */
static struct watch **snapshot(pdot_async *a, size_t *n)
{
    struct watch *w, **list;
    size_t count = 0;

    pdot_mutex_lock(&a->lock);
    for (w = a->watches; w; w = w->next)
        count++;
    list = count ? malloc(count * sizeof *list) : NULL;
    *n = 0;
    for (w = a->watches; list && w; w = w->next)
        if (!w->removed)
            list[(*n)++] = w;
    pdot_mutex_unlock(&a->lock);
    return list;
}

static int ensure_keys(pdot_client *c, struct watch *w)
{
    size_t i;
    int rc;

    if (w->have_keys && w->keys_spec == c->spec_version)
        return 0;
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++) {
        pdot_buf_free(&w->keys[i]);
        pdot_buf_init(&w->keys[i]);
    }
    if ((rc = pdot_query_keys(c, &w->q, w->keys, &w->nkeys)))
        return rc;
    w->have_keys = 1;
    w->keys_spec = c->spec_version;
    return 0;
}

/* What a watch last saw, to tell whether its value changed. */
static void fingerprint(const pdot_value *v, size_t n, pdot_buf *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pdot_scale_u8(out, (uint8_t)v[i].present);
        pdot_scale_u32(out, (uint32_t)v[i].len);
        pdot_scale_raw(out, v[i].data, v[i].len);
    }
}

/* Reads every value watch at `at` with one request (one proof with a verifier). */
static void read_values(pdot_async *a, struct watch **list, size_t n,
                        const pdot_final_block *at, const pdot_header *head)
{
    pdot_client *c = a->sc;
    const uint8_t **kp;
    size_t *kl, *off, total = 0, i, k;
    pdot_value *values = NULL;
    int rc = 0;

    for (i = 0; i < n; i++) {
        if (list[i]->head)
            continue;
        if ((rc = ensure_keys(c, list[i]))) {
            watch_failed(a, list[i], rc, c->err);
            list[i]->have_keys = 0;
            continue;
        }
        total += list[i]->nkeys;
    }
    if (!total)
        return;
    kp = malloc(total * sizeof *kp);
    kl = malloc(total * sizeof *kl);
    off = malloc(n * sizeof *off);
    values = calloc(total, sizeof *values);
    if (!kp || !kl || !off || !values) {
        free(kp);
        free(kl);
        free(off);
        free(values);
        return;
    }
    for (i = 0, total = 0; i < n; i++) {
        off[i] = total;
        if (list[i]->head || !list[i]->have_keys)
            continue;
        for (k = 0; k < list[i]->nkeys; k++) {
            kp[total] = list[i]->keys[k].data;
            kl[total++] = list[i]->keys[k].len;
        }
    }
    c->err[0] = '\0';
    if ((rc = pdot_storage_read_at(c, at, head, kp, kl, total, values))) {
        for (i = 0; i < n; i++)
            if (!list[i]->head && list[i]->have_keys)
                watch_failed(a, list[i], rc, c->err);
    } else {
        for (i = 0; i < n; i++) {
            struct watch *w = list[i];
            pdot_buf fp;
            union result res;

            if (w->head || !w->have_keys)
                continue;
            pdot_buf_init(&fp);
            fingerprint(&values[off[i]], w->nkeys, &fp);
            if (fp.err || (w->reported && fp.len == w->last.len &&
                           memcmp(fp.data, w->last.data, fp.len) == 0)) {
                pdot_buf_free(&fp);
                w->failing = 0;
                continue;  /* unchanged */
            }
            if ((rc = pdot_query_decode(c, &w->q, &values[off[i]], at, &res))) {
                pdot_buf_free(&fp);
                watch_failed(a, w, rc, c->err);
                continue;
            }
            pdot_buf_free(&w->last);
            w->last = fp;
            w->reported = 1;
            w->failing = 0;
            report(a, w, 0, NULL, &res, w->q.kind == PDOT_Q_STORAGE ? res.storage.data : NULL);
        }
    }
    pdot_values_free(values, total);
    free(values);
    free(kp);
    free(kl);
    free(off);
}

/* Reads the newest final block and reports what changed since the last one. */
static void tick(pdot_async *a, pdot_final_block *last, int *have_last, int force)
{
    pdot_client *c = a->sc;
    pdot_final_block at;
    const pdot_header *head;
    struct watch **list;
    size_t n, i;
    int rc, moved;

    if (!(list = snapshot(a, &n)))
        return;
    c->err[0] = '\0';
    if ((rc = pdot_chain_sync(c)) || (rc = pdot_final_block_get(c, &at, &head))) {
        for (i = 0; i < n; i++)
            watch_failed(a, list[i], rc, c->err);
        free(list);
        return;
    }
    moved = !*have_last || at.number != last->number || memcmp(at.hash, last->hash, 32) != 0;
    if (moved || force) {
        *last = at;
        *have_last = 1;
        for (i = 0; i < n; i++) {
            union result res;
            if (!list[i]->head || (!moved && list[i]->reported))
                continue;
            res.head = at;
            list[i]->reported = 1;
            list[i]->failing = 0;
            report(a, list[i], 0, NULL, &res, NULL);
        }
        read_values(a, list, n, &at, head);
    }
    free(list);
}

/* Until a new final block is announced, a watch changes, or it is time to poll. */
static void wait_trigger(pdot_async *a, pdot_ws **ws, time_t *ws_retry)
{
    unsigned waited = 0, limit = *ws ? WS_HEARTBEAT_MS : a->sc->poll_ms;

    while (waited < limit) {
        unsigned events = 0;
        int stop;

        pdot_mutex_lock(&a->lock);
        stop = a->shutdown || a->dirty;
        pdot_mutex_unlock(&a->lock);
        if (stop)
            return;
        if (*ws) {
            if (pdot_ws_drain(a->sc, *ws, &events)) {
                pdot_ws_close(*ws);
                *ws = NULL;
                *ws_retry = time(NULL) + WS_RETRY_S;
                return;
            }
            if (events)
                return;
        }
        pdot_sleep_ms(SLICE_MS);
        waited += SLICE_MS;
    }
}

static void watcher_main(void *arg)
{
    pdot_async *a = arg;
    pdot_client *c = a->sc;
    /* New final blocks are announced by the chain whose finality is trusted. */
    const char *url = c->verifier ? c->verifier->relay->rpc_url : c->rpc_url;
    pdot_final_block last;
    pdot_ws *ws = NULL;
    time_t ws_retry = 0;
    int have_last = 0;

    memset(&last, 0, sizeof last);
    for (;;) {
        int force;

        pdot_mutex_lock(&a->lock);
        if (a->shutdown) {
            pdot_mutex_unlock(&a->lock);
            break;
        }
        force = a->dirty;
        a->dirty = 0;
        pdot_mutex_unlock(&a->lock);

        reap(a);
        if (!ws && time(NULL) >= ws_retry && pdot_ws_available() &&
            pdot_ws_subscribe(c, url, "chain_subscribeFinalizedHeads", &ws))
            ws_retry = time(NULL) + WS_RETRY_S;
        ck_catch_up(a, c);
        tick(a, &last, &have_last, force);
        ck_publish(a, c);
        wait_trigger(a, &ws, &ws_retry);
    }
    pdot_ws_close(ws);
}

static uint64_t add_watch(pdot_async *a, struct watch *w)
{
    uint64_t id = 0;

    if (!w)
        return 0;
    pdot_mutex_lock(&a->lock);
    if (!a->shutdown &&
        (a->watcher_on || pdot_thread_start(&a->watcher, watcher_main, a) == 0)) {
        a->watcher_on = 1;
        w->id = id = ++a->next_id;
        w->next = a->watches;
        a->watches = w;
        a->dirty = 1;
    }
    pdot_mutex_unlock(&a->lock);
    if (!id)
        free_watch(w);
    return id;
}

static struct watch *new_watch(int kind, pdot_result_fn fn, void *ud)
{
    struct watch *w = calloc(1, sizeof *w);
    size_t i;

    if (!w)
        return NULL;
    for (i = 0; i < PDOT_Q_MAX_KEYS; i++)
        pdot_buf_init(&w->keys[i]);
    pdot_buf_init(&w->last);
    w->q.kind = kind;
    w->fn = fn;
    w->ud = ud;
    return w;
}

/* ---- Lifecycle ------------------------------------------------------------- */

pdot_async *pdot_async_new(pdot_client *c)
{
    pdot_async *a;

    if (!c || !(a = calloc(1, sizeof *a)))
        return NULL;
    if (pdot_mutex_init(&a->lock)) {
        free(a);
        return NULL;
    }
    if (pdot_cond_init(&a->cond)) {
        pdot_mutex_destroy(&a->lock);
        free(a);
        return NULL;
    }
    if (!(a->sc = pdot_client_clone(c))) {
        pdot_cond_destroy(&a->cond);
        pdot_mutex_destroy(&a->lock);
        free(a);
        return NULL;
    }
    a->c = c;
    ck_publish(a, c);  /* the starting point, before any thread runs */
    pdot_client_set_stop(a->c, worker_stop, a);
    pdot_client_set_stop(a->sc, watcher_stop, a);
    return a;
}

void pdot_async_free(pdot_async *a)
{
    struct item *it;
    struct watch *w;

    if (!a)
        return;
    pdot_mutex_lock(&a->lock);
    a->shutdown = 1;
    pdot_cond_broadcast(&a->cond);
    pdot_mutex_unlock(&a->lock);
    if (a->worker_on)
        pdot_thread_join(a->worker);
    if (a->watcher_on)
        pdot_thread_join(a->watcher);

    /* Every callback still owed runs now, on this thread. */
    a->wake = NULL;
    while ((it = a->queue)) {
        a->queue = it->next;
        it->next = NULL;
        set_error(it, PDOT_E_CANCELLED, NULL);
        finish(a, it);
    }
    a->queue_tail = NULL;
    while ((w = a->watches)) {
        a->watches = w->next;
        end_watch(a, w);
    }
    pdot_async_poll(a);

    pdot_client_free(a->c);
    pdot_client_free(a->sc);
    free(a->ck);
    pdot_cond_destroy(&a->cond);
    pdot_mutex_destroy(&a->lock);
    free(a);
}

int pdot_async_poll(pdot_async *a)
{
    struct item *list, *it;
    void (*ck_fn)(void *, const uint8_t *, size_t) = NULL;
    void *ck_ud = NULL;
    uint8_t *ck = NULL;
    size_t ck_len = 0;
    int n = 0;

    if (!a)
        return 0;
    pdot_mutex_lock(&a->lock);
    list = a->done;
    a->done = a->done_tail = NULL;
    if (a->ck_new && a->ck_fn && (ck = malloc(a->ck_len)) != NULL) {
        memcpy(ck, a->ck, a->ck_len);
        ck_len = a->ck_len;
        ck_fn = a->ck_fn;
        ck_ud = a->ck_ud;
        a->ck_new = 0;
    }
    pdot_mutex_unlock(&a->lock);
    while ((it = list)) {
        list = it->next;
        deliver(it);
        free_item(it);
        n++;
    }
    if (ck_fn)
        ck_fn(ck_ud, ck, ck_len);
    free(ck);
    return n;
}

int pdot_async_checkpoint(pdot_async *a, uint8_t **blob, size_t *len)
{
    int rc = PDOT_E_INVALID;

    if (!a || !blob || !len)
        return PDOT_E_INVALID;
    *blob = NULL;
    *len = 0;
    pdot_mutex_lock(&a->lock);
    if (a->ck) {
        rc = PDOT_E_NOMEM;
        if ((*blob = malloc(a->ck_len)) != NULL) {
            memcpy(*blob, a->ck, a->ck_len);
            *len = a->ck_len;
            rc = 0;
        }
    }
    pdot_mutex_unlock(&a->lock);
    return rc;
}

void pdot_async_set_checkpoint_fn(pdot_async *a,
                                  void (*fn)(void *ud, const uint8_t *blob, size_t len), void *ud)
{
    if (!a)
        return;
    pdot_mutex_lock(&a->lock);
    a->ck_fn = fn;
    a->ck_ud = ud;
    pdot_mutex_unlock(&a->lock);
}

void pdot_async_set_wake(pdot_async *a, void (*wake)(void *ud), void *ud)
{
    if (!a)
        return;
    pdot_mutex_lock(&a->lock);
    a->wake = wake;
    a->wake_ud = ud;
    pdot_mutex_unlock(&a->lock);
}

int pdot_async_cancel(pdot_async *a, uint64_t id)
{
    struct item **pp, *it = NULL, *prev = NULL;
    struct watch *w;
    int found = 0;

    if (!a || !id)
        return PDOT_E_INVALID;
    pdot_mutex_lock(&a->lock);
    for (pp = &a->queue; *pp; prev = *pp, pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            it = *pp;
            *pp = it->next;
            if (a->queue_tail == it)
                a->queue_tail = prev;
            it->next = NULL;
            found = 1;
            break;
        }
    }
    if (!found && a->running == id) {
        a->cancel_running = 1;
        found = 1;
    }
    for (w = a->watches; !found && w; w = w->next) {
        if (w->id == id && !w->removed) {
            w->removed = 1;
            a->dirty = 1;
            found = 1;
        }
    }
    pdot_mutex_unlock(&a->lock);
    if (it) {
        set_error(it, PDOT_E_CANCELLED, NULL);
        finish(a, it);
    }
    return found ? 0 : PDOT_E_NOTFOUND;
}

/* ---- Jobs ------------------------------------------------------------------ */

static int w_run(pdot_client *c, struct item *it)
{
    return it->user_work(c, it->ud);
}

static int w_store(pdot_client *c, struct item *it)
{
    int rc = pdot_bulletin_store(c, &it->signer, it->data, it->len, &it->res.store);

    it->has_result = rc == PDOT_E_DISPATCH;
    return rc;
}

static void on_chunk(void *ud, size_t index, size_t total, const pdot_cid *cid,
                     const pdot_tx_result *tx, int rc)
{
    struct item *job = ud, *it;

    if (!job->progress || !(it = new_item(job->a, IT_PROGRESS, NULL, job->ud)))
        return;
    it->progress = job->progress;
    it->index = index;
    it->total = total;
    if (cid)
        it->cid = *cid;
    if (tx)
        it->tx = *tx;
    it->rc = rc;
    finish(job->a, it);
}

static int w_store_chunked(pdot_client *c, struct item *it)
{
    int rc = pdot_bulletin_store_chunked(c, &it->signer, it->data, it->len, it->chunk_size,
                                         on_chunk, it, &it->res.chunked);

    it->has_result = rc == PDOT_E_DISPATCH;
    return rc;
}

static int w_renew(pdot_client *c, struct item *it)
{
    int rc = pdot_bulletin_renew(c, &it->signer, &it->cid, &it->res.tx);

    it->has_result = rc == PDOT_E_DISPATCH;
    return rc;
}

static int w_verify(pdot_client *c, struct item *it)
{
    return pdot_bulletin_verify_cid(c, &it->cid, &it->res.proof);
}

static int w_authorization(pdot_client *c, struct item *it)
{
    return pdot_bulletin_authorization(c, it->account, &it->res.auth);
}

static int w_fetch(pdot_client *c, struct item *it)
{
    uint8_t *data;
    size_t len;
    int rc = pdot_bulletin_fetch(c, &it->cid, &data, &len);

    if (!rc) {
        it->owned = data;
        it->res.bytes.data = data;
        it->res.bytes.len = len;
    }
    return rc;
}

static int w_query(pdot_client *c, struct item *it)
{
    int rc = pdot_query_run(c, &it->q, &it->res);

    if (!rc && it->q.kind == PDOT_Q_STORAGE)
        it->owned = it->res.storage.data;
    return rc;
}

static int w_nft_owned(pdot_client *c, struct item *it)
{
    uint32_t *items;
    size_t n;
    int rc = pdot_nft_owned(c, it->account, it->q.id, it->max, &items, &n, &it->res.nfts.at);

    if (!rc) {
        it->owned = items;
        it->res.nfts.items = items;
        it->res.nfts.n = n;
    }
    return rc;
}

static int w_submit(pdot_client *c, struct item *it)
{
    int rc = pdot_tx_submit(c, &it->signer, it->data, it->len, &it->opts, &it->res.tx);

    it->has_result = rc == PDOT_E_DISPATCH;
    return rc;
}

static int w_fee(pdot_client *c, struct item *it)
{
    return pdot_tx_fee(c, &it->signer, it->data, it->len, &it->opts, &it->res.fee);
}

static int w_transfer(pdot_client *c, struct item *it)
{
    int rc = pdot_transfer_submit(c, &it->signer, &it->transfer, &it->opts, &it->res.tx);

    it->has_result = rc == PDOT_E_DISPATCH;
    return rc;
}

/* Copies transaction options into the job, pointing proxy_for at its own copy. */
static struct item *with_options(struct item *it, const pdot_tx_options *o)
{
    if (it && o) {
        it->opts = *o;
        if (o->proxy_for) {
            memcpy(it->proxy_for, o->proxy_for, 32);
            it->opts.proxy_for = it->proxy_for;
        }
    }
    return it;
}

static struct item *job(pdot_async *a, work_fn work, pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!a || !(it = new_item(a, IT_JOB, done, ud)))
        return NULL;
    it->work = work;
    return it;
}

/* Copies data (and a signer) into a job; NULL on failure, freeing it. */
static struct item *with_input(struct item *it, const pdot_signer *s, const uint8_t *data,
                               size_t len)
{
    if (!it)
        return NULL;
    if (s)
        it->signer = *s;
    if (data && !(it->data = malloc(len ? len : 1))) {
        free_item(it);
        return NULL;
    }
    if (data)
        memcpy(it->data, data, len);
    it->len = len;
    return it;
}

uint64_t pdot_async_run(pdot_async *a, int (*work)(pdot_client *c, void *ud),
                        pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!work || !(it = job(a, w_run, done, ud)))
        return 0;
    it->user_work = work;
    return submit(a, it);
}

uint64_t pdot_async_store(pdot_async *a, const pdot_signer *s, const uint8_t *data,
                          size_t len, pdot_result_fn done, void *ud)
{
    if (!s || !data)
        return 0;
    return submit(a, with_input(job(a, w_store, done, ud), s, data, len));
}

uint64_t pdot_async_store_chunked(pdot_async *a, const pdot_signer *s, const uint8_t *data,
                                  size_t len, size_t chunk_size, pdot_progress_fn progress,
                                  pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!s || !data || !(it = with_input(job(a, w_store_chunked, done, ud), s, data, len)))
        return 0;
    it->chunk_size = chunk_size;
    it->progress = progress;
    return submit(a, it);
}

uint64_t pdot_async_renew(pdot_async *a, const pdot_signer *s, const pdot_cid *cid,
                          pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!s || !cid || !(it = with_input(job(a, w_renew, done, ud), s, NULL, 0)))
        return 0;
    it->cid = *cid;
    return submit(a, it);
}

uint64_t pdot_async_verify_cid(pdot_async *a, const pdot_cid *cid, pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!cid || !(it = job(a, w_verify, done, ud)))
        return 0;
    it->cid = *cid;
    return submit(a, it);
}

uint64_t pdot_async_authorization(pdot_async *a, const uint8_t account_id[32],
                                  pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!account_id || !(it = job(a, w_authorization, done, ud)))
        return 0;
    memcpy(it->account, account_id, 32);
    return submit(a, it);
}

uint64_t pdot_async_fetch(pdot_async *a, const pdot_cid *cid, pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!cid || !(it = job(a, w_fetch, done, ud)))
        return 0;
    it->cid = *cid;
    return submit(a, it);
}

static uint64_t query_job(pdot_async *a, const pdot_query *q, pdot_result_fn done, void *ud)
{
    struct item *it = job(a, w_query, done, ud);

    if (!it)
        return 0;
    it->q = *q;
    return submit(a, it);
}

uint64_t pdot_async_balance(pdot_async *a, const uint8_t account_id[32],
                            pdot_result_fn done, void *ud)
{
    pdot_query q;

    if (!account_id)
        return 0;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_BALANCE;
    memcpy(q.account, account_id, 32);
    return query_job(a, &q, done, ud);
}

uint64_t pdot_async_asset_balance(pdot_async *a, uint32_t asset_id, const uint8_t account_id[32],
                                  pdot_result_fn done, void *ud)
{
    pdot_query q;

    if (!account_id)
        return 0;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_ASSET_BALANCE;
    q.id = asset_id;
    memcpy(q.account, account_id, 32);
    return query_job(a, &q, done, ud);
}

uint64_t pdot_async_asset_info(pdot_async *a, uint32_t asset_id, pdot_result_fn done, void *ud)
{
    pdot_query q;

    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_ASSET_INFO;
    q.id = asset_id;
    return query_job(a, &q, done, ud);
}

uint64_t pdot_async_nft_item(pdot_async *a, uint32_t collection, uint32_t item,
                             pdot_result_fn done, void *ud)
{
    pdot_query q;

    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_NFT_ITEM;
    q.id = collection;
    q.item = item;
    return query_job(a, &q, done, ud);
}

uint64_t pdot_async_nft_owned(pdot_async *a, const uint8_t account_id[32], uint32_t collection,
                              size_t max, pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!account_id || !(it = job(a, w_nft_owned, done, ud)))
        return 0;
    memcpy(it->account, account_id, 32);
    it->q.id = collection;
    it->max = max;
    return submit(a, it);
}

/* ---- Watches ---------------------------------------------------------------- */

uint64_t pdot_async_watch_head(pdot_async *a, pdot_result_fn fn, void *ud)
{
    struct watch *w;

    if (!a || !fn || !(w = new_watch(PDOT_Q_STORAGE, fn, ud)))
        return 0;
    w->head = 1;
    return add_watch(a, w);
}

uint64_t pdot_async_watch_storage(pdot_async *a, const uint8_t *key, size_t key_len,
                                  pdot_result_fn fn, void *ud)
{
    struct watch *w;

    if (!a || !fn || !key || !key_len || !(w = new_watch(PDOT_Q_STORAGE, fn, ud)))
        return 0;
    if (!(w->bytes = malloc(key_len))) {
        free_watch(w);
        return 0;
    }
    memcpy(w->bytes, key, key_len);
    w->q.bytes = w->bytes;
    w->q.len = key_len;
    return add_watch(a, w);
}

uint64_t pdot_async_watch_balance(pdot_async *a, const uint8_t account_id[32],
                                  pdot_result_fn fn, void *ud)
{
    struct watch *w;

    if (!a || !fn || !account_id || !(w = new_watch(PDOT_Q_BALANCE, fn, ud)))
        return 0;
    memcpy(w->q.account, account_id, 32);
    return add_watch(a, w);
}

uint64_t pdot_async_watch_asset_balance(pdot_async *a, uint32_t asset_id,
                                        const uint8_t account_id[32],
                                        pdot_result_fn fn, void *ud)
{
    struct watch *w;

    if (!a || !fn || !account_id || !(w = new_watch(PDOT_Q_ASSET_BALANCE, fn, ud)))
        return 0;
    w->q.id = asset_id;
    memcpy(w->q.account, account_id, 32);
    return add_watch(a, w);
}

uint64_t pdot_async_watch_nft_item(pdot_async *a, uint32_t collection, uint32_t item,
                                   pdot_result_fn fn, void *ud)
{
    struct watch *w;

    if (!a || !fn || !(w = new_watch(PDOT_Q_NFT_ITEM, fn, ud)))
        return 0;
    w->q.id = collection;
    w->q.item = item;
    return add_watch(a, w);
}

/* ---- Transactions -------------------------------------------------------- */

uint64_t pdot_async_submit(pdot_async *a, const pdot_signer *s, const uint8_t *call,
                           size_t len, const pdot_tx_options *o, pdot_result_fn done, void *ud)
{
    if (!s || !call || !len)
        return 0;
    return submit(a, with_options(with_input(job(a, w_submit, done, ud), s, call, len), o));
}

uint64_t pdot_async_fee(pdot_async *a, const pdot_signer *s, const uint8_t *call, size_t len,
                        const pdot_tx_options *o, pdot_result_fn done, void *ud)
{
    if (!s || !call || !len)
        return 0;
    return submit(a, with_options(with_input(job(a, w_fee, done, ud), s, call, len), o));
}

uint64_t pdot_async_transfer(pdot_async *a, const pdot_signer *s, const pdot_transfer *t,
                             const pdot_tx_options *o, pdot_result_fn done, void *ud)
{
    struct item *it;

    if (!s || !t || !(it = with_options(with_input(job(a, w_transfer, done, ud), s, NULL, 0), o)))
        return 0;
    it->transfer = *t;
    return submit(a, it);
}

uint64_t pdot_async_proxies(pdot_async *a, const uint8_t account_id[32],
                            pdot_result_fn done, void *ud)
{
    pdot_query q;

    if (!account_id)
        return 0;
    memset(&q, 0, sizeof q);
    q.kind = PDOT_Q_PROXIES;
    memcpy(q.account, account_id, 32);
    return query_job(a, &q, done, ud);
}
