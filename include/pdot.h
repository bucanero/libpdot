/*
 * libpdot - store and retrieve data on the Polkadot Bulletin Chain.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every call returns 0 or a negative PDOT_E_* code.
 */
#ifndef PDOT_H
#define PDOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDOT_VERSION_MAJOR 0
#define PDOT_VERSION_MINOR 1
#define PDOT_VERSION_PATCH 0

/* Error codes */
#define PDOT_OK              0
#define PDOT_E_INVALID     (-1)  /* bad argument */
#define PDOT_E_NOMEM       (-2)
#define PDOT_E_DECODE      (-3)  /* malformed input data */
#define PDOT_E_BUFFER      (-4)  /* output buffer too small */
#define PDOT_E_CHECKSUM    (-5)
#define PDOT_E_UNSUPPORTED (-6)  /* valid but not handled by this build */
#define PDOT_E_MISMATCH    (-7)  /* data does not match the expected hash */
#define PDOT_E_CRYPTO      (-8)  /* invalid key or crypto backend failure */
#define PDOT_E_RNG         (-9)  /* random number generator unavailable */
#define PDOT_E_NETWORK     (-10) /* transport failure or non-2xx HTTP status */
#define PDOT_E_RPC         (-11) /* the node returned a JSON-RPC error */
#define PDOT_E_NOTFOUND    (-12)
#define PDOT_E_TIMEOUT     (-13) /* e.g. not included within the mortality window */
#define PDOT_E_METADATA    (-14) /* metadata lacks or changes something we need */
#define PDOT_E_DISPATCH    (-15) /* included, but the call failed (see error) */

const char *pdot_strerror(int err);

/* Frees memory that the library returned to the caller. */
void pdot_free(void *p);

/* ---- Client ---------------------------------------------------------- */

typedef struct pdot_client pdot_client;

/*
 * rpc_url is an HTTP(S) JSON-RPC endpoint (a node's ws:// port also answers
 * HTTP). gateway_url is an IPFS HTTP gateway for pdot_bulletin_fetch, or NULL
 * to fetch from the node itself with bitswap_v1_get.
 */
pdot_client *pdot_client_new(const char *rpc_url, const char *gateway_url);
void         pdot_client_free(pdot_client *c);
/* Message for the last failed call on this client. */
const char  *pdot_last_error(const pdot_client *c);
/* Per-request timeout; default 60 s. */
void         pdot_client_set_timeout(pdot_client *c, long timeout_ms);

/* ---- Signers -------------------------------------------------------- */

/* MultiSignature variant indices. */
#define PDOT_SCHEME_SR25519 1
#define PDOT_SCHEME_ECDSA   2

#define PDOT_SIG_MAX 65

/*
 * A signer produces a MultiSignature body over raw bytes. Built-in signers
 * keep their key in `secret`; external signers (HSM, wallet bridge) set
 * `sign` and `ctx` themselves and leave `secret` unused.
 */
typedef struct pdot_signer {
    uint8_t  scheme;          /* PDOT_SCHEME_* */
    uint8_t  account_id[32];  /* AccountId32 used in MultiAddress::Id */
    uint8_t  public_key[33];  /* 32 bytes sr25519, 33 bytes compressed ECDSA */
    size_t   public_key_len;
    size_t   sig_len;         /* 64 for sr25519, 65 for ECDSA */
    int    (*sign)(const struct pdot_signer *s, const uint8_t *msg, size_t len,
                   uint8_t *sig_out);
    void    *ctx;
    uint8_t  secret[96];
} pdot_signer;

/* Seeds are the 32-byte "Secret seed" printed by `subkey inspect`. */
int pdot_signer_sr25519_from_seed(pdot_signer *s, const uint8_t seed[32]);
int pdot_signer_ecdsa_from_seed(pdot_signer *s, const uint8_t seed[32]);

/* Signs msg as-is; sig_out must hold s->sig_len bytes. */
int pdot_signer_sign(const pdot_signer *s, const uint8_t *msg, size_t len,
                     uint8_t *sig_out);

/* Zeroes the key material. */
void pdot_signer_wipe(pdot_signer *s);

/* ---- Content identifiers ------------------------------------------- */

/*
 * A binary CID. A CIDv1 with a Blake2b-256 multihash is 38 bytes:
 * version (1) | codec varint | multihash code varint (3) | length (1) | digest (32).
 */
#define PDOT_CID_MAX 64

typedef struct {
    uint8_t bytes[PDOT_CID_MAX];
    size_t  len;
} pdot_cid;

#define PDOT_CODEC_RAW      0x55
#define PDOT_CODEC_DAG_PB   0x70
#define PDOT_MH_SHA2_256    0x12
#define PDOT_MH_BLAKE2B_256 0xb220

/* CIDv1, raw codec, Blake2b-256 multihash of data. */
int pdot_cid_compute(pdot_cid *cid, const uint8_t *data, size_t len);
/* Same with another codec, e.g. PDOT_CODEC_DAG_PB for a chunked-file manifest. */
int pdot_cid_compute_codec(pdot_cid *cid, uint64_t codec, const uint8_t *data, size_t len);
/* Codec of a CIDv1, or 0 if cid is not one. */
uint64_t pdot_cid_codec(const pdot_cid *cid);

/* Returns 0 if data hashes to the digest in cid, PDOT_E_MISMATCH if not. */
int pdot_cid_verify(const pdot_cid *cid, const uint8_t *data, size_t len);

/* Multihash code and digest of a CIDv1; digest points into cid. */
int pdot_cid_digest(const pdot_cid *cid, uint64_t *mh_code,
                    const uint8_t **digest, size_t *digest_len);

