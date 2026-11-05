/*
 * Verification without trusting RPC nodes: a relay-chain light client.
 *
 *   checkpoint      trusted GRANDPA set (id + authorities) and its anchor block
 *   relay finality  GRANDPA justifications, walking authority-set changes
 *   para head       read proof of Paras::Heads(para_id) in a final relay block
 *   para state      read proofs against that Bulletin header's state root
 */
#ifndef PDOT_VERIFY_H
#define PDOT_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"
#include "grandpa.h"
#include "rpc.h"

typedef struct pdot_verifier {
    pdot_client     *relay;
    uint32_t         para_id;
    uint8_t          genesis[32];    /* relay genesis, from the checkpoint */
    pdot_grandpa_set set;            /* the set finalizing blocks after the anchor */
    uint32_t         anchor_number;
    uint8_t          anchor_hash[32];
    int              have_head;
    pdot_header      head;           /* newest proven Bulletin header */
    uint32_t         head_relay;     /* relay block that proved it */
} pdot_verifier;

void pdot_verifier_free(pdot_verifier *v);

/*
 * A Bulletin header proven final with number >= min_number, polling relay
 * finality until the client's verify timeout. Reuses the last proof when new
 * enough.
 */
int pdot_verify_para_head(pdot_client *c, uint32_t min_number, const pdot_header **head);

/*
 * The parachain header at the newest final relay block the relay node can
 * prove right now, without waiting for a newer one.
 */
int pdot_verify_newest_head(pdot_client *c, const pdot_header **head);

/*
 * Reads key from the Bulletin state at head through a read proof. *value is a
 * fresh allocation; PDOT_E_NOTFOUND when the proof shows the key absent.
 */
int pdot_verify_read(pdot_client *c, const pdot_header *head, const uint8_t *key,
                     size_t key_len, uint8_t **value, size_t *value_len);

/* Reads several keys at head through one read proof; absent keys are not present. */
int pdot_verify_read_many(pdot_client *c, const pdot_header *head, const uint8_t *const *keys,
                          const size_t *lens, size_t nkeys, pdot_value *values);

#endif
