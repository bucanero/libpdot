/* Internal client state shared by the transport, RPC and Bulletin modules. */
#ifndef PDOT_CLIENT_H
#define PDOT_CLIENT_H

#include <stdint.h>

#include "pdot.h"
#include "metadata.h"

struct pdot_client {
    char    *rpc_url;
    char    *gateway_url;      /* NULL: fetch through bitswap_v1_get */
    void    *curl;             /* CURL easy handle, reused for keep-alive */
    long     timeout_ms;
    unsigned poll_ms;          /* finalized-head polling interval */
    uint32_t next_id;
    char     err[256];

    /* Chain state, refreshed by pdot_chain_sync(). */
    int      have_chain;
    uint8_t  genesis[32];
    uint32_t spec_version;
    uint32_t tx_version;
    pdot_metadata *md;         /* parsed for spec_version */
};

/* Records a message for pdot_last_error and returns code. */
int pdot_fail(pdot_client *c, int code, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void pdot_sleep_ms(unsigned ms);

/* Fetches genesis and runtime version; refetches metadata if specVersion changed. */
int pdot_chain_sync(pdot_client *c);

#endif
