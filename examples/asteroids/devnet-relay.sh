#!/bin/sh
# pdot-asteroids under a local relay chain, for proven mode: a westend-local
# relay (validators //Alice and //Bob) with Asset Hub as parachain 1000 and
# the Bulletin chain as parachain 2487, from the runtimes live on Westend.
# Then the same setup as devnet.sh (//Bob's trophy collection 0, //Alice's
# story collection 1 for examples/visualnovel, //Eve authorizing //Bob on
# Bulletin) and a checkpoint for the verifiers.
#
#   VALIDATOR_EXTRA_ARGS=--insecure-validator-i-know-what-i-do \
#   POLKADOT=/path/to/polkadot OMNI_NODE=/path/to/polkadot-omni-node \
#     examples/asteroids/devnet-relay.sh [build-dir]
#
#   relay      http://127.0.0.1:9900
#   Asset Hub  http://127.0.0.1:9944   (the game's default --rpc)
#   Bulletin   http://127.0.0.1:9945   (the game's default --bulletin)
#
# It prints the operator and game commands, with --relay and a checkpoint
# each, and runs until Ctrl-C. Validators need Linux's secure-validator
# sandbox; where it is unavailable (macOS, most containers) pass the opt-out
# yourself through VALIDATOR_EXTRA_ARGS, for these throwaway validators only.
set -eu

