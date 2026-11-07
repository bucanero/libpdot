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
#define PDOT_E_UNVERIFIED  (-16) /* a proof from an RPC node did not verify */
#define PDOT_E_CANCELLED   (-17) /* abandoned through pdot_async_cancel or pdot_async_free */

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

/*
 * Signs with CheckMetadataHash (RFC-0078): the signature also covers a hash
 * of the metadata the transaction was built from, so if a node serves false
 * metadata the chain rejects the transaction instead of running a call you
 * did not mean. The hash includes the token symbol and decimals the runtime
 * was built with: NULL and -1 take them from the node's system_properties,
 * which match on the public Bulletin chains. A wrong value of any input only
 * gets transactions rejected. Off by default; ignored on runtimes without
 * the extension.
 */
int pdot_client_set_metadata_hash(pdot_client *c, int enable, const char *token_symbol,
                                  int decimals);
/* The hash transactions are signed with; PDOT_E_NOTFOUND if off or unsupported. */
int pdot_client_metadata_hash(pdot_client *c, uint8_t out[32]);

/* 1 when pdot_async asks the job running on c to stop (cancel or free). */
int pdot_client_cancelled(pdot_client *c);

/* The chain's native token symbol and decimals, from the node's system_properties. */
int pdot_client_token(pdot_client *c, char *symbol, size_t cap, uint8_t *decimals);

/* ---- Verification without trusting RPC nodes -------------------------

   With a verifier set, libpdot proves what it reports instead of believing
   the RPC node: relay-chain finality from GRANDPA signatures, the Bulletin
   block from the relay chain's Paras::Heads, and Bulletin storage from read
   proofs. It starts from a checkpoint (a trusted GRANDPA authority set) and
   advances it through authority-set changes; save it again with
   pdot_client_checkpoint so later runs start from there.

   Still trusted: the runtime metadata used to encode calls and to decode
   proven values, and the RPC nodes' liveness (they can refuse to answer). */

/*
 * Builds a checkpoint from a relay node's newest final block, TRUSTING that
 * node once. Ship the result with your app, or create it from a node you
 * trust. *blob is freed with pdot_free.
 */
int pdot_checkpoint_fetch(const char *relay_url, uint8_t **blob, size_t *len);
/* Same, anchored at a given final relay block (0: the newest). */
int pdot_checkpoint_fetch_at(const char *relay_url, uint32_t block, uint8_t **blob, size_t *len);

/* Turns verification on: relay_url is the relay chain's RPC, para_id the Bulletin chain's. */
int pdot_client_set_verifier(pdot_client *c, const char *relay_url, uint32_t para_id,
                             const uint8_t *checkpoint, size_t len);

/*
 * The current checkpoint, advanced past any set changes seen; save it. Once
 * the client is handed to pdot_async_new, use pdot_async_checkpoint.
 */
int pdot_client_checkpoint(pdot_client *c, uint8_t **blob, size_t *len);

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

/* An unsigned 128-bit integer, as balances are on chain (see Amounts). */
typedef struct {
    uint64_t lo, hi;
} pdot_u128;

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
    int      verified;   /* 1: proven from relay-chain finality (needs a verifier) */
    int      has_fee;
    pdot_u128 fee;       /* fee actually paid, tip included, from the *FeePaid event */
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
    uint32_t now;                     /* best block when read, or the proven block */
    int      verified;                /* 1: read through a proof (needs a verifier) */
} pdot_authorization;

/* Where data is stored, read from chain state (proven with a verifier). */
typedef struct {
    uint32_t block;        /* Bulletin block of the newest stored copy */
    uint32_t index;        /* its position in that block's stored transactions */
    uint32_t expires;      /* retained until this block (block + RetentionPeriod) */
    uint32_t proven_at;    /* final Bulletin block whose state was read */
    uint32_t relay_block;  /* with verified: relay block whose GRANDPA finality anchors it */
    int      verified;     /* 1: read through a proof */
} pdot_storage_proof;

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
 * Proves that the data behind cid is stored on the Bulletin chain and still
 * retained, without trusting the RPC nodes. Needs a verifier.
 * PDOT_E_NOTFOUND when the proof shows it is not stored.
 */
