#!/bin/sh
# End-to-end test against a local Bulletin dev node (the PoC acceptance flow).
#
# Builds a dev chain spec from the runtime currently deployed on Westend
# Bulletin (no Rust build needed), starts polkadot-omni-node, then runs the
# store / fetch / verify / renew round trip with sr25519 //Alice and with
# ECDSA //Alice after //Eve (a registered authorizer in the dev genesis)
# grants it storage, and a chunked file with a DAG-PB manifest. With KUBO=1
# it also fetches through a local Kubo gateway.
#
#   OMNI_NODE=/path/to/polkadot-omni-node tools/e2e-devnode.sh [build-dir]
set -eu

BUILD=${1:-build}
CLI=$BUILD/pdot-bulletin
OMNI_NODE=${OMNI_NODE:-polkadot-omni-node}
RUNTIME_RPC=${RUNTIME_RPC:-https://westend-bulletin-rpc.polkadot.io}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pdot-e2e.XXXXXX")
RPC=http://127.0.0.1:9944

# Well-known dev seeds (`subkey inspect //Alice`, `--scheme ecdsa`, `//Eve`).
ALICE_SR=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
ALICE_ECDSA=cb6df9de1efca7a3998a8ead4e02159d5fa99c3e0d4fd6432667390bb4726854
ALICE_ECDSA_ADDR=5C7C2Z5sWbytvHpuLTvzKunnnRwQxft1jiqrLD5rhucQ5S9X
EVE_SR=786ad0e2df456fe43dd1f91ebca22e235bc162e0bb8d53c633e8c85b2af68b7a

NODE_PID=
KUBO_PID=
cleanup() {
    if [ -n "$NODE_PID" ]; then kill "$NODE_PID" 2>/dev/null || true; fi
    if [ -n "$KUBO_PID" ]; then kill "$KUBO_PID" 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

echo "== runtime from $RUNTIME_RPC"
curl -fsS -m 120 -H 'Content-Type: application/json' \
    -d '{"id":1,"jsonrpc":"2.0","method":"state_getStorage","params":["0x3a636f6465"]}' \
    "$RUNTIME_RPC" | sed 's/.*"result":"0x\([0-9a-f]*\)".*/\1/' | xxd -r -p > "$WORK/runtime.wasm"
(cd "$WORK" && "$OMNI_NODE" chain-spec-builder -c spec.json create -p 2487 -c westend \
    -i bulletin-westend -n Bulletin -t local -r runtime.wasm named-preset local_testnet >/dev/null)

echo "== starting dev node"
"$OMNI_NODE" --chain "$WORK/spec.json" --dev --ipfs-server --base-path "$WORK/node" \
    > "$WORK/node.log" 2>&1 &
NODE_PID=$!
i=0
until grep -q 'finalized #[1-9]' "$WORK/node.log"; do
    i=$((i + 1)); [ $i -gt 120 ] && { tail -20 "$WORK/node.log"; exit 1; }
    sleep 1
done

head -c 1048576 /dev/urandom > "$WORK/1mib.bin"
head -c 700000 /dev/urandom > "$WORK/700k.bin"

echo "== sr25519 //Alice, 1 MiB"
"$CLI" --rpc $RPC --seed $ALICE_SR roundtrip "$WORK/1mib.bin" | tee "$WORK/sr.out"
grep -q 'dispatch:   ok' "$WORK/sr.out"
grep -q 'stored as:  #' "$WORK/sr.out"
grep -q '^renewed ' "$WORK/sr.out"

echo "== sr25519 //Alice, 3.5 MiB chunked (auto: over 2 MiB)"
head -c 3670016 /dev/urandom > "$WORK/chunked.bin"
"$CLI" --rpc $RPC --seed $ALICE_SR roundtrip "$WORK/chunked.bin" 2>&1 | tee "$WORK/chunked.out"
grep -q 'as 4 chunks + manifest' "$WORK/chunked.out"
grep -q 'roundtrip ok' "$WORK/chunked.out"

echo "== a failing call is reported by name"
if "$CLI" --rpc $RPC --seed $EVE_SR authorize $ALICE_ECDSA_ADDR 1 0 > "$WORK/fail.out" 2>&1; then
    echo "expected authorize with 0 bytes to fail"; exit 1
fi
grep -q 'TransactionStorage.BadDataSize' "$WORK/fail.out"

echo "== //Eve authorizes ECDSA //Alice"
"$CLI" --rpc $RPC --seed $EVE_SR authorize $ALICE_ECDSA_ADDR 10 4194304

echo "== ecdsa //Alice, 700 KB"
"$CLI" --rpc $RPC --scheme ecdsa --seed $ALICE_ECDSA roundtrip "$WORK/700k.bin" | tee "$WORK/ecdsa.out"
grep -q 'dispatch:   ok' "$WORK/ecdsa.out"
grep -q '^renewed ' "$WORK/ecdsa.out"

if [ "${KUBO:-0}" = 1 ]; then
    echo "== fetch through a Kubo gateway"
    export IPFS_PATH="$WORK/kubo"
    ipfs init --profile=test -e >/dev/null
    ipfs config Addresses.Gateway /ip4/127.0.0.1/tcp/8283
    ipfs config Addresses.API /ip4/127.0.0.1/tcp/5011
    ipfs config --json Addresses.Swarm '["/ip4/127.0.0.1/tcp/4011"]'
    ipfs config --json Bootstrap '[]'
    ipfs daemon > "$WORK/kubo.log" 2>&1 &
    KUBO_PID=$!
    until grep -q 'Daemon is ready' "$WORK/kubo.log"; do sleep 1; done
    PEER=$(sed -n 's/.*Local node identity is: \([^ ]*\).*/\1/p' "$WORK/node.log")
    ipfs swarm connect "/ip4/127.0.0.1/tcp/30333/p2p/$PEER"
    CID=$(sed -n 's/^  cid: *//p' "$WORK/ecdsa.out")
    "$CLI" --gateway http://127.0.0.1:8283 fetch "$CID" "$WORK/700k.out"
    cmp "$WORK/700k.bin" "$WORK/700k.out"

    echo "== chunked file through the gateway: libpdot (raw blocks) and plain IPFS"
    ROOT=$(sed -n 's/^  cid: *//p' "$WORK/chunked.out")
    "$CLI" --gateway http://127.0.0.1:8283 fetch "$ROOT" "$WORK/chunked.gw"
    cmp "$WORK/chunked.bin" "$WORK/chunked.gw"
    curl -fsS -m 120 "http://127.0.0.1:8283/ipfs/$ROOT" -o "$WORK/chunked.ipfs"
    cmp "$WORK/chunked.bin" "$WORK/chunked.ipfs"
fi

echo "== e2e ok"
