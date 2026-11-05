# libpdot

[![CI](https://github.com/bucanero/libpdot/actions/workflows/ci.yml/badge.svg)](https://github.com/bucanero/libpdot/actions/workflows/ci.yml)

**Store and retrieve data on Polkadot from plain C.** libpdot is a portable C99 library, plus a small command-line tool, for the [Polkadot Bulletin Chain](https://github.com/paritytech/polkadot-bulletin-chain): a chain that stores files, content-addressed by CID and served over IPFS.

It is for C programs that want Polkadot without a Rust toolchain or a JavaScript runtime: games, desktop apps, embedded and IoT devices, emulators, command-line tools, and bindings for other languages through C's ABI. It also reads balances, assets and NFTs on [Asset Hub](https://docs.polkadot.com/polkadot-protocol/architecture/system-chains/asset-hub/), proven from relay-chain finality if you want, and has a non-blocking API for game loops and UI threads.

```c
pdot_client *c = pdot_client_new("https://paseo-bulletin-next-rpc.polkadot.io", NULL);
pdot_bulletin_store(c, &signer, data, len, &res);   /* waits for finalization  */
pdot_bulletin_fetch(c, &res.cid, &bytes, &n);       /* verified against the CID */
```

> **Status: proof of concept.** It works end to end on a local dev node and on Paseo Bulletin (TestNet), and is tested on x86_64, arm64, 32-bit ARM and big-endian s390x. The API may still change, and it has not been audited. Storing on Polkadot mainnet's Bulletin chain is not supported yet; reads and proofs work on mainnet.

## What it does

- **Store** data up to 2 MiB in one transaction, and files up to 64 MiB as chunks plus a [UnixFS](https://github.com/ipfs/specs/blob/main/UNIXFS.md) manifest, laid out byte for byte as the official Bulletin TypeScript SDK does.
- **Retrieve** by CID straight from a node, or through any IPFS HTTP gateway. Every byte is checked against the CID's hash, so the node and the gateway don't have to be trusted.
- **Renew** stored data before its retention period (about 14 days on testnets) runs out.
- **Check and grant storage authorizations**: allowances, usage and expiry.
- **Sign** with sr25519 (what Polkadot wallets use) or ECDSA secp256k1, or plug in your own signer: a hardware wallet, a secure enclave or a remote service.
- **Prove results without trusting the RPC node** (optional): a built-in relay-chain light client checks GRANDPA finality signatures and storage proofs, so "stored", "renewed" and "authorized" are facts, not the node's word. Signing over an RFC-0078 metadata hash means a node can't trick you into signing a different call either.
- **Read balances, assets and NFTs** on Asset Hub, or any chain with those pallets: native and asset balances, asset details, NFT owners, metadata and attributes, and the items of a collection an account owns. Each value is read at a final block, and proven like everything else when a verifier is set.
- **Never block your frame loop**: `pdot_async` runs calls on a worker thread and hands results to callbacks when you poll, once per frame. Watches report a balance, an asset holding or an NFT owner again whenever a new final block changes it.
- **Report what happened on chain**: the block and position of every transaction, and whether it succeeded. Failures come back by name, such as `TransactionStorage.BadDataSize`.

Under the hood it builds and signs Substrate transactions from the chain's own metadata (v14/v15). Runtime upgrades that change call indices or transaction extensions are picked up automatically.

## Quick start

### 1. Install the dependencies

libpdot needs a C99 compiler, CMake 3.16+, [mbedTLS 3.6 LTS](https://github.com/Mbed-TLS/mbedtls) (not 4.x), [libcurl](https://curl.se/libcurl/) and [cJSON](https://github.com/DaveGamble/cJSON).

| Platform | Command |
| --- | --- |
| macOS (Homebrew) | `brew install cmake mbedtls@3 cjson` (curl ships with macOS) |
| Debian 13 (trixie) and later | `apt install cmake gcc libmbedtls-dev libcurl4-openssl-dev libcjson-dev` |
| Ubuntu 24.04 and older | Ships mbedTLS 2.28: build [mbedTLS 3.6](https://github.com/Mbed-TLS/mbedtls/releases) from source, or build inside a `debian:trixie` container |
| Windows | Not tested yet |

### 2. Build and test

```sh
git clone --recurse-submodules https://github.com/bucanero/libpdot
cd libpdot
cmake -S . -B build                 # macOS: add -DCMAKE_PREFIX_PATH="$(brew --prefix mbedtls@3);$(brew --prefix cjson)"
cmake --build build
ctest --test-dir build
```

The build produces `libpdot.a`, the `pdot-bulletin` tool and the example programs.

### 3. Get an account with storage rights

Storing data needs an **authorization** on chain; reading never does.

- **Local dev node** (recommended to start). `//Alice` is already authorized. Run a node with [`tools/e2e-devnode.sh`](tools/e2e-devnode.sh), or follow the [Bulletin Chain quickstart](https://github.com/paritytech/polkadot-bulletin-chain#quickstart). The dev keys below are public test keys; never use them on a real network.
- **Paseo Bulletin (TestNet)**. Request an authorization for your account in the [Bulletin Chain Console](https://paritytech.github.io/polkadot-bulletin-chain/).

Keys load from a 32-byte hex **seed**, the "Secret seed" that [`subkey inspect`](https://github.com/paritytech/polkadot-sdk/tree/master/substrate/bin/utils/subkey) prints:

| Dev account | Scheme | Seed |
| --- | --- | --- |
| `//Alice` | sr25519 | `e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a` |
| `//Alice` | ECDSA | `cb6df9de1efca7a3998a8ead4e02159d5fa99c3e0d4fd6432667390bb4726854` |

### 4. Try the command-line tool

```sh
export PDOT_SEED=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
./build/pdot-bulletin address
./build/pdot-bulletin authorized
./build/pdot-bulletin store photo.png                 # prints the CID; over 2 MiB it is chunked
./build/pdot-bulletin fetch <cid> copy.png
./build/pdot-bulletin renew <cid>
./build/pdot-bulletin roundtrip photo.png             # all of the above, checked
```

Add `--rpc <url>` for another node, `--scheme ecdsa` for ECDSA keys, and `--gateway http://127.0.0.1:8080` to read through an IPFS gateway. Run `pdot-bulletin --help` for every option.

## Using the library

Everything is in one header, [`include/pdot.h`](include/pdot.h). Calls return `0` or a negative `PDOT_E_*` code; `pdot_strerror(rc)` names the code and `pdot_last_error(client)` gives the details.

### Store and fetch

From [`examples/store_and_fetch.c`](examples/store_and_fetch.c):

```c
#include <pdot.h>

pdot_client *c = pdot_client_new("http://127.0.0.1:9944", NULL);  /* NULL: no gateway */
pdot_signer signer;
pdot_signer_sr25519_from_seed(&signer, seed);

int authorized;
pdot_bulletin_is_authorized(c, signer.account_id, &authorized);

pdot_store_result res;
int rc = pdot_bulletin_store(c, &signer, (const uint8_t *)msg, strlen(msg), &res);
if (rc)
    fprintf(stderr, "store: %s (%s)\n", pdot_strerror(rc), pdot_last_error(c));

char cid[128];
pdot_cid_to_string(&res.cid, cid, sizeof cid);   /* "bafk2bzace..." */

uint8_t *data;
size_t len;
if (pdot_bulletin_fetch(c, &res.cid, &data, &len) == 0) {
    /* data matches the CID's hash */
    pdot_free(data);
}

pdot_signer_wipe(&signer);
pdot_client_free(c);
```

`pdot_bulletin_store` returns once the transaction is in a finalized block. `res.tx` then holds the block number, the transaction hash, and `dispatch`, which says whether the call succeeded on chain. If it failed, `rc` is `PDOT_E_DISPATCH` and `res.tx.error` holds the reason.

### Large files

[`examples/chunked_upload.c`](examples/chunked_upload.c) stores a file of up to 64 MiB as 1 MiB chunks plus a manifest and reports progress per chunk:

```c
pdot_chunked_result res;
rc = pdot_bulletin_store_chunked(c, &signer, data, len, PDOT_CHUNK_SIZE_DEFAULT,
                                 progress_cb, NULL, &res);
/* res.root is the file's CID: an IPFS gateway serves the whole file under it */
```

`pdot_bulletin_fetch(c, &res.root, ...)` returns the reassembled file after checking every chunk, and `pdot_bulletin_renew_tree` renews all of them. The root CID matches what the Bulletin TypeScript SDK produces for the same file.

### Your own signer

Keys don't have to live in your process. A `pdot_signer` is a callback plus a public key, so signing can happen in a hardware wallet, a secure enclave, a game server or a wallet app. See [`examples/custom_signer.c`](examples/custom_signer.c):

```c
pdot_signer s = {0};
s.scheme = PDOT_SCHEME_SR25519;
memcpy(s.public_key, device_public_key, 32);
s.public_key_len = 32;
memcpy(s.account_id, device_public_key, 32);   /* for sr25519, account = key */
s.sig_len = 64;
s.sign = device_sign;     /* int (*)(const pdot_signer *, const uint8_t *msg, size_t, uint8_t *sig) */
s.ctx = my_device;
```

### Content IDs without a network

CIDs are computed locally, so you can check a download from anywhere, from any IPFS gateway or a cache:

```c
pdot_cid cid;
pdot_cid_from_string(&cid, "bafk2bzace...");
if (pdot_cid_verify(&cid, bytes, len) == 0) {
    /* bytes are exactly what was stored */
}
```

### Verify instead of trusting the node

By default libpdot believes what the RPC node says about chain state. Give the client a **verifier** and it proves it instead:

1. Relay-chain finality, from GRANDPA signatures by 2/3 of the validator set, following validator-set changes from a checkpoint.
2. The Bulletin block, from the relay chain's record of parachain heads (`Paras::Heads`).
3. Bulletin storage, from Merkle read proofs against that block.

```c
uint8_t *ck; size_t ck_len;           /* a checkpoint: load it from your app's files */
pdot_client_set_verifier(c, "https://rpc.polkadot.io", 1010 /* Bulletin para id */, ck, ck_len);

pdot_storage_proof p;
if (pdot_bulletin_verify_cid(c, &cid, &p) == 0)
    printf("stored at block #%u, retained until #%u\n", p.block, p.expires);

/* store/renew/authorize now set res.tx.verified, or fail with PDOT_E_UNVERIFIED */
pdot_client_checkpoint(c, &ck, &ck_len);   /* save it: later runs start from here */
```

The **checkpoint** is the one thing you must trust: a GRANDPA validator set at some relay block, a few KB. Create it once from a node you trust and ship it with your app; libpdot advances it through validator-set changes (one every few hours) and hands the newer one back for you to save. `pdot_checkpoint_fetch` creates one, trusting the given node for that call only.

| Network | Relay RPC | Bulletin para id |
| --- | --- | --- |
| Polkadot | `https://rpc.polkadot.io` | 1010 |
| Paseo (TestNet) | `https://pas-rpc.stakeworld.io` | 1501 |
| Westend (TestNet) | `https://westend-rpc.polkadot.io` | 2487 |

From the command line:

```sh
pdot-bulletin --relay https://rpc.polkadot.io --checkpoint dot.ck checkpoint     # once
pdot-bulletin --rpc https://bulletin-rpc.polkadot.io --relay https://rpc.polkadot.io \
              --para-id 1010 --checkpoint dot.ck verify <cid>
```

A proof costs a few KB to about 60 KB and 1 to 20 ms of signature checks on a desktop, depending on the network's validator count (Polkadot has 600). Still trusted with a verifier: the nodes' willingness to answer. A node can refuse or stall, but it can no longer lie undetected. Turn on the metadata hash too (next section), so the metadata used to build transactions isn't trusted either.

### Sign over the metadata

Transactions are built from the chain's metadata, which comes from the RPC node. A node that serves altered metadata could make you sign a different call than the one you meant. With the metadata hash on, the signature also covers an [RFC-0078](https://polkadot-fellows.github.io/RFCs/approved/0078-merkleized-metadata.html) hash of that metadata, and the runtime checks it against the hash it was built with (`CheckMetadataHash`). Altered metadata then gets the transaction rejected, never run.

```c
pdot_client_set_metadata_hash(c, 1, NULL, -1);   /* on; token from the node's properties */
```

The hash also covers the token symbol and decimals the runtime was built with. On the public Bulletin chains these match the node's `system_properties`, so `NULL, -1` is right. A local chain spec may differ: pass the values explicitly (`"WND", 12` for the Westend runtime). A wrong value can only get transactions rejected, with a "bad signature" error that says so. The hash takes about 5 ms to compute and is cached until the next runtime upgrade. The command-line tool turns it on by default (`--metadata-hash on|off|SYMBOL:DECIMALS`), and `pdot-bulletin metadata-hash` prints it.

### Read balances, assets and NFTs

Point a client at Asset Hub (or any chain with `pallet-balances`, `pallet-assets` or `pallet-nfts`) and read its state. Values come from the newest final block, and the result says which.

```c
pdot_client *c = pdot_client_new("https://polkadot-asset-hub-rpc.polkadot.io", NULL);

pdot_asset_info usdt;
pdot_asset_info_get(c, 1984, &usdt);                  /* "Tether USD", USDt, 6 decimals */

pdot_nft_item sword;
pdot_nft_item_get(c, 752, 56, &sword);                /* owner and metadata (often ipfs://...) */
int owns = sword.exists && memcmp(sword.owner, player, 32) == 0;

pdot_balance b;
char text[64];
pdot_balance_get(c, player, &b);
pdot_u128_format(b.free, 10, text, sizeof text);      /* balances are 128-bit: "293.325" */
```

With a verifier (relay chain plus `1000`, Asset Hub's parachain id on every network), each value is proven, absence included. All the keys of one read share a single proof. `pdot_nft_owned` lists the items of a collection an account owns: the node does the listing, so it could leave items out, but every item it returns is proven owned. Also available: `pdot_asset_balance_get`, `pdot_nft_attribute_get` for item and collection attributes (game stats, for example), and `pdot_storage_get` for any other storage key.

```sh
pdot-bulletin --rpc https://polkadot-asset-hub-rpc.polkadot.io --relay https://rpc.polkadot.io \
              --para-id 1000 --checkpoint dot.ck nft 752 56
```

### Without blocking: `pdot_async`

Every call above blocks: a store waits for finalization, and a proven read takes a few round trips. `pdot_async` takes over a configured client, runs calls on a worker thread in order, and runs your callbacks inside `pdot_async_poll`, on your thread. Call that once per frame.

```c
pdot_async *a = pdot_async_new(c);                    /* c is now owned by a */

pdot_async_store(a, &signer, replay, len, on_stored, game);         /* -> pdot_store_result */
pdot_async_watch_nft_item(a, 752, 56, on_sword, game);              /* now, and on each change */
pdot_async_watch_balance(a, player, on_balance, game);

while (running) {
    pdot_async_poll(a);           /* callbacks run here; never blocks */
    update();
    render();
}
pdot_async_free(a);               /* runs every callback still owed, with PDOT_E_CANCELLED */
```

```c
static void on_sword(void *ud, int rc, const char *error, const void *result)
{
    const pdot_nft_item *item = result;   /* valid during the callback only */
    if (rc == 0)
        show_sword(ud, item->exists && memcmp(item->owner, player, 32) == 0);
}
```

- **One callback signature** for everything: `(ud, rc, error, result)`, with `result` pointing to the type documented for each call. That keeps bindings simple (one delegate type in C#).
- **Each job's callback runs exactly once**, cancelled or not, including from `pdot_async_free`. Watches end with one `PDOT_E_CANCELLED` call, so `ud` can always be released there.
- **Cancel** with `pdot_async_cancel(a, id)`. A queued job never runs; a running one stops at its next network call or wait. A store may still land on chain.
- **Watches** live on a second thread with their own connection. Each new final block triggers one batched read of every watched key (one proof with a verifier), and a callback runs only when a value changed. A failed read is reported once and retried. New blocks are announced over a WebSocket subscription (`chain_subscribeFinalizedHeads`, on the relay chain when verifying) if libcurl supports WebSockets, and found by polling every 2 s otherwise.
- **Wake-ups**: `pdot_async_set_wake` lets a loop that sleeps be woken when results are waiting.
- `pdot_async_run(a, work, done, ud)` runs any blocking libpdot code of your own on the worker.

[`examples/game_loop.c`](examples/game_loop.c) is a complete 60 fps loop. `pdot-bulletin watch head|balance|asset|nft` does the same from the command line.

### API overview

| Area | Functions |
| --- | --- |
| Client | `pdot_client_new`, `pdot_client_free`, `pdot_client_set_timeout`, `pdot_last_error`, `pdot_strerror`, `pdot_free` |
| Keys and signing | `pdot_signer_sr25519_from_seed`, `pdot_signer_ecdsa_from_seed`, `pdot_signer_sign`, `pdot_signer_wipe` |
| Addresses | `pdot_ss58_encode`, `pdot_ss58_decode` |
| Content IDs | `pdot_cid_compute`, `pdot_cid_compute_codec`, `pdot_cid_verify`, `pdot_cid_to_string`, `pdot_cid_from_string`, `pdot_cid_codec`, `pdot_cid_digest` |
| Authorizations | `pdot_bulletin_authorization`, `pdot_bulletin_is_authorized`, `pdot_bulletin_authorize_account` |
| Metadata hash | `pdot_client_set_metadata_hash`, `pdot_client_metadata_hash` |
| Verification | `pdot_checkpoint_fetch`, `pdot_checkpoint_fetch_at`, `pdot_client_set_verifier`, `pdot_client_checkpoint`, `pdot_bulletin_verify_cid` |
| Data | `pdot_bulletin_store`, `pdot_bulletin_store_chunked`, `pdot_bulletin_fetch`, `pdot_bulletin_renew`, `pdot_bulletin_renew_tree` |
| Chain state | `pdot_balance_get`, `pdot_asset_balance_get`, `pdot_asset_info_get`, `pdot_nft_item_get`, `pdot_nft_attribute_get`, `pdot_nft_owned`, `pdot_storage_get`, `pdot_client_token` |
| Amounts | `pdot_u128_format`, `pdot_u128_to_double` |
| Non-blocking | `pdot_async_new`, `pdot_async_free`, `pdot_async_poll`, `pdot_async_cancel`, `pdot_async_set_wake`, `pdot_async_run`, `pdot_client_cancelled`, and `pdot_async_` versions of the store, renew, fetch, verify and read calls |
| Watches | `pdot_async_watch_head`, `pdot_async_watch_storage`, `pdot_async_watch_balance`, `pdot_async_watch_asset_balance`, `pdot_async_watch_nft_item` |

### Things to know

- **Calls block.** A store waits for finalization, a few seconds to a minute. In a game or GUI, use `pdot_async` (above).
- **Threads:** use one `pdot_client` per thread; `pdot_async` keeps its own two. Signers can be shared. The random number generator is seeded once and is thread-safe.
- **Memory:** buffers the library returns are freed with `pdot_free`. Call `pdot_signer_wipe` when you're done with a key.
- **Trust:** without a verifier, the RPC node is trusted to report chain state, such as authorizations and inclusion. With one, those are proven (see above). Without the metadata hash, it is also trusted for the metadata transactions are built from. Retrieved data is never trusted; it's always checked against its CID.
- **Networks:** any Bulletin node that answers JSON-RPC over HTTP works. `ws://` and `wss://` URLs are accepted and converted to HTTP.

| Network | RPC endpoint |
| --- | --- |
| Local dev node | `http://127.0.0.1:9944` |
| Paseo Bulletin (TestNet) | `https://paseo-bulletin-next-rpc.polkadot.io` |
| Westend Bulletin (TestNet) | `https://westend-bulletin-rpc.polkadot.io` |

## Adding libpdot to your project

With CMake, add it as a subdirectory, for example as a git submodule:

```cmake
set(PDOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PDOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libpdot)
target_link_libraries(my_game PRIVATE pdot)
```

| Option | Default | What it does |
| --- | --- | --- |
| `PDOT_WITH_SR25519` | `ON` | sr25519 signer (sr25519-donna) |
| `PDOT_WITH_ECDSA` | `ON` | ECDSA secp256k1 signer (mbedTLS) |
| `PDOT_BUILD_TESTS` | `ON` | Unit tests |
| `PDOT_BUILD_EXAMPLES` | `ON` | Programs in `examples/` |
| `PDOT_SANITIZE` | `OFF` | Build with ASan and UBSan |
| `PDOT_WERROR` | `OFF` | Warnings in libpdot's own code are errors (CI uses this) |
| `PDOT_WITH_VERIFY` | `ON` | Relay-chain light client for verified results (needs `PDOT_WITH_SR25519`) |
| `PDOT_FUZZ` | `OFF` | libFuzzer targets (clang) |

## How it is tested

Every layer is checked against Parity's own implementations before it touches a network:

- **Signatures:** against [sp-core](https://crates.io/crates/sp-core), in both directions for sr25519 and byte for byte for ECDSA.
- **Transactions:** against [subxt](https://github.com/paritytech/subxt), byte for byte.
- **Event decoding:** against subxt, on events from Paseo blocks.
- **Metadata hash:** against hashes the Rust [merkleized-metadata](https://github.com/bkchr/merkleized-metadata) crate computed: the ones the Westend and Paseo Bulletin runtimes embed, and one of the crate's own test vectors. BLAKE3 against its official test vectors.
- **Verification:** GRANDPA justifications, validator-set handovers and storage proofs recorded from Paseo and Polkadot mainnet, plus tampered copies that must fail. [`tools/record-proofs.c`](tools/record-proofs.c) re-records them.
- **Asset Hub reads:** keys compared byte for byte with keys a Polkadot Asset Hub node listed, then balances, USDt, an NFT and its attributes decoded from one recorded read proof, checked against the block's state root. A proof with any byte changed must fail. [`tools/record-assets.c`](tools/record-assets.c) re-records it.
- **`pdot_async`:** ordering, callbacks only inside `pdot_async_poll`, cancelling queued and running jobs, callbacks owed at `pdot_async_free`, and one report per failure streak for watches. Also run under ThreadSanitizer.
- **Chunked-file manifests:** against the JavaScript libraries the Bulletin TypeScript SDK uses.

[`tools/fixtures-gen`](tools/fixtures-gen) regenerates those fixtures. It is a separate Rust program; the C build never needs Rust.

[CI](.github/workflows/ci.yml) runs these jobs on every push:
- gcc and clang, each with and without AddressSanitizer and UBSan;
- arm64, 32-bit ARM and big-endian s390x under QEMU;
- ThreadSanitizer on the unit tests, for `pdot_async`;
- libFuzzer on the metadata parser and metadata hash, on the event, manifest and proof parsers, and on balance, asset and NFT decoding;
- an end-to-end run against a dev node and an IPFS gateway, signing with the metadata hash, where a transaction signed over a wrong hash must be rejected, and a `pdot_async` balance watch must see a store change the account's nonce;
- nightly, read-only verification against live Paseo and Polkadot, including proven Asset Hub reads and a proven watch over WebSocket, and a check that the Westend, Paseo and Polkadot Bulletin runtimes accept libpdot's metadata hash ([`ci/verify-live.sh`](ci/verify-live.sh)).

Every job runs in a container, so you can reproduce one locally with Docker:

```sh
ci/in-docker.sh linux/amd64 ci/build-test.sh CC=clang SANITIZE=1
ci/in-docker.sh linux/arm/v7 ci/build-test.sh
ci/in-docker.sh linux/amd64 ci/e2e.sh
```

## Limitations

- Beyond the Bulletin Chain, only reads: balances, `pallet-assets` and `pallet-nfts`. No transfers yet, and no foreign (XCM-location) assets.
- `pdot_nft_owned` cannot prove that a node listed every item, only that each listed item is owned.
- The first call on a chain downloads its metadata, about 0.7 MB on Asset Hub, so the first result takes a few seconds. `pdot_async`'s two connections share that download, and a runtime upgrade triggers one new download.
- Watches are woken by WebSocket only where libcurl supports it (Debian 13 does; the macOS system libcurl does not); otherwise they poll every 2 s.
- Verification proves storage, renewals and authorizations, but not the outcome of arbitrary transactions.
- The metadata hash needs runtimes built with it (all Bulletin runtimes are) and v15 metadata. The token symbol and decimals in it are not in the metadata; libpdot takes them from the node unless you pass them.
- Verification needs a relay chain that has changed its GRANDPA validator set at least once: until then, nodes cannot serve `grandpa_proveFinality`. Every public network has; a fresh local relay chain has not. [`tools/e2e-relay.sh`](tools/e2e-relay.sh) schedules a change at block 1 for that reason.
- Chunks upload one at a time, so a 64 MiB file takes about 65 transactions.
- Polkadot mainnet is not supported; its Bulletin authorization model is not final.
- Not tested on Windows or macOS CI. The library builds and runs on macOS. `pdot_async` has Win32 threads, but they have never been compiled.
- sr25519 uses [sr25519-donna](https://github.com/TerenceGe/sr25519-donna) with fixes applied at build time ([`cmake/PatchSr25519.cmake`](cmake/PatchSr25519.cmake)). These include big-endian fixes, without which sr25519 signatures fail on those platforms.

## References

**Bulletin Chain**
- [Polkadot Bulletin Chain repository](https://github.com/paritytech/polkadot-bulletin-chain): pallets, runtimes, dev node setup, and the Rust and TypeScript SDKs
- [Store and retrieve data on the Bulletin Chain](https://docs.polkadot.com/chain-interactions/store-data/bulletin-chain/)
- [Store data on chain](https://docs.polkadot.com/apps/build/store-data-on-chain/)
- [Bulletin Chain Console](https://paritytech.github.io/polkadot-bulletin-chain/): browser UI, including storage authorizations

**Polkadot and Substrate internals**
- [Polkadot developer docs](https://docs.polkadot.com/)
- [SCALE codec](https://github.com/paritytech/parity-scale-codec)
- [Runtime metadata formats](https://github.com/paritytech/frame-metadata)
- [RFC-0078: Merkleized Metadata](https://polkadot-fellows.github.io/RFCs/approved/0078-merkleized-metadata.html) and its reference implementation, [merkleized-metadata](https://github.com/bkchr/merkleized-metadata)
- [subxt](https://github.com/paritytech/subxt), the Rust client libpdot is checked against
- [polkadot-sdk](https://github.com/paritytech/polkadot-sdk): transaction extensions, `polkadot-omni-node`
- [JSON-RPC interface spec](https://github.com/paritytech/json-rpc-interface-spec)
- [schnorrkel (sr25519)](https://github.com/w3f/schnorrkel)

**IPFS**
- [CID specification](https://github.com/multiformats/cid)
- [UnixFS](https://github.com/ipfs/specs/blob/main/UNIXFS.md) and [DAG-PB](https://ipld.io/specs/codecs/dag-pb/spec/)
- [Kubo](https://github.com/ipfs/kubo), an IPFS node with an HTTP gateway

## License

libpdot is licensed under the [Apache License 2.0](LICENSE). Bundled third-party code keeps its own compatible license: BLAKE2 (CC0), BLAKE3 (Apache-2.0), xxHash (BSD-2-Clause) and sr25519-donna (Apache-2.0).