int pdot_bulletin_verify_cid(pdot_client *c, const pdot_cid *cid, pdot_storage_proof *out);

/*
 * Where the data behind cid is stored and until when it is retained, at the
 * newest final block: proven with a verifier, the RPC node's word without.
 * Renew (pdot_bulletin_renew) before `expires` to keep it. PDOT_E_NOTFOUND
 * when it is not stored.
 */
int pdot_bulletin_stored(pdot_client *c, const pdot_cid *cid, pdot_storage_proof *out);

/*
 * Fetches data by CID from the gateway, or from the node when the client has
 * none, and checks it against the CID. A dag-pb CID (a chunked file) is
 * resolved through its manifest: every block is fetched and verified, and the
 * chunks are returned concatenated. *data is freed with pdot_free.
 */
int pdot_bulletin_fetch(pdot_client *c, const pdot_cid *cid,
                        uint8_t **data, size_t *len);

/* ---- Amounts -------------------------------------------------------- */

/*
 * Decimal text of v scaled down by `decimals` places, trailing zeros trimmed:
 * 12345 with 3 decimals is "12.345". PDOT_E_BUFFER if out is too small; 64
 * bytes fit any amount with up to 20 decimals.
 */
int pdot_u128_format(pdot_u128 v, uint8_t decimals, char *out, size_t out_len);
/* v / 10^decimals as a double, for display; may round. */
double pdot_u128_to_double(pdot_u128 v, uint8_t decimals);
/*
 * Parses decimal text ("1.25") into the smallest unit, scaled up by
 * `decimals` places: 1.25 with 12 decimals is 1250000000000. PDOT_E_INVALID
 * on anything else, more fractional digits than decimals, or overflow.
 */
int pdot_u128_parse(const char *text, uint8_t decimals, pdot_u128 *out);

/* ---- Chain state at a final block ----------------------------------- */

/*
 * The block a value was read at. Reads happen at the newest final block:
 * with a verifier, one proven from relay-chain finality, with the value
 * proven against its state root; without one, the RPC node's finalized head.
 */
typedef struct {
    uint32_t number;
    uint8_t  hash[32];
    int      verified;     /* 1: proven (needs a verifier) */
    uint32_t relay_block;  /* with verified: the relay block whose finality anchors it */
} pdot_final_block;

/* A raw storage value. With pdot_storage_get, data is freed with pdot_free. */
typedef struct {
    int              exists;
    uint8_t         *data;
    size_t           len;
    pdot_final_block at;
} pdot_storage_value;

/* Reads any storage key; use it for state the typed calls below do not cover. */
int pdot_storage_get(pdot_client *c, const uint8_t *key, size_t key_len,
                     pdot_storage_value *out);

/* ---- Asset Hub: balances, assets and NFTs ----------------------------

   Reads of the chain the client points at; any chain with these pallets
   works. To prove them, set a verifier with the relay chain and the
   parachain's id (Asset Hub is 1000 on Polkadot, Kusama, Westend and Paseo). */

/* System.Account: the native token. Amounts are in the token's smallest unit. */
typedef struct {
    int              exists;
    uint32_t         nonce;
    pdot_u128        free, reserved, frozen;
    pdot_final_block at;
} pdot_balance;

int pdot_balance_get(pdot_client *c, const uint8_t account_id[32], pdot_balance *out);

/* Assets.Account: an account's holding of a fungible asset (pallet-assets). */
typedef struct {
    int              exists;
    pdot_u128        balance;
    int              frozen;  /* the account's holding is frozen or blocked */
    pdot_final_block at;
} pdot_asset_balance;

int pdot_asset_balance_get(pdot_client *c, uint32_t asset_id, const uint8_t account_id[32],
                           pdot_asset_balance *out);

/* Assets.Asset and Assets.Metadata of a fungible asset. */
typedef struct {
    int              exists;
    uint8_t          owner[32], issuer[32], admin[32], freezer[32];
    pdot_u128        supply, min_balance;
    uint32_t         accounts;    /* holders */
    int              sufficient;  /* can keep an account alive on its own */
    int              live;        /* not frozen or being destroyed */
    char             name[64];    /* from Metadata, NUL-terminated, may be cut short */
    char             symbol[32];
    uint8_t          decimals;
    pdot_final_block at;
} pdot_asset_info;