BUILD=${1:-build-game}
CLI=$BUILD/pdot-bulletin
POLKADOT=${POLKADOT:-polkadot}
OMNI_NODE=${OMNI_NODE:-polkadot-omni-node}
AH_RUNTIME_RPC=${AH_RUNTIME_RPC:-https://westend-asset-hub-rpc.polkadot.io}
BULLETIN_RUNTIME_RPC=${BULLETIN_RUNTIME_RPC:-https://westend-bulletin-rpc.polkadot.io}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pdot-asteroids-relay.XXXXXX")
RELAY=http://127.0.0.1:9900
AH=http://127.0.0.1:9944
BULLETIN=http://127.0.0.1:9945
AH_ID=1000
BULLETIN_ID=2487
CK=$BUILD/asteroids-relay.ck

BOB=398f0c28f98885e046333d4a41c19cee4c37368a9832c6502f6cfd182e2aef89
BOB_ADDR=5FHneW46xGXgs5mUiveU4sbTyGBzmstUspZC92UhjJM694ty
EVE=786ad0e2df456fe43dd1f91ebca22e235bc162e0bb8d53c633e8c85b2af68b7a
# //Alice: owns examples/visualnovel's story collection.
ALICE=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
ALICE_ADDR=5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY

PIDS=
KEEP_LOGS=
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    if [ -n "$KEEP_LOGS" ]; then echo "logs kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT INT TERM
fail() { echo "FAIL: $*"; show_logs; exit 1; }

show_logs() {
    for f in "$WORK"/*.log; do
        [ -f "$f" ] || continue
        echo "--- $f: first errors"
        grep -m 8 -iE 'error|cannot|secure|panic' "$f" || echo "(none)"
        echo "--- $f: last lines"
        tail -8 "$f"
    done
    KEEP_LOGS=1
}

[ -x "$CLI" ] || { echo "no $CLI: build with -DPDOT_BUILD_ASTEROIDS=ON into $BUILD"; exit 1; }

# polkadot 1.25 and later have export-chain-spec, older releases build-spec.
relay_spec() {  # out-file args...
    out=$1
    shift
    "$POLKADOT" export-chain-spec "$@" > "$out" 2> "$WORK/spec.err" && return 0
    "$POLKADOT" build-spec --disable-default-bootnode "$@" > "$out" 2>> "$WORK/spec.err" && return 0
    echo "cannot create the relay chain spec with $POLKADOT:"
    cat "$WORK/spec.err"
    exit 1
}

wait_for() {  # file pattern seconds
    i=0
    until grep -q "$2" "$1" 2>/dev/null; do
        i=$((i + 1))
        [ $i -le "$3" ] || fail "timed out waiting for '$2' in $1"
        for p in $PIDS; do
            kill -0 "$p" 2>/dev/null || fail "a node exited while waiting for '$2'"
        done
        sleep 1
    done
}

# para NAME RUNTIME_RPC PARA_ID RUNTIME_ID PRESET: spec, genesis head and code
para() {
    curl -fsS -m 300 -H 'Content-Type: application/json' \
        -d '{"id":1,"jsonrpc":"2.0","method":"state_getStorage","params":["0x3a636f6465"]}' \
        "$2" | sed 's/.*"result":"0x\([0-9a-f]*\)".*/\1/' | xxd -r -p > "$WORK/$1.wasm"
    "$OMNI_NODE" chain-spec-builder -c "$WORK/$1.json" create -p "$3" -c westend_local_testnet \
        -i "$4" -n "$1" -t local -r "$WORK/$1.wasm" named-preset "$5" >/dev/null
    # The relay schedules the id given here; CheckMetadataHash covers the
    # token the runtime was built with (WND, 12).
    python3 - "$WORK/$1.json" "$3" <<'EOF'
import json, sys
path, para = sys.argv[1], int(sys.argv[2])
spec = json.load(open(path))
spec["genesis"]["runtimeGenesis"]["patch"]["parachainInfo"] = {"parachainId": para}
spec["properties"] = {"tokenSymbol": "WND", "tokenDecimals": 12}
json.dump(spec, open(path, "w"))
EOF
    "$OMNI_NODE" export-genesis-head --chain "$WORK/$1.json" > "$WORK/$1.head" 2>/dev/null
    "$OMNI_NODE" export-genesis-wasm --chain "$WORK/$1.json" > "$WORK/$1.code" 2>/dev/null
}

echo "== runtimes from $AH_RUNTIME_RPC and $BULLETIN_RUNTIME_RPC"
para AssetHub "$AH_RUNTIME_RPC" $AH_ID asset-hub-westend local_testnet
para Bulletin "$BULLETIN_RUNTIME_RPC" $BULLETIN_ID bulletin-westend local_testnet

echo "== westend-local relay with parachains $AH_ID and $BULLETIN_ID at genesis"
relay_spec "$WORK/relay-plain.json" --chain westend-local
python3 - "$WORK" $AH_ID $BULLETIN_ID <<'EOF'
import json, sys
work, ah, bul = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
spec = json.load(open(f"{work}/relay-plain.json"))
def entry(name):
    return {"genesis_head": open(f"{work}/{name}.head").read().strip(),
            "validation_code": open(f"{work}/{name}.code").read().strip(),
            "parachain": True}
spec["genesis"]["runtimeGenesis"]["patch"]["paras"] = {
    "paras": [[ah, entry("AssetHub")], [bul, entry("Bulletin")]]
}
json.dump(spec, open(f"{work}/relay-plain.json", "w"))
EOF
relay_spec "$WORK/relay.json" --chain "$WORK/relay-plain.json" --raw
# A node serves grandpa_proveFinality only after its first authority set
# change, and westend-local makes none for hours. Schedule one at block 1, to
# the same authorities: set 0 hands over to set 1 at once.
python3 - "$WORK/relay.json" <<'EOF'
import json, sys
path = sys.argv[1]
spec = json.load(open(path))
top = spec["genesis"]["raw"]["top"]
grandpa = "0x5f9cc45b7a00c5899361e1c6099678dc"           # twox128("Grandpa")
authorities = top[grandpa + "5e0621c4869aa60c02be9adcc98a0d1d"][2:]
top[grandpa + "2ff65991b1c915dd6cc8d4825eacfcb4"] = "0x01000000" + "00000000" + authorities + "00"
top[grandpa + "8a2d09463effcc78a22d75b9cb87dffc"] = "0x0100000000000000"  # CurrentSetId
json.dump(spec, open(path, "w"))
EOF

echo "== starting 2 validators and 2 collators (logs in $WORK)"
VALIDATOR="--validator --tmp --no-telemetry --no-mdns --no-prometheus ${VALIDATOR_EXTRA_ARGS:-}"
# shellcheck disable=SC2086
"$POLKADOT" --chain "$WORK/relay.json" --alice $VALIDATOR --port 30333 --rpc-port 9900 \
    --node-key 0000000000000000000000000000000000000000000000000000000000000001 \
    > "$WORK/alice.log" 2>&1 &
PIDS="$PIDS $!"
ALICE_P2P=/ip4/127.0.0.1/tcp/30333/p2p/12D3KooWEyoppNCUx8Yx66oV9fJnriXwCcXwDDUA2kj6vnc6iDEp
# shellcheck disable=SC2086
"$POLKADOT" --chain "$WORK/relay.json" --bob $VALIDATOR --port 30334 --rpc-port 9901 \
    --node-key 0000000000000000000000000000000000000000000000000000000000000002 \
    --bootnodes $ALICE_P2P > "$WORK/bob.log" 2>&1 &
PIDS="$PIDS $!"

# Runtimes that build on a relay parent one block back need the slot-based
# collator. Each collator's embedded relay node gets its own RPC port (after
# --), or it takes the default and pushes the parachain's off its port.
"$OMNI_NODE" --chain "$WORK/AssetHub.json" --collator --alice --tmp --port 40333 --rpc-port 9944 \
    --authoring slot-based --no-telemetry --no-mdns --no-prometheus \
    --node-key 0000000000000000000000000000000000000000000000000000000000000003 \
    -- --chain "$WORK/relay.json" --port 30335 --rpc-port 9902 --bootnodes $ALICE_P2P \
    --no-prometheus \
    > "$WORK/assethub.log" 2>&1 &
PIDS="$PIDS $!"
# Bulletin serves stored data over bitswap_v1_get with --ipfs-server.
"$OMNI_NODE" --chain "$WORK/Bulletin.json" --collator --alice --tmp --port 40334 --rpc-port 9945 \
    --authoring slot-based --ipfs-server --no-telemetry --no-mdns --no-prometheus \
    --node-key 0000000000000000000000000000000000000000000000000000000000000004 \
    -- --chain "$WORK/relay.json" --port 30336 --rpc-port 9903 --bootnodes $ALICE_P2P \
    --no-prometheus \
    > "$WORK/bulletin.log" 2>&1 &
PIDS="$PIDS $!"

wait_for "$WORK/alice.log" 'finalized #[1-9]' 180
wait_for "$WORK/assethub.log" 'Parachain.*finalized #[1-9]' 900
wait_for "$WORK/bulletin.log" 'Parachain.*finalized #[1-9]' 900
# Each port must answer with its own chain.
for pair in "$AH:AssetHub" "$BULLETIN:Bulletin"; do
    url=${pair%:*}
    name=${pair##*:}
    curl -fsS -m 10 -H 'Content-Type: application/json' \
        -d '{"id":1,"jsonrpc":"2.0","method":"system_chain","params":[]}' "$url" \
        | grep -q "\"$name\"" || fail "$url is not the $name node"
done
echo "== relay and both parachains are finalizing"

echo "== checkpoint for the verifiers: $CK"
"$CLI" --rpc $AH --relay $RELAY --para-id $AH_ID --checkpoint "$CK" checkpoint

V="--relay $RELAY --checkpoint $CK"
echo "== //Bob creates trophy collection 0 on Asset Hub (proven)"
# shellcheck disable=SC2086
"$CLI" --rpc $AH $V --para-id $AH_ID --seed $BOB call Nfts create "[\"$BOB_ADDR\", {\"settings\": 0,
    \"max_supply\": null, \"mint_settings\": {\"mint_type\": \"Issuer\", \"price\": null,
    \"start_block\": null, \"end_block\": null, \"default_item_settings\": 0}}]" \
    > "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create"; }
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create"; }
grep 'verified' "$WORK/tx.out" || true

echo "== //Alice creates story collection 1 for examples/visualnovel (public minting, proven)"
# shellcheck disable=SC2086
"$CLI" --rpc $AH $V --para-id $AH_ID --seed $ALICE call Nfts create "[\"$ALICE_ADDR\", {\"settings\": 0,
    \"max_supply\": null, \"mint_settings\": {\"mint_type\": \"Public\", \"price\": null,
    \"start_block\": null, \"end_block\": null, \"default_item_settings\": 0}}]" \
    > "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create (story)"; }
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "Nfts.create (story)"; }

