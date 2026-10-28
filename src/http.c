#include <curl/curl.h>

#include "pdot.h"
#include "http.h"

/* Response bodies larger than this are refused (fetched data is <= 2 MiB hex). */
#define HTTP_MAX_RESPONSE (64u * 1024 * 1024)

int pdot_http_init(pdot_client *c)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return pdot_fail(c, PDOT_E_NETWORK, "curl_global_init failed");
    c->curl = curl_easy_init();
    if (!c->curl) {
        curl_global_cleanup();
        return pdot_fail(c, PDOT_E_NOMEM, "curl_easy_init failed");
    }
    return 0;
}

void pdot_http_free(pdot_client *c)
{
    if (c->curl) {
        curl_easy_cleanup(c->curl);
        c->curl = NULL;
        curl_global_cleanup();
    }
}

static size_t on_data(char *p, size_t size, size_t n, void *userdata)
{
    pdot_buf *b = userdata;
    size_t len = size * n;

    if (b->len + len > HTTP_MAX_RESPONSE)
        return 0;  /* aborts the transfer */
    pdot_scale_raw(b, (const uint8_t *)p, len);
    return b->err ? 0 : len;
}

int pdot_http_request(pdot_client *c, const char *url, const char *body,
                      size_t body_len, pdot_buf *resp)
{
    CURL *h = c->curl;
    struct curl_slist *headers = NULL;
    CURLcode rc;
    long status = 0;

    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, c->timeout_ms < 10000 ? c->timeout_ms : 10000L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, resp);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }

    rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    if (resp->err)
        return pdot_fail(c, resp->err, "HTTP response from %s too large", url);
    if (rc != CURLE_OK)
        return pdot_fail(c, rc == CURLE_OPERATION_TIMEDOUT ? PDOT_E_TIMEOUT : PDOT_E_NETWORK,
                         "%s: %s", url, curl_easy_strerror(rc));
    if (status < 200 || status > 299)
        return pdot_fail(c, status == 404 ? PDOT_E_NOTFOUND : PDOT_E_NETWORK,
                         "%s: HTTP %ld", url, status);
    return 0;
}
