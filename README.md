# libpdot

[![CI](https://github.com/bucanero/libpdot/actions/workflows/ci.yml/badge.svg)](https://github.com/bucanero/libpdot/actions/workflows/ci.yml)

**Store and retrieve data on Polkadot from plain C.** libpdot is a portable C99 library, plus a small command-line tool, for the [Polkadot Bulletin Chain](https://github.com/paritytech/polkadot-bulletin-chain): a chain that stores files, content-addressed by CID and served over IPFS.

It is for C programs that want Polkadot without a Rust toolchain or a JavaScript runtime: games, desktop apps, embedded and IoT devices, emulators, command-line tools, and bindings for other languages through C's ABI.

```c
pdot_client *c = pdot_client_new("https://paseo-bulletin-next-rpc.polkadot.io", NULL);
pdot_bulletin_store(c, &signer, data, len, &res);   /* waits for finalization  */
pdot_bulletin_fetch(c, &res.cid, &bytes, &n);       /* verified against the CID */
```

> **Status: proof of concept.** It works end to end on a local dev node and on Paseo Bulletin (TestNet), and is tested on x86_64, arm64, 32-bit ARM and big-endian s390x. The API may still change, it has not been audited, and Polkadot mainnet is not supported yet.

## What it does

- **Store** data up to 2 MiB in one transaction, and files up to 64 MiB as chunks plus a [UnixFS](https://github.com/ipfs/specs/blob/main/UNIXFS.md) manifest, laid out byte for byte as the official Bulletin TypeScript SDK does.
- **Retrieve** by CID straight from a node, or through any IPFS HTTP gateway. Every byte is checked against the CID's hash, so the node and the gateway don't have to be trusted.
- **Renew** stored data before its retention period (about 14 days on testnets) runs out.
- **Check and grant storage authorizations**: allowances, usage and expiry.
- **Sign** with sr25519 (what Polkadot wallets use) or ECDSA secp256k1, or plug in your own signer: a hardware wallet, a secure enclave or a remote service.
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

### API overview

| Area | Functions |
| --- | --- |
| Client | `pdot_client_new`, `pdot_client_free`, `pdot_client_set_timeout`, `pdot_last_error`, `pdot_strerror`, `pdot_free` |
| Keys and signing | `pdot_signer_sr25519_from_seed`, `pdot_signer_ecdsa_from_seed`, `pdot_signer_sign`, `pdot_signer_wipe` |
| Addresses | `pdot_ss58_encode`, `pdot_ss58_decode` |
| Content IDs | `pdot_cid_compute`, `pdot_cid_compute_codec`, `pdot_cid_verify`, `pdot_cid_to_string`, `pdot_cid_from_string`, `pdot_cid_codec`, `pdot_cid_digest` |
| Authorizations | `pdot_bulletin_authorization`, `pdot_bulletin_is_authorized`, `pdot_bulletin_authorize_account` |
| Data | `pdot_bulletin_store`, `pdot_bulletin_store_chunked`, `pdot_bulletin_fetch`, `pdot_bulletin_renew`, `pdot_bulletin_renew_tree` |

### Things to know

- **Calls block.** A store waits for finalization, a few seconds to a minute. In a game or GUI, call libpdot from a worker thread.
- **Threads:** use one `pdot_client` per thread. Signers can be shared. The random number generator is seeded once and is thread-safe.
- **Memory:** buffers the library returns are freed with `pdot_free`. Call `pdot_signer_wipe` when you're done with a key.
- **Trust:** the RPC node is trusted to report chain state, such as authorizations and inclusion. Retrieved data is never trusted; it's always checked against its CID.
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
| `PDOT_FUZZ` | `OFF` | libFuzzer targets (clang) |

## How it is tested

Every layer is checked against Parity's own implementations before it touches a network:

- **Signatures:** against [sp-core](https://crates.io/crates/sp-core), in both directions for sr25519 and byte for byte for ECDSA.
- **Transactions:** against [subxt](https://github.com/paritytech/subxt), byte for byte.
- **Event decoding:** against subxt, on events from Paseo blocks.
- **Chunked-file manifests:** against the JavaScript libraries the Bulletin TypeScript SDK uses.

[`tools/fixtures-gen`](tools/fixtures-gen) regenerates those fixtures. It is a separate Rust program; the C build never needs Rust.

[CI](.github/workflows/ci.yml) runs these jobs on every push:
- gcc and clang, each with and without AddressSanitizer and UBSan;
- arm64, 32-bit ARM and big-endian s390x under QEMU;
- libFuzzer on the metadata, event and manifest parsers;
- an end-to-end run against a dev node and an IPFS gateway.

Every job runs in a container, so you can reproduce one locally with Docker:

```sh
ci/in-docker.sh linux/amd64 ci/build-test.sh CC=clang SANITIZE=1
ci/in-docker.sh linux/arm/v7 ci/build-test.sh
ci/in-docker.sh linux/amd64 ci/e2e.sh
```

## Limitations

- Bulletin Chain only. There is no general API for other pallets or for balance transfers yet.
- Reading chain state relies on the RPC node. There is no light client or storage-proof verification.
- Chunks upload one at a time, so a 64 MiB file takes about 65 transactions.
- Polkadot mainnet is not supported; its Bulletin authorization model is not final.
- Not tested on Windows or macOS CI. The library builds and runs on macOS.
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
- [subxt](https://github.com/paritytech/subxt), the Rust client libpdot is checked against
- [polkadot-sdk](https://github.com/paritytech/polkadot-sdk): transaction extensions, `polkadot-omni-node`
- [JSON-RPC interface spec](https://github.com/paritytech/json-rpc-interface-spec)
- [schnorrkel (sr25519)](https://github.com/w3f/schnorrkel)

**IPFS**
- [CID specification](https://github.com/multiformats/cid)
- [UnixFS](https://github.com/ipfs/specs/blob/main/UNIXFS.md) and [DAG-PB](https://ipld.io/specs/codecs/dag-pb/spec/)
- [Kubo](https://github.com/ipfs/kubo), an IPFS node with an HTTP gateway

## License

libpdot is licensed under the [Apache License 2.0](LICENSE). Bundled third-party code keeps its own compatible license: BLAKE2 (CC0), xxHash (BSD-2-Clause) and sr25519-donna (Apache-2.0).