int pdot_asset_info_get(pdot_client *c, uint32_t asset_id, pdot_asset_info *out);

/* Nfts.Item and Nfts.ItemMetadataOf (pallet-nfts). */
#define PDOT_NFT_DATA_MAX 256

typedef struct {
    int              exists;
    uint8_t          owner[32];
    uint8_t          metadata[PDOT_NFT_DATA_MAX];  /* often an IPFS CID or a URL */
    size_t           metadata_len;
    int              has_metadata;
    pdot_final_block at;
} pdot_nft_item;

int pdot_nft_item_get(pdot_client *c, uint32_t collection, uint32_t item, pdot_nft_item *out);

/* Who set an attribute (AttributeNamespace). */
#define PDOT_NFT_NS_PALLET           0
#define PDOT_NFT_NS_COLLECTION_OWNER 1
#define PDOT_NFT_NS_ITEM_OWNER       2
#define PDOT_NFT_NS_ACCOUNT          3  /* an account approved to set attributes */

typedef struct {
    int              exists;
    uint8_t          value[PDOT_NFT_DATA_MAX];
    size_t           len;
    pdot_final_block at;
} pdot_nft_attribute;

/*
 * Nfts.Attribute: a key/value pair on an item (has_item 1) or on the
 * collection itself. ns_account is the account for PDOT_NFT_NS_ACCOUNT.
 */
int pdot_nft_attribute_get(pdot_client *c, uint32_t collection, int has_item, uint32_t item,
                           int ns, const uint8_t ns_account[32], const uint8_t *key,
                           size_t key_len, pdot_nft_attribute *out);

/*
 * Items of a collection the account owns (Nfts.Account), up to max. The node
 * lists them; with a verifier each one is then proven owned, so a node can
 * leave items out but cannot add any. *items is freed with pdot_free.
 */
int pdot_nft_owned(pdot_client *c, const uint8_t account_id[32], uint32_t collection,
                   size_t max, uint32_t **items, size_t *n, pdot_final_block *at);

/* ---- Transactions: any call, fees, transfers and proxies -------------

   For Asset Hub and any other chain the client points at. Calls are built
   from the runtime's metadata, so they follow its argument types. Where a
   transaction landed and how it went (dispatch outcome, fee) come from the
   RPC node's blocks and events; with a verifier they are proven (see
   pdot_tx_submit). */

/* Options for a transaction; pass NULL for none. */
typedef struct {
    pdot_u128      tip;        /* paid to the block author on top of the fee */
    /*
     * Sign as a proxy of this account: the call is wrapped in Proxy.proxy
     * and runs as that account, while the signer pays the fee. NULL: the
     * call runs as the signer.
     */
    const uint8_t *proxy_for;
} pdot_tx_options;

/* What a transaction would pay now (TransactionPaymentApi_query_info). */
typedef struct {
    pdot_u128 partial_fee;     /* base + length + weight fee; add the tip for the total */
    uint64_t  ref_time;        /* its weight */
    uint64_t  proof_size;
    uint8_t   dispatch_class;  /* 0 normal, 1 operational, 2 mandatory */
} pdot_fee;

/*
 * Encodes pallet.call from JSON arguments, an array in order or an object by
 * name: integers as numbers or decimal strings, accounts as SS58 or 0x hex,
 * bytes as 0x hex or text, enums as "Variant" or {"Variant": value}, null
 * for None. Messages name the argument that did not fit. *call is freed
 * with pdot_free.
 */
int pdot_call_from_json(pdot_client *c, const char *pallet, const char *call,
                        const char *json_args, uint8_t **call_out, size_t *len);

/*
 * Signs and submits an encoded call, then blocks until it is in a finalized
 * block (PDOT_E_TIMEOUT after the mortality window). PDOT_E_DISPATCH, with
 * out->error, when the call failed; through a proxy, also when the proxied
 * call failed although the transaction itself went through.
 *
 * With a verifier, the result is proven and out->verified set, a failed call
 * included: the block from relay-chain finality, the extrinsic's place in the
 * block body against the header's extrinsics root, and the outcome and fee
 * from a read proof of the block's System.Events. PDOT_E_UNVERIFIED when the
 * node's answers do not prove it. Every transaction call (transfers, proxies,
 * Bulletin store, renew and authorize) goes through here.
 */
