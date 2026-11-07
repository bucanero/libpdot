/* Internal client state shared by the transport, RPC and Bulletin modules. */
#ifndef PDOT_CLIENT_H
#define PDOT_CLIENT_H

#include <stdint.h>
#include <stdio.h>  /* MinGW: __MINGW_PRINTF_FORMAT */

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
    struct pdot_md_share *share;  /* metadata shared with clones; NULL if none */

    /* CheckMetadataHash (RFC-0078), refreshed with the metadata. */
    int      mdhash_on;
    char     token_symbol[32];  /* empty: from system_properties */
    int      token_decimals;    /* -1: from system_properties */
    int      have_mdhash;       /* 0 if off or the runtime lacks the extension */
    uint32_t mdhash_spec;       /* spec_version the hash was computed for */
    uint8_t  mdhash[32];

    struct pdot_verifier *verifier;  /* NULL: RPC answers are trusted */

    /* Nonzero from stop(stop_ud) abandons the current call (pdot_async). */
    int    (*stop)(void *ud);
    void    *stop_ud;

    /* pdot_client_set_included_fn; NULL: best blocks are not watched. */
    void   (*included)(void *ud, const pdot_tx_result *where);
    void    *included_ud;
};

/* MinGW checks printf formats against the Microsoft runtime unless told it is C99's. */
#if defined(__MINGW_PRINTF_FORMAT)
#define PDOT_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define PDOT_PRINTF_FORMAT printf
#endif

/* Records a message for pdot_last_error and returns code. */
int pdot_fail(pdot_client *c, int code, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(PDOT_PRINTF_FORMAT, 3, 4)))
#endif
    ;

void pdot_sleep_ms(unsigned ms);

/* 1 once c->stop asks the current call to give up. */
int  pdot_client_stopped(pdot_client *c);
/* Sleeps up to ms, or PDOT_E_CANCELLED as soon as the client is stopped. */
int  pdot_client_wait(pdot_client *c, unsigned ms);
/* Sets the stop hook on c and on its verifier's relay client. */
void pdot_client_set_stop(pdot_client *c, int (*stop)(void *), void *ud);
/*
 * A second client with the same endpoints, settings and verifier state. The
 * two (and further clones) share downloaded metadata, so each runtime
 * version is fetched once, by whichever syncs first.
 */
pdot_client *pdot_client_clone(pdot_client *c);

/* Fetches genesis and runtime version; refetches metadata if specVersion changed. */
int pdot_chain_sync(pdot_client *c);

#endif
