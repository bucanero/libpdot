#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "pdot.h"
#include "scale.h"
#include "ws.h"

/* curl_ws_send and curl_ws_recv arrived in libcurl 7.86.0. */
#if LIBCURL_VERSION_NUM >= 0x075600

/* The first reply (to the subscription request) is kept to look for an error. */
#define FIRST_REPLY_MAX 4096

struct pdot_ws {
    CURL    *h;
    unsigned messages;
    pdot_buf first;  /* the first message, up to FIRST_REPLY_MAX bytes */
};

int pdot_ws_available(void)
{
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    const char *const *p;

    for (p = v ? v->protocols : NULL; p && *p; p++)
        if (strcmp(*p, "ws") == 0)
            return 1;
    return 0;
}

static char *ws_url(const char *http_url)
{
    size_t n = strlen(http_url);
    char *u = malloc(n + 1);

    if (!u)
        return NULL;
    if (strncmp(http_url, "https://", 8) == 0)
        snprintf(u, n + 1, "wss://%s", http_url + 8);
    else if (strncmp(http_url, "http://", 7) == 0)
        snprintf(u, n + 1, "ws://%s", http_url + 7);
    else
        memcpy(u, http_url, n + 1);
    return u;
}

void pdot_ws_close(pdot_ws *ws)
{
    if (!ws)
        return;
    if (ws->h)
        curl_easy_cleanup(ws->h);
    pdot_buf_free(&ws->first);
    free(ws);
}

int pdot_ws_subscribe(pdot_client *c, const char *http_url, const char *method,
                      pdot_ws **out)
{
    pdot_ws *ws;
    char *url, req[160];
    size_t sent = 0, len;
    CURLcode rc;
    int waited = 0;

    *out = NULL;
    if (!pdot_ws_available())
        return pdot_fail(c, PDOT_E_UNSUPPORTED, "libcurl has no WebSocket support");
    if (!(ws = calloc(1, sizeof *ws)) || !(url = ws_url(http_url))) {
        free(ws);
        return pdot_fail(c, PDOT_E_NOMEM, "out of memory");
    }
    pdot_buf_init(&ws->first);
    if (!(ws->h = curl_easy_init())) {
        free(url);
        pdot_ws_close(ws);
        return pdot_fail(c, PDOT_E_NOMEM, "curl_easy_init failed");
    }
    curl_easy_setopt(ws->h, CURLOPT_URL, url);
    curl_easy_setopt(ws->h, CURLOPT_CONNECT_ONLY, 2L);  /* WebSocket upgrade, then hand over */
    curl_easy_setopt(ws->h, CURLOPT_CONNECTTIMEOUT_MS, c->timeout_ms < 10000 ? c->timeout_ms : 10000L);
    curl_easy_setopt(ws->h, CURLOPT_NOSIGNAL, 1L);
    rc = curl_easy_perform(ws->h);
    free(url);
    if (rc != CURLE_OK) {
        pdot_ws_close(ws);
        return pdot_fail(c, PDOT_E_NETWORK, "WebSocket: %s", curl_easy_strerror(rc));
    }

    len = (size_t)snprintf(req, sizeof req,
                           "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":[]}", method);
    while (sent < len) {
        size_t n = 0;
        rc = curl_ws_send(ws->h, req + sent, len - sent, &n, 0, CURLWS_TEXT);
        if (rc == CURLE_AGAIN && waited++ < 200 && !pdot_client_wait(c, 10))
            continue;
        if (rc != CURLE_OK) {
            pdot_ws_close(ws);
            return pdot_fail(c, PDOT_E_NETWORK, "WebSocket send: %s", curl_easy_strerror(rc));
        }
        sent += n;
    }
    *out = ws;
    return 0;
}

/* A JSON-RPC error reply: an "error" member (the buffer is not NUL-terminated). */
static int has_error(const pdot_buf *b)
{
    static const char key[] = "\"error\"";
    size_t i;

    for (i = 0; b->data && i + sizeof key - 1 <= b->len; i++)
        if (memcmp(b->data + i, key, sizeof key - 1) == 0)
            return 1;
    return 0;
}

int pdot_ws_drain(pdot_client *c, pdot_ws *ws, unsigned *events)
{
    char buf[16384];

    *events = 0;
    for (;;) {
        const struct curl_ws_frame *meta = NULL;
        size_t n = 0;
        CURLcode rc = curl_ws_recv(ws->h, buf, sizeof buf, &n, &meta);

        if (rc == CURLE_AGAIN)
            return 0;
        if (rc != CURLE_OK)
            return pdot_fail(c, PDOT_E_NETWORK, "WebSocket: %s", curl_easy_strerror(rc));
        if (!meta || (meta->flags & CURLWS_CLOSE))
            return pdot_fail(c, PDOT_E_NETWORK, "WebSocket closed by the node");
        if (!(meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)))
            continue;  /* ping and pong: libcurl answers pings itself */
        if (ws->messages == 0 && ws->first.len + n <= FIRST_REPLY_MAX)
            pdot_scale_raw(&ws->first, (const uint8_t *)buf, n);
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            /* The first message answers the subscription request. */
            if (ws->messages++ == 0 && has_error(&ws->first))
                return pdot_fail(c, PDOT_E_NETWORK, "the node refused the subscription");
            (*events)++;
        }
    }
}

#else  /* libcurl without curl_ws_* */

int pdot_ws_available(void)
{
    return 0;
}

int pdot_ws_subscribe(pdot_client *c, const char *http_url, const char *method,
                      pdot_ws **out)
{
    (void)http_url;
    (void)method;
    *out = NULL;
    return pdot_fail(c, PDOT_E_UNSUPPORTED, "libcurl has no WebSocket support");
}

int pdot_ws_drain(pdot_client *c, pdot_ws *ws, unsigned *events)
{
    (void)ws;
    *events = 0;
    return pdot_fail(c, PDOT_E_UNSUPPORTED, "libcurl has no WebSocket support");
}

void pdot_ws_close(pdot_ws *ws)
{
    (void)ws;
}

#endif