echo "== //Eve authorizes //Bob on Bulletin: 1000 stores, 16 MiB (proven)"
# shellcheck disable=SC2086
"$CLI" --rpc $BULLETIN $V --para-id $BULLETIN_ID --seed $EVE authorize $BOB_ADDR 1000 16777216 \
    > "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "authorize"; }
grep -q 'dispatch:   ok' "$WORK/tx.out" || { cat "$WORK/tx.out"; fail "authorize"; }
grep 'verified' "$WORK/tx.out" || true

# A fresh chain reuses block numbers: claims recorded on an earlier one
# (block << 32 | index) would refuse new scores as already claimed.
if [ -f asteroids-claims.txt ]; then
    rm -f asteroids-claims.txt
    echo "== fresh chain: forgot the earlier chain's claims (asteroids-claims.txt)"
fi
cp "$CK" "$BUILD/asteroids-operator.ck"
cp "$CK" "$BUILD/asteroids-game.ck"
cp "$CK" "$BUILD/visualnovel.ck"
echo "== ready. In two more terminals:"
echo "   $BUILD/examples/asteroids/asteroids-operator --relay $RELAY --checkpoint $BUILD/asteroids-operator.ck"
echo "   $BUILD/examples/asteroids/asteroids --relay $RELAY --checkpoint $BUILD/asteroids-game.ck"
echo "   The visual novel, proven: cd examples/visualnovel/build/game &&"
echo "     PDOT_RELAY=$RELAY PDOT_CHECKPOINT=$(cd "$BUILD" && pwd)/visualnovel.ck ./Suika3.app/Contents/MacOS/Suika3"
echo "   (Ctrl-C here stops the chains)"
wait
