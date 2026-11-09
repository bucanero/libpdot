#!/bin/sh
# Local dev chains for pdot-asteroids, built like tools/e2e-assethub.sh and
# tools/e2e-devnode.sh from the runtimes deployed on Westend (no Rust build):
#   Asset Hub  http://127.0.0.1:9944  //Bob, the operator's key, creates trophy collection 0;
#                                     //Alice creates story collection 1 for examples/visualnovel
#   Bulletin   http://127.0.0.1:9945  //Eve authorizes //Bob to store the leaderboard
# The nodes run until Ctrl-C.
#
#   OMNI_NODE=/path/to/polkadot-omni-node examples/asteroids/devnet.sh [build-dir]
#   build-dir/examples/asteroids/asteroids-operator   # in another terminal
#   build-dir/examples/asteroids/asteroids            # and another
set -eu

BUILD=${1:-build-game}
CLI=$BUILD/pdot-bulletin
OMNI_NODE=${OMNI_NODE:-polkadot-omni-node}
AH_RUNTIME_RPC=${AH_RUNTIME_RPC:-https://westend-asset-hub-rpc.polkadot.io}
BULLETIN_RUNTIME_RPC=${BULLETIN_RUNTIME_RPC:-https://westend-bulletin-rpc.polkadot.io}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pdot-asteroids.XXXXXX")
AH=http://127.0.0.1:9944
BULLETIN=http://127.0.0.1:9945

# Well-known dev seeds (`subkey inspect //Bob`, `//Eve`).
BOB=398f0c28f98885e046333d4a41c19cee4c37368a9832c6502f6cfd182e2aef89
BOB_ADDR=5FHneW46xGXgs5mUiveU4sbTyGBzmstUspZC92UhjJM694ty
EVE=786ad0e2df456fe43dd1f91ebca22e235bc162e0bb8d53c633e8c85b2af68b7a
# //Alice: owns examples/visualnovel's story collection.
ALICE=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
ALICE_ADDR=5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY

AH_PID=
BULLETIN_PID=
# Stops the processes and waits for each to exit (SIGKILL after 15 s), so a
# node has closed its database before the work directory goes: removing it
# while the node still writes fails with "Directory not empty".
stop() {
    for p in "$@"; do kill "$p" 2>/dev/null || true; done
    for p in "$@"; do
        (sleep 15; kill -9 "$p" 2>/dev/null) &
        w=$!
        wait "$p" 2>/dev/null || true
        kill "$w" 2>/dev/null || true
    done
}
cleanup() {
    stop $AH_PID $BULLETIN_PID
    rm -rf "$WORK" || true
}
trap cleanup EXIT INT TERM
fail() { echo "FAIL: $*"; exit 1; }

[ -x "$CLI" ] || fail "no $CLI: build with -DPDOT_BUILD_ASTEROIDS=ON into $BUILD"

# spec NAME RUNTIME_RPC PARA_ID RUNTIME_ID PRESET
spec() {
    curl -fsS -m 300 -H 'Content-Type: application/json' \
        -d '{"id":1,"jsonrpc":"2.0","method":"state_getStorage","params":["0x3a636f6465"]}' \
        "$2" | sed 's/.*"result":"0x\([0-9a-f]*\)".*/\1/' | xxd -r -p > "$WORK/$1.wasm"
    "$OMNI_NODE" chain-spec-builder -c "$WORK/$1.json" create -p "$3" -c westend -i "$4" \
        -n "$1" -t local -r "$WORK/$1.wasm" named-preset "$5" >/dev/null
    # CheckMetadataHash covers the token the runtimes were built with (WND, 12).
    sed 's/"tokenSymbol": *"UNIT"/"tokenSymbol": "WND"/' "$WORK/$1.json" > "$WORK/$1.tmp"
    mv "$WORK/$1.tmp" "$WORK/$1.json"
}

# wait_final NAME PID: until the node finalizes a block
wait_final() {
    i=0
    until grep -q 'finalized #[1-9]' "$WORK/$1.log"; do
        i=$((i + 1))
        kill -0 "$2" 2>/dev/null || { tail -40 "$WORK/$1.log"; fail "the $1 node exited"; }
        [ $i -le 900 ] || { tail -40 "$WORK/$1.log"; fail "$1: no finalized block after 900s"; }
        sleep 1
    done
    echo "$1: first block finalized after ${i}s"
}

echo "== runtimes from $AH_RUNTIME_RPC and $BULLETIN_RUNTIME_RPC"
spec AssetHub "$AH_RUNTIME_RPC" 1000 asset-hub-westend development
spec Bulletin "$BULLETIN_RUNTIME_RPC" 2487 bulletin-westend local_testnet

echo "== starting the dev nodes (logs in $WORK)"
"$OMNI_NODE" --chain "$WORK/AssetHub.json" --dev --base-path "$WORK/ah" \
    > "$WORK/AssetHub.log" 2>&1 &
AH_PID=$!
# Bulletin serves stored data over bitswap_v1_get with --ipfs-server.
"$OMNI_NODE" --chain "$WORK/Bulletin.json" --dev --ipfs-server --base-path "$WORK/bulletin" \
    --rpc-port 9945 --port 30334 --no-prometheus > "$WORK/Bulletin.log" 2>&1 &
BULLETIN_PID=$!
wait_final Bulletin $BULLETIN_PID
wait_final AssetHub $AH_PID

echo "== //Bob creates trophy collection 0 on Asset Hub"
"$CLI" --rpc $AH --seed $BOB call Nfts create "[\"$BOB_ADDR\", {\"settings\": 0,
    \"max_supply\": null, \"mint_settings\": {\"mint_type\": \"Issuer\", \"price\": null,
    \"start_block\": null, \"end_block\": null, \"default_item_settings\": 0}}]" \
    > "$WORK/tx.out"
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create"; }

echo "== //Alice creates story collection 1 for examples/visualnovel (public minting)"
"$CLI" --rpc $AH --seed $ALICE call Nfts create "[\"$ALICE_ADDR\", {\"settings\": 0,
    \"max_supply\": null, \"mint_settings\": {\"mint_type\": \"Public\", \"price\": null,
    \"start_block\": null, \"end_block\": null, \"default_item_settings\": 0}}]" \
    > "$WORK/tx.out"
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create (story)"; }

echo "== //Eve authorizes //Bob on Bulletin: 1000 stores, 16 MiB"
"$CLI" --rpc $BULLETIN --seed $EVE authorize $BOB_ADDR 1000 16777216 > "$WORK/tx.out"
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "authorize"; }

# A fresh chain reuses block numbers: claims recorded on an earlier one
# (block << 32 | index) would refuse new scores as already claimed.
if [ -f asteroids-claims.txt ]; then
    rm -f asteroids-claims.txt
    echo "== fresh chain: forgot the earlier chain's claims (asteroids-claims.txt)"
fi
echo "== ready: run $BUILD/examples/asteroids/asteroids-operator, then"
echo "   $BUILD/examples/asteroids/asteroids, each in its own terminal (Ctrl-C here stops the nodes)"
echo "   The visual novel: cd examples/visualnovel/build/game && ./Suika3.app/Contents/MacOS/Suika3"
wait "$AH_PID"
