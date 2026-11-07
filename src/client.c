/* nanosleep is POSIX; strict -std=c99 hides it on glibc without this. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "pdot.h"
#include "client.h"
#include "http.h"
#include "mdhash.h"
#include "jsonrpc.h"
#include "scale.h"
#include "thread.h"
#include "verify.h"

/*
 * Metadata shared by a client and its clones, which run on different
 * threads. The lock guards `md`, `spec` and every holder count change of the
 * metadata the clients hold; it is held during a download so the others
 * wait for it instead of fetching the same metadata again.
 */
struct pdot_md_share {
    pdot_mutex     lock;
    unsigned       clients;
    pdot_metadata *md;    /* newest download, or NULL */
    uint32_t       spec;  /* its spec_version */
};

static void release_metadata(pdot_client *c, pdot_metadata *md)
{
    if (!md)
        return;
    if (!c->share) {
        pdot_metadata_free(md);
        return;
    }
    pdot_mutex_lock(&c->share->lock);
    pdot_metadata_free(md);
    pdot_mutex_unlock(&c->share->lock);
}

static void leave_share(pdot_client *c)
{
    struct pdot_md_share *sh = c->share;
    unsigned left;

    if (!sh)
        return;
    pdot_mutex_lock(&sh->lock);
    left = --sh->clients;
    pdot_mutex_unlock(&sh->lock);
    c->share = NULL;
    if (left == 0) {
        pdot_metadata_free(sh->md);
        pdot_mutex_destroy(&sh->lock);
        free(sh);
    }
}

static char *dup_url(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s);
    /* ws:// and wss:// node endpoints answer HTTP on the same port. */
    if (strncmp(s, "ws://", 5) == 0 || strncmp(s, "wss://", 6) == 0) {
        if (!(p = malloc(n + 3)))
            return NULL;
        sprintf(p, "http%s", s + 2);
        return p;
    }
    if (!(p = malloc(n + 1)))
        return NULL;
    memcpy(p, s, n + 1);
    while (n > 0 && p[n - 1] == '/')
        p[--n] = '\0';
    return p;
}

pdot_client *pdot_client_new(const char *rpc_url, const char *gateway_url)
{
    pdot_client *c;

    if (!rpc_url || !(c = calloc(1, sizeof *c)))
        return NULL;
    c->timeout_ms = 60000;
    c->token_decimals = -1;
    c->poll_ms = 2000;
    c->rpc_url = dup_url(rpc_url);
    c->gateway_url = dup_url(gateway_url);
    if (!c->rpc_url || (gateway_url && !c->gateway_url) || pdot_http_init(c)) {
        pdot_client_free(c);
        return NULL;
    }
    return c;
}

void pdot_client_free(pdot_client *c)
{
    if (!c)
        return;
    pdot_http_free(c);
    pdot_verifier_free(c->verifier);
    release_metadata(c, c->md);
    leave_share(c);
    free(c->rpc_url);
    free(c->gateway_url);
    free(c);
}

const char *pdot_last_error(const pdot_client *c)
{
    return c && c->err[0] ? c->err : "no error";
}

void pdot_client_set_timeout(pdot_client *c, long timeout_ms)
{
    if (c && timeout_ms > 0) {
        c->timeout_ms = timeout_ms;
        if (c->verifier)
            c->verifier->relay->timeout_ms = timeout_ms;
    }
}

void pdot_sleep_ms(unsigned ms)
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

int pdot_client_stopped(pdot_client *c)
{
    return c && c->stop && c->stop(c->stop_ud);
}

int pdot_client_cancelled(pdot_client *c)
{
    return pdot_client_stopped(c);
}

int pdot_client_wait(pdot_client *c, unsigned ms)
{
    while (ms > 0) {
        unsigned slice = ms < 50 ? ms : 50;

        if (pdot_client_stopped(c))
            return PDOT_E_CANCELLED;
        pdot_sleep_ms(slice);
        ms -= slice;
    }
    return pdot_client_stopped(c) ? PDOT_E_CANCELLED : 0;
}

void pdot_client_set_included_fn(pdot_client *c,
                                 void (*fn)(void *ud, const pdot_tx_result *where), void *ud)
{
    if (c) {
        c->included = fn;
        c->included_ud = fn ? ud : NULL;
    }
}

