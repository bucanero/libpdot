/*
 * WebSocket subscriptions over libcurl (7.86+ built with WebSocket support).
 * Used only to learn promptly that a new block was finalized; reads still go
 * over HTTP. Where libcurl lacks WebSockets every call is PDOT_E_UNSUPPORTED
 * and callers poll instead.
 */
#ifndef PDOT_WS_H
#define PDOT_WS_H

#include "client.h"

typedef struct pdot_ws pdot_ws;

/* 1 if this libcurl speaks ws:// and wss://. */
int pdot_ws_available(void);

/*
 * Connects to the node behind http_url (http -> ws, https -> wss) and sends
 * one JSON-RPC subscription request. c supplies the timeout, stop hook and
 * error message.
 */
int pdot_ws_subscribe(pdot_client *c, const char *http_url, const char *method,
                      pdot_ws **out);

/*
 * Reads whatever has arrived without blocking. *events is the number of
 * complete messages; an error reply to the subscription or a closed
 * connection is PDOT_E_NETWORK.
 */
int pdot_ws_drain(pdot_client *c, pdot_ws *ws, unsigned *events);

void pdot_ws_close(pdot_ws *ws);

#endif
