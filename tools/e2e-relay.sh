#!/bin/sh
# End-to-end test of verification on a local relay chain: a westend-local
# relay (validators //Alice and //Bob) with the Bulletin chain as parachain
# 2487, then store, renew and authorize with results proven from GRANDPA
# finality instead of taken from the RPC node. Transactions are signed with
# CheckMetadataHash, and one over a wrong hash must be rejected. A proven
# pdot_async balance watch must see a store change the nonce.
#
#   POLKADOT=/path/to/polkadot OMNI_NODE=/path/to/polkadot-omni-node \
#     tools/e2e-relay.sh [build-dir]
#
# The polkadot-prepare-worker and polkadot-execute-worker binaries must sit
# next to polkadot. Validators need Linux's secure-validator sandbox; where it
# is unavailable (macOS, most containers) pass the opt-out yourself through
# VALIDATOR_EXTRA_ARGS, for these throwaway local validators only.
set -eu

BUILD=${1:-build}
CLI=$BUILD/pdot-bulletin
POLKADOT=${POLKADOT:-polkadot}
OMNI_NODE=${OMNI_NODE:-polkadot-omni-node}
RUNTIME_RPC=${RUNTIME_RPC:-https://westend-bulletin-rpc.polkadot.io}
PARA_ID=2487
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pdot-relay.XXXXXX")
RELAY=http://127.0.0.1:9944
PARA=http://127.0.0.1:9988

ALICE_SR=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
ALICE_ECDSA_ADDR=5C7C2Z5sWbytvHpuLTvzKunnnRwQxft1jiqrLD5rhucQ5S9X
EVE_SR=786ad0e2df456fe43dd1f91ebca22e235bc162e0bb8d53c633e8c85b2af68b7a

PIDS=
WATCH_PID=
cleanup() {
    for p in $PIDS $WATCH_PID; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

wait_for() {  # file pattern seconds
    i=0
    until grep -q "$2" "$1" 2>/dev/null; do
        i=$((i + 1))
        if [ $i -gt "$3" ]; then echo "timed out waiting for '$2' in $1"; tail -20 "$1"; exit 1; fi
        for p in $PIDS; do  # a node that exited will never log it
            if ! kill -0 "$p" 2>/dev/null; then
                echo "a node exited while waiting for '$2'"
                for f in "$WORK"/*.log; do echo "--- $f"; tail -20 "$f"; done
                exit 1
            fi
        done
        sleep 1
    done
}

echo "== Bulletin runtime from $RUNTIME_RPC"
curl -fsS -m 120 -H 'Content-Type: application/json' \
    -d '{"id":1,"jsonrpc":"2.0","method":"state_getStorage","params":["0x3a636f6465"]}' \
    "$RUNTIME_RPC" | sed 's/.*"result":"0x\([0-9a-f]*\)".*/\1/' | xxd -r -p > "$WORK/runtime.wasm"
"$OMNI_NODE" chain-spec-builder -c "$WORK/para.json" create -p $PARA_ID -c westend_local_testnet \
    -i bulletin-westend -n Bulletin -t local -r "$WORK/runtime.wasm" named-preset local_testnet >/dev/null
# The preset's own parachain id (1010) must match the id the relay schedules,
# and the token properties the runtime build (see CheckMetadataHash).
python3 - "$WORK/para.json" $PARA_ID <<'EOF'
import json, sys
path, para = sys.argv[1], int(sys.argv[2])
spec = json.load(open(path))
spec["genesis"]["runtimeGenesis"]["patch"]["parachainInfo"] = {"parachainId": para}
# The token the runtime was built with, which CheckMetadataHash covers.
spec["properties"] = {"tokenSymbol": "WND", "tokenDecimals": 12}
json.dump(spec, open(path, "w"))
EOF
"$OMNI_NODE" export-genesis-head --chain "$WORK/para.json" > "$WORK/head.hex" 2>/dev/null
"$OMNI_NODE" export-genesis-wasm --chain "$WORK/para.json" > "$WORK/code.hex" 2>/dev/null

echo "== westend-local relay with parachain $PARA_ID at genesis"
"$POLKADOT" build-spec --chain westend-local --disable-default-bootnode > "$WORK/relay-plain.json" 2>/dev/null
python3 - "$WORK" $PARA_ID <<'EOF'
import json, sys
work, para = sys.argv[1], int(sys.argv[2])
spec = json.load(open(f"{work}/relay-plain.json"))
head = open(f"{work}/head.hex").read().strip()
code = open(f"{work}/code.hex").read().strip()
spec["genesis"]["runtimeGenesis"]["patch"]["paras"] = {
    "paras": [[para, {"genesis_head": head, "validation_code": code, "parachain": True}]]
}
json.dump(spec, open(f"{work}/relay-plain.json", "w"))
EOF
"$POLKADOT" build-spec --chain "$WORK/relay-plain.json" --raw --disable-default-bootnode > "$WORK/relay.json" 2>/dev/null
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
# PendingChange { scheduled_at: 1, delay: 0, next_authorities, forced: None }
top[grandpa + "2ff65991b1c915dd6cc8d4825eacfcb4"] = "0x01000000" + "00000000" + authorities + "00"
top[grandpa + "8a2d09463effcc78a22d75b9cb87dffc"] = "0x0100000000000000"  # CurrentSetId
json.dump(spec, open(path, "w"))
EOF

VALIDATOR="--validator --tmp --no-telemetry --no-mdns ${VALIDATOR_EXTRA_ARGS:-}"
# shellcheck disable=SC2086
"$POLKADOT" --chain "$WORK/relay.json" --alice $VALIDATOR --port 30333 --rpc-port 9944 \
    --node-key 0000000000000000000000000000000000000000000000000000000000000001 \
    > "$WORK/alice.log" 2>&1 &
PIDS="$PIDS $!"
ALICE_P2P=/ip4/127.0.0.1/tcp/30333/p2p/12D3KooWEyoppNCUx8Yx66oV9fJnriXwCcXwDDUA2kj6vnc6iDEp
# shellcheck disable=SC2086
"$POLKADOT" --chain "$WORK/relay.json" --bob $VALIDATOR --port 30334 --rpc-port 9945 \
    --node-key 0000000000000000000000000000000000000000000000000000000000000002 \
    --bootnodes $ALICE_P2P > "$WORK/bob.log" 2>&1 &
PIDS="$PIDS $!"

"$OMNI_NODE" --chain "$WORK/para.json" --collator --alice --tmp --port 40333 --rpc-port 9988 \
    --node-key 0000000000000000000000000000000000000000000000000000000000000003 \
    --no-telemetry --no-mdns -- --chain "$WORK/relay.json" --port 30335 --bootnodes $ALICE_P2P \
    > "$WORK/collator.log" 2>&1 &
PIDS="$PIDS $!"

wait_for "$WORK/alice.log" 'finalized #[1-9]' 180
wait_for "$WORK/collator.log" 'Parachain.*finalized #[1-9]' 600
echo "== relay and parachain are finalizing"

CK="$WORK/relay.ck"
V="--rpc $PARA --relay $RELAY --para-id $PARA_ID --checkpoint $CK"
# shellcheck disable=SC2086
"$CLI" $V checkpoint

echo "== proven authorization"
# shellcheck disable=SC2086
"$CLI" $V authorized 5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY | tee "$WORK/auth.out"
grep -q 'verified:     yes' "$WORK/auth.out"

echo "== store, renew: each result proven"
head -c 300000 /dev/urandom > "$WORK/data.bin"
# shellcheck disable=SC2086
"$CLI" $V --seed $ALICE_SR roundtrip "$WORK/data.bin" | tee "$WORK/rt.out"
grep -q 'verified:   yes' "$WORK/rt.out"
CID=$(sed -n 's/^  cid: *//p' "$WORK/rt.out")
# shellcheck disable=SC2086
"$CLI" $V verify "$CID"

echo "== chunked store: every chunk and the manifest proven"
head -c 2500000 /dev/urandom > "$WORK/big.bin"
# shellcheck disable=SC2086
"$CLI" $V --seed $ALICE_SR store "$WORK/big.bin" | tee "$WORK/big.out"
grep -q 'every chunk and the manifest proven' "$WORK/big.out"

echo "== CheckMetadataHash: a hash over the wrong inputs is rejected by the chain"
# shellcheck disable=SC2086
"$CLI" $V metadata-hash
head -c 1000 /dev/urandom > "$WORK/small.bin"
# shellcheck disable=SC2086
if "$CLI" $V --seed $ALICE_SR --metadata-hash UNIT:12 store "$WORK/small.bin" \
        > "$WORK/mdh.out" 2>&1; then
    echo "expected a transaction signed over another metadata hash to be rejected"; exit 1
fi
cat "$WORK/mdh.out"
grep -q 'signed with CheckMetadataHash' "$WORK/mdh.out"

echo "== pdot_async: a balance watch reports //Alice's nonce changing"
# shellcheck disable=SC2086
"$CLI" $V --count 2 watch balance 5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY \
    > "$WORK/watch.out" 2>&1 &
WATCH_PID=$!
i=0
until grep -q '^balance:' "$WORK/watch.out"; do  # its first report: the current value
    i=$((i + 1)); [ $i -gt 120 ] && { cat "$WORK/watch.out"; exit 1; }
    sleep 1
done
head -c 1000 /dev/urandom > "$WORK/small.bin"
# shellcheck disable=SC2086
"$CLI" $V --seed $ALICE_SR store "$WORK/small.bin" > /dev/null
i=0
while kill -0 $WATCH_PID 2>/dev/null; do  # exits after the second report
    i=$((i + 1)); [ $i -gt 120 ] && { cat "$WORK/watch.out"; echo "the watch saw no change"; exit 1; }
    sleep 1
done
cat "$WORK/watch.out"
[ "$(grep -c '^balance:' "$WORK/watch.out")" = 2 ]
[ "$(grep 'nonce:' "$WORK/watch.out" | sort -u | wc -l)" -eq 2 ]
grep -q 'proven from relay' "$WORK/watch.out"

echo "== proven grant to ECDSA //Alice by //Eve"
# shellcheck disable=SC2086
"$CLI" $V --seed $EVE_SR authorize $ALICE_ECDSA_ADDR 4 1000000 | tee "$WORK/grant.out"
grep -q 'verified:   yes' "$WORK/grant.out"

echo "== a CID that was never stored is proven absent"
# shellcheck disable=SC2086
if "$CLI" $V verify bafk2bzaceduekb27ny4kgtaaubzxyw7yelmuhdz4jwglvh4dfgrtvfwcgotgu; then
    echo "expected a proof of absence"; exit 1
fi

echo "== a lying parachain RPC is caught"
# The relay proves the real parachain head; a dev node is another chain.
# Its state cannot match that head's state root.
"$OMNI_NODE" --chain "$WORK/para.json" --dev --tmp --rpc-port 9999 --port 40334 \
    --no-telemetry --no-mdns > "$WORK/liar.log" 2>&1 &
PIDS="$PIDS $!"
wait_for "$WORK/liar.log" 'Running JSON-RPC server' 120
if "$CLI" --rpc http://127.0.0.1:9999 --relay $RELAY --para-id $PARA_ID --checkpoint "$CK" \
        verify "$CID" > "$WORK/liar.out" 2>&1; then
    echo "expected the lying node to be caught"; cat "$WORK/liar.out"; exit 1
fi
cat "$WORK/liar.out"

echo "== e2e (relay) ok"
