/* HTTP transport over libcurl. The only module that touches the network. */
#ifndef PDOT_HTTP_H
#define PDOT_HTTP_H

#include <stddef.h>

#include "client.h"
#include "scale.h"

int  pdot_http_init(pdot_client *c);
void pdot_http_free(pdot_client *c);

/*
 * POSTs a JSON body (or GETs when body is NULL) and appends the response body
 * to resp. Any status other than 2xx is PDOT_E_NETWORK.
 */
int pdot_http_request(pdot_client *c, const char *url, const char *body,
                      size_t body_len, pdot_buf *resp);

#endif