void pdot_client_set_stop(pdot_client *c, int (*stop)(void *), void *ud)
{
    c->stop = stop;
    c->stop_ud = ud;
    if (c->verifier) {
        c->verifier->relay->stop = stop;
        c->verifier->relay->stop_ud = ud;
    }
}

pdot_client *pdot_client_clone(pdot_client *c)
{
    pdot_client *n = pdot_client_new(c->rpc_url, c->gateway_url);
    uint8_t *ck;
    size_t len;
    int rc;

    if (!n)
        return NULL;
    if (!c->share) {
        struct pdot_md_share *sh = calloc(1, sizeof *sh);
        if (!sh || pdot_mutex_init(&sh->lock)) {
            free(sh);
            pdot_client_free(n);
            return NULL;
        }
        sh->clients = 1;
        if (c->md) {  /* what c already has serves the clone too */
            c->md->refs++;
            sh->md = c->md;
            sh->spec = c->spec_version;
        }
        c->share = sh;
    }
    pdot_mutex_lock(&c->share->lock);
    c->share->clients++;
    pdot_mutex_unlock(&c->share->lock);
    n->share = c->share;
    n->timeout_ms = c->timeout_ms;
    n->poll_ms = c->poll_ms;
    n->mdhash_on = c->mdhash_on;
    strcpy(n->token_symbol, c->token_symbol);
    n->token_decimals = c->token_decimals;
    if (c->verifier) {
        if (pdot_client_checkpoint(c, &ck, &len)) {
            pdot_client_free(n);
            return NULL;
        }
        rc = pdot_client_set_verifier(n, c->verifier->relay->rpc_url, c->verifier->para_id,
                                      ck, len);
        free(ck);
        if (rc) {
            pdot_client_free(n);
            return NULL;
        }
    }
    return n;
}

int pdot_fail(pdot_client *c, int code, const char *fmt, ...)
{
    va_list ap;

    if (c) {
        va_start(ap, fmt);
        vsnprintf(c->err, sizeof c->err, fmt, ap);
        va_end(ap);
    }
    return code;
}

/*
 * Metadata v15 through Metadata_metadata_at_version, which returns
 * Option<OpaqueMetadata>; nodes without it get state_getMetadata (v14).
 */
static int fetch_metadata(pdot_client *c, pdot_metadata **md)
{
    static const uint8_t v15[4] = { 15, 0, 0, 0 };
    uint8_t *raw = NULL;
    size_t len = 0;
    const uint8_t *blob = NULL;
    size_t blob_len = 0;
    int rc;

    rc = pdot_rpc_state_call(c, "Metadata_metadata_at_version", v15, sizeof v15, &raw, &len);
    if (rc == 0) {
        pdot_reader r;
        int some;

        pdot_reader_init(&r, raw, len);
        if (pdot_read_bool(&r, &some) || (some && pdot_read_bytes(&r, &blob, &blob_len))) {
            free(raw);
            return pdot_fail(c, PDOT_E_DECODE, "Metadata_metadata_at_version: bad encoding");
        }
        if (!some) {
            free(raw);
            raw = NULL;
            rc = PDOT_E_NOTFOUND;
        }
    }
    if (rc) {
        if ((rc = pdot_rpc_bytes(c, "state_getMetadata", NULL, &raw, &len)))
            return rc;
        blob = raw;
        blob_len = len;
    }
    rc = pdot_metadata_parse(blob, blob_len, md);
    free(raw);
    if (rc)
        return pdot_fail(c, rc, "cannot parse metadata: %s", pdot_strerror(rc));
    return 0;
}

/* Metadata for spec from the share, downloading it if no clone has yet. */
static int shared_metadata(pdot_client *c, uint32_t spec, pdot_metadata **md)
{
    struct pdot_md_share *sh = c->share;
    int rc = 0;

    pdot_mutex_lock(&sh->lock);
    if (!sh->md || sh->spec != spec) {
        pdot_metadata *fresh;
        if (!(rc = fetch_metadata(c, &fresh))) {
            pdot_metadata_free(sh->md);
            sh->md = fresh;
            sh->spec = spec;
        }
    }
    if (!rc) {
        sh->md->refs++;  /* one for c */
        *md = sh->md;
    }
    pdot_mutex_unlock(&sh->lock);
    return rc;
}