int pdot_tx_submit(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                   const pdot_tx_options *o, pdot_tx_result *out);

/* The fee submitting would cost now. Signs, but submits nothing. */
int pdot_tx_fee(pdot_client *c, const pdot_signer *s, const uint8_t *call, size_t len,
                const pdot_tx_options *o, pdot_fee *out);

/*
 * Remarks as attestations: System.remark_with_event(data) records an event
 * with the dispatch origin and blake2_256(data), which anyone can check
 * later from the block number alone.
 */
typedef struct {
    pdot_final_block at;     /* the block searched; verified when proven */
    uint32_t         index;  /* the extrinsic that made the remark */
} pdot_remark;

/*
 * Looks in final block `block` for a System.Remarked event from sender over
 * exactly data. The sender is the call's origin: through a proxy, the
 * account acted for, not the proxy key. PDOT_E_NOTFOUND when the block has
 * none, PDOT_E_INVALID when the block is not final yet. With a verifier the
 * block's events are proven from relay-chain finality (PDOT_E_UNVERIFIED
 * otherwise). Events are decoded with the client's current metadata, so
 * blocks from before a runtime upgrade that changed them may not decode.
 */
int pdot_remark_find(pdot_client *c, uint32_t block, const uint8_t sender[32],
                     const uint8_t *data, size_t len, pdot_remark *out);

/* Transfers. "keep alive" ones fail rather than take the sender below the existential deposit. */
#define PDOT_TRANSFER_NATIVE 0  /* Balances.transfer_keep_alive: amount of the native token */
#define PDOT_TRANSFER_ASSET  1  /* Assets.transfer_keep_alive: amount of asset id */
#define PDOT_TRANSFER_NFT    2  /* Nfts.transfer: item of collection id */
#define PDOT_TRANSFER_ALL    3  /* Balances.transfer_all: all of the native token, closing the account */

typedef struct {
    int       kind;       /* PDOT_TRANSFER_* */
    uint8_t   dest[32];
    pdot_u128 amount;     /* smallest units; NATIVE and ASSET */
    uint32_t  id;         /* asset id, or NFT collection */
    uint32_t  item;       /* NFT item */
} pdot_transfer;

/* The encoded call for t, to submit, estimate or batch yourself. Freed with pdot_free. */
int pdot_transfer_call(pdot_client *c, const pdot_transfer *t, uint8_t **call, size_t *len);
/* pdot_transfer_call + pdot_tx_submit. */
int pdot_transfer_submit(pdot_client *c, const pdot_signer *s, const pdot_transfer *t,
                         const pdot_tx_options *o, pdot_tx_result *out);

/* ---- Proxy session keys ----------------------------------------------

   A game asks the player to sign once, letting a fresh session key act for
   their account (a proxy), and then signs moves with the session key alone.
   The session key pays its own fees, so fund it when adding it. Proxy
   types limit what it can do: "Any", "NonTransfer" (no transfers of the
   native token), "Assets" (pallet-assets and NFT calls), ... as the
   runtime's ProxyType lists them. Adding one reserves a deposit from the
   real account, returned on removal. */

/* A new random sr25519 key; seed_out (may be NULL) gets its seed, to save. */
int pdot_signer_sr25519_random(pdot_signer *s, uint8_t seed_out[32]);

/*
 * Proxy.add_proxy(delegate, proxy_type, delay 0), signed by the real account
 * s. With fund nonzero, the same transaction (Utility.batch_all) also sends
 * the delegate fund of the native token, all or nothing.
 */
int pdot_proxy_add(pdot_client *c, const pdot_signer *s, const uint8_t delegate[32],
                   const char *proxy_type, pdot_u128 fund, pdot_tx_result *out);
int pdot_proxy_remove(pdot_client *c, const pdot_signer *s, const uint8_t delegate[32],
                      const char *proxy_type, pdot_tx_result *out);

#define PDOT_PROXIES_MAX 32

