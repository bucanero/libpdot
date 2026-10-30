/* Substrate headers and GRANDPA justification verification. */
#ifndef PDOT_GRANDPA_H
#define PDOT_GRANDPA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  hash[32];        /* Blake2b-256 of the encoded header */
    uint8_t  parent[32];
    uint32_t number;
    uint8_t  state_root[32];
    uint8_t  extrinsics_root[32];
    size_t   encoded_len;
} pdot_header;

/* Parses one SCALE-encoded header at the start of p; sets encoded_len. */
int pdot_header_decode(const uint8_t *p, size_t len, pdot_header *h);

typedef struct {
    uint8_t  id[32];          /* ed25519 public key */
    uint64_t weight;
} pdot_grandpa_authority;

typedef struct {
    pdot_grandpa_authority *authorities;
    size_t                  nauthorities;
    uint64_t                set_id;
} pdot_grandpa_set;

/* Decodes Vec<(AuthorityId, u64)> as returned by GrandpaApi_grandpa_authorities. */
int pdot_grandpa_set_decode(const uint8_t *p, size_t len, uint64_t set_id, pdot_grandpa_set *set);
void pdot_grandpa_set_free(pdot_grandpa_set *set);

/*
 * Finds a GRANDPA authority-set change in an encoded header's digest.
 * 0 and *next filled (set_id left 0) for a standard ScheduledChange enacted
 * delay blocks later; PDOT_E_UNSUPPORTED for a ForcedChange; PDOT_E_NOTFOUND.
 */
int pdot_grandpa_scheduled_change(const uint8_t *header, size_t len,
                                  pdot_grandpa_set *next, uint32_t *delay);

typedef struct {
    size_t   precommits;      /* in the justification */
    size_t   signers;         /* distinct, valid, counted */
    uint64_t weight;          /* their total weight */
    uint64_t threshold;       /* needed weight */
} pdot_grandpa_stats;

/*
 * Verifies an encoded GrandpaJustification finalizes target (hash, number)
 * under set. 0 on success; PDOT_E_CRYPTO for a bad signature or unknown
 * signer; PDOT_E_MISMATCH for a wrong target, set or ancestry; PDOT_E_DECODE.
 */
int pdot_grandpa_verify(const pdot_grandpa_set *set, const uint8_t *justification,
                        size_t len, uint8_t target_hash[32], uint32_t *target_number,
                        pdot_grandpa_stats *stats);

#endif