/* The RFC-0078 hash for the current metadata, if the runtime checks one. */
static int refresh_mdhash(pdot_client *c)
{
    pdot_mdhash_info info;
    char spec_name[64], symbol[32];
    uint32_t spec, tx;
    uint8_t decimals = 0;
    size_t i;
    int rc;

    c->have_mdhash = 0;
    for (i = 0; i < c->md->next; i++)
        if (pdot_md_name_eq(c->md->ext[i].name, c->md->ext[i].name_len, "CheckMetadataHash"))
            break;
    if (i == c->md->next)
        return 0;
    if ((rc = pdot_rpc_runtime_version(c, &spec, &tx, spec_name, sizeof spec_name)))
        return rc;
    /* Values given to the runtime build; the node's properties unless overridden. */
    if ((!c->token_symbol[0] || c->token_decimals < 0) &&
        (rc = pdot_rpc_token(c, symbol, sizeof symbol, &decimals)))
        return rc;
    if (c->token_symbol[0])
        strcpy(symbol, c->token_symbol);
    if (c->token_decimals >= 0)
        decimals = (uint8_t)c->token_decimals;
    info.spec_version = c->spec_version;
    info.spec_name = spec_name;
    info.decimals = decimals;
    info.token_symbol = symbol;
    if ((rc = pdot_md_ss58_prefix(c->md, &info.ss58_prefix)) ||
        (rc = pdot_metadata_hash(c->md, &info, c->mdhash)))
        return pdot_fail(c, rc, "cannot compute the metadata hash: %s", pdot_strerror(rc));
    c->have_mdhash = 1;
    c->mdhash_spec = c->spec_version;
    return 0;
}

int pdot_chain_sync(pdot_client *c)
{
    uint32_t spec, tx;
    int rc;

    if (!c->have_chain) {
        if ((rc = pdot_rpc_block_hash(c, 0, c->genesis)))
            return rc;
        c->have_chain = 1;
    }
    if ((rc = pdot_rpc_runtime_version(c, &spec, &tx, NULL, 0)))
        return rc;
    if (!c->md || spec != c->spec_version) {
        pdot_metadata *md;
        if ((rc = c->share ? shared_metadata(c, spec, &md) : fetch_metadata(c, &md)))
            return rc;
        release_metadata(c, c->md);
        c->md = md;
        c->have_mdhash = 0;
    }
    c->spec_version = spec;
    c->tx_version = tx;
    if (c->mdhash_on && !(c->have_mdhash && c->mdhash_spec == spec))
        return refresh_mdhash(c);
    return 0;
}

int pdot_client_set_metadata_hash(pdot_client *c, int enable, const char *token_symbol,
                                  int decimals)
{
    if (!c || decimals > 255 || (token_symbol && strlen(token_symbol) >= sizeof c->token_symbol))
        return PDOT_E_INVALID;
    c->mdhash_on = enable;
    strcpy(c->token_symbol, token_symbol ? token_symbol : "");
    c->token_decimals = decimals < 0 ? -1 : decimals;
    c->have_mdhash = 0;
    return 0;
}

int pdot_client_metadata_hash(pdot_client *c, uint8_t out[32])
{
    int rc;

    if (!c || !out)
        return PDOT_E_INVALID;
    if (!c->mdhash_on)
        return pdot_fail(c, PDOT_E_NOTFOUND, "metadata hash is off");
    if ((rc = pdot_chain_sync(c)))
        return rc;
    if (!c->have_mdhash)
        return pdot_fail(c, PDOT_E_NOTFOUND, "the runtime has no CheckMetadataHash extension");
    memcpy(out, c->mdhash, 32);
    return 0;
}

int pdot_client_token(pdot_client *c, char *symbol, size_t cap, uint8_t *decimals)
{
    if (!c || !symbol || !cap || !decimals)
        return PDOT_E_INVALID;
    return pdot_rpc_token(c, symbol, cap, decimals);
}