typedef struct {
    uint8_t  delegate[32];
    char     type[32];  /* ProxyType variant name */
    uint32_t delay;     /* blocks an announcement must wait; 0 acts at once */
} pdot_proxy_def;

/* Proxy.Proxies: the accounts that may act for account_id. */
typedef struct {
    size_t           n;
    pdot_proxy_def   list[PDOT_PROXIES_MAX];
    pdot_u128        deposit;
    pdot_final_block at;
} pdot_proxies;

int pdot_proxies_get(pdot_client *c, const uint8_t account_id[32], pdot_proxies *out);

/* ---- Non-blocking use: pdot_async -----------------------------------

   For game loops and UI threads. pdot_async takes over a configured client
   and runs its blocking calls on a worker thread, one at a time in order.
   Results come back through callbacks that run inside pdot_async_poll, on
   whichever thread calls it (say once per frame), so callbacks can touch
   engine state without locking.

   Each call returns a job id (0 if it could not be queued) and invokes its
   callback exactly once. `result` points to the type named at each call,
   valid only during the callback; it is NULL when rc is not 0, except where
   noted. `error` is the message for rc. Requests can be made from any
   thread; pdot_async_poll from one thread at a time.

   Watches report a value when first read and again whenever it changes, at
   each new final block (proven with a verifier). They run on a second
   thread with their own connection, woken by a WebSocket subscription to
   finalized heads when libcurl supports WebSockets, else by polling. */

typedef struct pdot_async pdot_async;

typedef void (*pdot_result_fn)(void *ud, int rc, const char *error, const void *result);

/* Takes ownership of c, configured beforehand (verifier, metadata hash). */
pdot_async *pdot_async_new(pdot_client *c);

/*
 * Cancels everything and joins the threads, then runs every callback still
 * owed on the calling thread (unfinished jobs and watches with
 * PDOT_E_CANCELLED) so their user data can be released. Frees the client.
 */
void pdot_async_free(pdot_async *a);

/* Runs the callbacks for everything finished so far; returns how many ran. */
int pdot_async_poll(pdot_async *a);

/*
 * With a verifier: the checkpoint furthest along, from jobs and watches
 * alike (they verify on separate connections and share their progress).
 * Save it so the next run starts from there. *blob is freed with pdot_free;
 * PDOT_E_INVALID without a verifier.
 */
int pdot_async_checkpoint(pdot_async *a, uint8_t **blob, size_t *len);

/*
 * Optional: fn(ud, blob, len) runs inside pdot_async_poll each time the
 * checkpoint advances (at GRANDPA authority-set changes, every few hours on
 * live relay chains), so a long-running app can save it as it goes. blob is
 * valid during the call only.
 */
void pdot_async_set_checkpoint_fn(pdot_async *a,
                                  void (*fn)(void *ud, const uint8_t *blob, size_t len), void *ud);

/*
 * Optional: wake(ud) is called from a background thread whenever callbacks
 * are waiting, for loops that sleep instead of polling every frame.
 */
void pdot_async_set_wake(pdot_async *a, void (*wake)(void *ud), void *ud);

/*
 * Cancels a queued or running job (its callback gets PDOT_E_CANCELLED) or
 * ends a watch (one final callback with PDOT_E_CANCELLED). A running store
 * may still land on chain. PDOT_E_NOTFOUND if the id is not pending.
 */
int pdot_async_cancel(pdot_async *a, uint64_t id);

/*
 * Runs work(c, ud) on the worker thread with the async's client; the callback
 * gets its return code and no result. Long-running work should return
 * PDOT_E_CANCELLED once pdot_client_cancelled(c) is 1; library calls made
 * inside it already do.
 */
uint64_t pdot_async_run(pdot_async *a, int (*work)(pdot_client *c, void *ud),
                        pdot_result_fn done, void *ud);

/* Bulletin Chain. Data and signers are copied; an external signer's ctx must outlive the job. */
uint64_t pdot_async_store(pdot_async *a, const pdot_signer *s, const uint8_t *data,
                          size_t len, pdot_result_fn done, void *ud);       /* pdot_store_result */