/* Multibase base32 ("b..."), as used in /ipfs/<cid> paths. */
int pdot_cid_to_string(const pdot_cid *cid, char *out, size_t out_len);
int pdot_cid_from_string(pdot_cid *cid, const char *str);

/* ---- SS58 addresses ------------------------------------------------- */

#define PDOT_SS58_MAX_LEN 64  /* buffer size that fits any address + NUL */

int pdot_ss58_encode(const uint8_t account_id[32], uint16_t prefix,
                     char *out, size_t out_len);
int pdot_ss58_decode(const char *address, uint8_t account_id[32],
                     uint16_t *prefix);

/* ---- Bulletin Chain ------------------------------------------------- */

/* Dispatch outcome, read from the block's System.Events. */
#define PDOT_DISPATCH_UNKNOWN (-1)  /* events unavailable (pruned state) */
#define PDOT_DISPATCH_FAILED    0
#define PDOT_DISPATCH_OK        1

/* Where a submitted transaction landed in a finalized block, and how it went. */
typedef struct {
    uint8_t  tx_hash[32];
    uint8_t  block_hash[32];
    uint32_t block;
    uint32_t index;      /* extrinsic index in the block */
    int      dispatch;   /* PDOT_DISPATCH_* */
    char     error[96];  /* "Pallet.Error" when dispatch failed */
} pdot_tx_result;

typedef struct {
    pdot_cid       cid;  /* the input for renew */
    pdot_tx_result tx;
    int            has_storage_index;
    uint32_t       storage_index;  /* TransactionStorage.Stored { index } */
} pdot_store_result;

/* An account's TransactionStorage.Authorizations entry. */
typedef struct {
    int      exists;
    uint32_t transactions;            /* used */
    uint32_t transactions_allowance;
    uint64_t bytes;                   /* used */
    uint64_t bytes_allowance;
    uint32_t expiration;              /* expired once the chain reaches this block */
    uint32_t now;                     /* best block when read */
} pdot_authorization;

/* Reads and decodes the account's authorization, if any. */
int pdot_bulletin_authorization(pdot_client *c, const uint8_t account_id[32],
                                pdot_authorization *out);

/*
 * 1 when an unexpired authorization exists. store is accepted past the byte
 * allowance (at lower priority); renew is not.
 */
int pdot_bulletin_is_authorized(pdot_client *c, const uint8_t account_id[32],
                                int *authorized);

/*
 * Submits TransactionStorage.store(data) and blocks until it is in a
 * finalized block, or PDOT_E_TIMEOUT once the mortality window has passed.
 * Returns PDOT_E_DISPATCH, with out->tx.error set, if the call failed.
 */
int pdot_bulletin_store(pdot_client *c, const pdot_signer *s,
                        const uint8_t *data, size_t len,
                        pdot_store_result *out);

/*
 * DataRenewal.force_renew(TransactionRef::ContentHash) for the data behind
 * cid, which must use a 32-byte digest. The chain resolves the hash to the
 * most recent copy, so no (block, index) bookkeeping is needed.
 */
int pdot_bulletin_renew(pdot_client *c, const pdot_signer *s,
                        const pdot_cid *cid, pdot_tx_result *out);

/* ---- Chunked files (UnixFS / DAG-PB, compatible with the TypeScript SDK) */

#define PDOT_CHUNK_SIZE_DEFAULT (1u << 20)   /* 1 MiB */
#define PDOT_CHUNK_SIZE_MAX     (2u << 20)   /* the chain's MaxTransactionSize */
#define PDOT_FILE_SIZE_MAX      (64u << 20)

/*
 * Called after each transaction of a multi-transaction operation: chunks
 * 0..total-2, then the manifest at index total-1. rc is that step's result.
 */
typedef void (*pdot_progress_fn)(void *ud, size_t index, size_t total,
                                 const pdot_cid *cid, const pdot_tx_result *tx, int rc);

typedef struct {
    pdot_cid          root;      /* dag-pb manifest CID: fetch and renew with this */
    pdot_store_result manifest;
    size_t            nchunks;
    uint64_t          size;
} pdot_chunked_result;

/*
 * Stores data as raw chunks of chunk_size bytes (0: 1 MiB) followed by a
 * DAG-PB manifest stored with codec dag-pb, as the Bulletin SDKs do. The root
 * CID matches the TypeScript SDK's for the same data and chunk size.
 * Transactions go one at a time; on failure, out->nchunks says how far it got.
 */
int pdot_bulletin_store_chunked(pdot_client *c, const pdot_signer *s,
                                const uint8_t *data, size_t len, size_t chunk_size,
                                pdot_progress_fn progress, void *ud,
                                pdot_chunked_result *out);

/* Renews every chunk behind a manifest, then the manifest; or just root if raw. */
int pdot_bulletin_renew_tree(pdot_client *c, const pdot_signer *s, const pdot_cid *root,
                             pdot_progress_fn progress, void *ud);

/*
 * TransactionStorage.authorize_account, signed by s. Succeeds only when s is
 * a registered authorizer (AllowedAuthorizers); Root grants go through sudo.
 */
int pdot_bulletin_authorize_account(pdot_client *c, const pdot_signer *s,
                                    const uint8_t who[32], uint32_t transactions,
                                    uint64_t bytes, pdot_tx_result *out);

/*
 * Fetches data by CID from the gateway, or from the node when the client has
 * none, and checks it against the CID. A dag-pb CID (a chunked file) is
 * resolved through its manifest: every block is fetched and verified, and the
 * chunks are returned concatenated. *data is freed with pdot_free.
 */
int pdot_bulletin_fetch(pdot_client *c, const pdot_cid *cid,
                        uint8_t **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* PDOT_H */
