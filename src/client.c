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
#include "rpc.h"
#include "scale.h"

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
    pdot_metadata_free(c->md);
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
    if (c && timeout_ms > 0)
        c->timeout_ms = timeout_ms;
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

int pdot_chain_sync(pdot_client *c)
{
    uint32_t spec, tx;
    int rc;

    if (!c->have_chain) {
        if ((rc = pdot_rpc_block_hash(c, 0, c->genesis)))
            return rc;
        c->have_chain = 1;
    }
    if ((rc = pdot_rpc_runtime_version(c, &spec, &tx)))
        return rc;
    if (!c->md || spec != c->spec_version) {
        pdot_metadata *md;
        if ((rc = fetch_metadata(c, &md)))
            return rc;
        pdot_metadata_free(c->md);
        c->md = md;
    }
    c->spec_version = spec;
    c->tx_version = tx;
    return 0;
}