/* progress (may be NULL) also runs inside pdot_async_poll. */
uint64_t pdot_async_store_chunked(pdot_async *a, const pdot_signer *s, const uint8_t *data,
                                  size_t len, size_t chunk_size, pdot_progress_fn progress,
                                  pdot_result_fn done, void *ud);           /* pdot_chunked_result */
uint64_t pdot_async_renew(pdot_async *a, const pdot_signer *s, const pdot_cid *cid,
                          pdot_result_fn done, void *ud);                   /* pdot_tx_result */
uint64_t pdot_async_verify_cid(pdot_async *a, const pdot_cid *cid,
                               pdot_result_fn done, void *ud);              /* pdot_storage_proof */
uint64_t pdot_async_authorization(pdot_async *a, const uint8_t account_id[32],
                                  pdot_result_fn done, void *ud);           /* pdot_authorization */

/* Fetched data, for pdot_async_fetch. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} pdot_bytes;

uint64_t pdot_async_fetch(pdot_async *a, const pdot_cid *cid,
                          pdot_result_fn done, void *ud);                   /* pdot_bytes */

/* Asset Hub reads. */
uint64_t pdot_async_balance(pdot_async *a, const uint8_t account_id[32],
                            pdot_result_fn done, void *ud);                 /* pdot_balance */
uint64_t pdot_async_asset_balance(pdot_async *a, uint32_t asset_id, const uint8_t account_id[32],
                                  pdot_result_fn done, void *ud);           /* pdot_asset_balance */
uint64_t pdot_async_asset_info(pdot_async *a, uint32_t asset_id,
                               pdot_result_fn done, void *ud);              /* pdot_asset_info */
uint64_t pdot_async_nft_item(pdot_async *a, uint32_t collection, uint32_t item,
                             pdot_result_fn done, void *ud);                /* pdot_nft_item */

/* Owned items, for pdot_async_nft_owned. */
typedef struct {
    const uint32_t  *items;
    size_t           n;
    pdot_final_block at;
} pdot_nft_list;

uint64_t pdot_async_nft_owned(pdot_async *a, const uint8_t account_id[32], uint32_t collection,
                              size_t max, pdot_result_fn done, void *ud);   /* pdot_nft_list */

/* Transactions. The call, signer and options are copied. */
uint64_t pdot_async_submit(pdot_async *a, const pdot_signer *s, const uint8_t *call,
                           size_t len, const pdot_tx_options *o,
                           pdot_result_fn done, void *ud);                  /* pdot_tx_result */
uint64_t pdot_async_fee(pdot_async *a, const pdot_signer *s, const uint8_t *call, size_t len,
                        const pdot_tx_options *o, pdot_result_fn done, void *ud); /* pdot_fee */
uint64_t pdot_async_transfer(pdot_async *a, const pdot_signer *s, const pdot_transfer *t,
                             const pdot_tx_options *o, pdot_result_fn done, void *ud);
                                                                            /* pdot_tx_result */
uint64_t pdot_async_proxies(pdot_async *a, const uint8_t account_id[32],
                            pdot_result_fn done, void *ud);                 /* pdot_proxies */

/*
 * Watches: the callback runs repeatedly until pdot_async_cancel or
 * pdot_async_free. A failed read is reported too (rc < 0, no result) and
 * retried; the value is reported again once reads work.
 */
uint64_t pdot_async_watch_head(pdot_async *a, pdot_result_fn fn, void *ud); /* pdot_final_block */
uint64_t pdot_async_watch_storage(pdot_async *a, const uint8_t *key, size_t key_len,
                                  pdot_result_fn fn, void *ud);             /* pdot_storage_value */
uint64_t pdot_async_watch_balance(pdot_async *a, const uint8_t account_id[32],
                                  pdot_result_fn fn, void *ud);             /* pdot_balance */
uint64_t pdot_async_watch_asset_balance(pdot_async *a, uint32_t asset_id,
                                        const uint8_t account_id[32],
                                        pdot_result_fn fn, void *ud);       /* pdot_asset_balance */
uint64_t pdot_async_watch_nft_item(pdot_async *a, uint32_t collection, uint32_t item,
                                   pdot_result_fn fn, void *ud);            /* pdot_nft_item */

#ifdef __cplusplus
}
#endif

#endif /* PDOT_H */
