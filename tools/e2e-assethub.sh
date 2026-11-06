#!/bin/sh
# End-to-end test of transactions on a local Asset Hub dev node: fees,
# transfers of the native token, an asset and an NFT, and a proxy session
# key acting for //Alice. Like e2e-devnode.sh it builds a dev chain spec from
# the runtime deployed on Westend Asset Hub (no Rust build needed) and runs
# polkadot-omni-node. Transactions are signed with CheckMetadataHash.
#
#   OMNI_NODE=/path/to/polkadot-omni-node tools/e2e-assethub.sh [build-dir]
set -eu

BUILD=${1:-build}
CLI=$BUILD/pdot-bulletin
OMNI_NODE=${OMNI_NODE:-polkadot-omni-node}
RUNTIME_RPC=${RUNTIME_RPC:-https://westend-asset-hub-rpc.polkadot.io}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pdot-ah.XXXXXX")
RPC=http://127.0.0.1:9944

# Well-known dev accounts (`subkey inspect //Alice`), funded by the dev preset.
ALICE=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
ALICE_ADDR=5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY
BOB_ADDR=5FHneW46xGXgs5mUiveU4sbTyGBzmstUspZC92UhjJM694ty

NODE_PID=
cleanup() {
    if [ -n "$NODE_PID" ]; then kill "$NODE_PID" 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cli() { "$CLI" --rpc $RPC "$@"; }
fail() { echo "FAIL: $*"; exit 1; }
field() { grep -m1 "$1" | sed "s/.*$1 *//"; }  # value after a label
ok() { grep -q 'dispatch:   ok' "$1" || { cat "$1"; fail "$2"; }; }

echo "== runtime from $RUNTIME_RPC"
curl -fsS -m 300 -H 'Content-Type: application/json' \
    -d '{"id":1,"jsonrpc":"2.0","method":"state_getStorage","params":["0x3a636f6465"]}' \
    "$RUNTIME_RPC" | sed 's/.*"result":"0x\([0-9a-f]*\)".*/\1/' | xxd -r -p > "$WORK/runtime.wasm"
"$OMNI_NODE" chain-spec-builder -c "$WORK/spec.json" create -p 1000 -c westend \
    -i asset-hub-westend -n AssetHub -t local -r "$WORK/runtime.wasm" \
    named-preset development >/dev/null
# CheckMetadataHash covers the token the runtime was built with (WND, 12).
sed 's/"tokenSymbol": *"UNIT"/"tokenSymbol": "WND"/' "$WORK/spec.json" > "$WORK/spec.tmp"
mv "$WORK/spec.tmp" "$WORK/spec.json"
grep -q '"tokenSymbol": "WND"' "$WORK/spec.json"

echo "== starting dev node"
"$OMNI_NODE" --chain "$WORK/spec.json" --dev --base-path "$WORK/node" > "$WORK/node.log" 2>&1 &
NODE_PID=$!
# The Asset Hub runtime is large: on a 4-core CI runner preparing it takes the
# node over 3 minutes, so give the first finalized block NODE_WAIT seconds.
NODE_WAIT=${NODE_WAIT:-900}
i=0
until grep -q 'finalized #[1-9]' "$WORK/node.log"; do
    i=$((i + 1))
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        tail -40 "$WORK/node.log"; fail "the node exited before finalizing a block"
    fi
    if [ $i -gt "$NODE_WAIT" ]; then
        tail -40 "$WORK/node.log"; fail "no finalized block after ${NODE_WAIT}s"
    fi
    sleep 1
done
echo "first block finalized after ${i}s"

echo "== fee estimate, then a transfer of 1.5 WND to //Bob"
cli balance $BOB_ADDR | grep -q 'free:       1000000 WND' || fail "Bob does not start at 1000000"
cli --seed $ALICE --fee transfer $BOB_ADDR 1.5 | tee "$WORK/fee.out"
est=$(field 'units:' < "$WORK/fee.out")
cli --seed $ALICE transfer $BOB_ADDR 1.5 | tee "$WORK/tx.out"
ok "$WORK/tx.out" "transfer"
paid=$(field 'fee:' < "$WORK/tx.out" | cut -d' ' -f1)
[ "$paid" -le "$est" ] || fail "paid $paid over the estimate $est"
cli balance $BOB_ADDR | grep -q 'free:       1000001.5 WND' || fail "Bob did not get 1.5"

echo "== a tip is paid on top of the fee"
cli --seed $ALICE --tip 0.5 --fee transfer $BOB_ADDR 1 > "$WORK/fee.out"
est=$(field 'units:' < "$WORK/fee.out")
cli --seed $ALICE --tip 0.5 transfer $BOB_ADDR 1 > "$WORK/tx.out"
ok "$WORK/tx.out" "tipped transfer"
paid=$(field 'fee:' < "$WORK/tx.out" | cut -d' ' -f1)
[ "$paid" -gt 500000000000 ] && [ "$paid" -le $((est + 500000000000)) ] ||
    fail "tipped: paid $paid, estimate $est + 0.5 WND"

echo "== a session key: one signature by //Alice adds and funds it"
cli session-key > "$WORK/session"
SEED=$(field 'seed:' < "$WORK/session")
SESSION=$(field 'address:' < "$WORK/session")
cli --seed $ALICE proxy-add "$SESSION" Any 1 | tee "$WORK/tx.out"
ok "$WORK/tx.out" "proxy-add"
cli proxies $ALICE_ADDR | tee "$WORK/proxies.out"
grep -q "proxy:      $SESSION Any delay 0" "$WORK/proxies.out" || fail "proxy not listed"
cli balance "$SESSION" | grep -q 'free:       1 WND' || fail "session key not funded"

echo "== the session key pays //Bob 2 WND from //Alice, through pdot_async"
nonce=$(cli balance $ALICE_ADDR | field 'nonce:')
cli --seed "$SEED" --as $ALICE_ADDR --async transfer $BOB_ADDR 2 | tee "$WORK/tx.out"
ok "$WORK/tx.out" "proxied transfer"
cli balance $BOB_ADDR | grep -q 'free:       1000004.5 WND' || fail "Bob did not get 2"
[ "$(cli balance $ALICE_ADDR | field 'nonce:')" = "$nonce" ] || fail "Alice signed"

echo "== an asset and an NFT, created through the generic call command"
cli --seed $ALICE call Assets create "[77, \"$ALICE_ADDR\", 1]" > "$WORK/tx.out"
ok "$WORK/tx.out" "Assets.create"
cli --seed $ALICE call Assets set_metadata '[77, "Gold", "GLD", 2]' > "$WORK/tx.out"
ok "$WORK/tx.out" "Assets.set_metadata"
cli --seed $ALICE call Assets mint "{\"id\": 77, \"beneficiary\": \"$ALICE_ADDR\", \"amount\": 100000}" \
    > "$WORK/tx.out"
ok "$WORK/tx.out" "Assets.mint"
cli --seed $ALICE call Nfts create "[\"$ALICE_ADDR\", {\"settings\": 0, \"max_supply\": null,
    \"mint_settings\": {\"mint_type\": \"Issuer\", \"price\": null, \"start_block\": null,
    \"end_block\": null, \"default_item_settings\": 0}}]" > "$WORK/tx.out"
ok "$WORK/tx.out" "Nfts.create"
cli --seed $ALICE call Nfts mint "[0, 7, \"$ALICE_ADDR\", null]" > "$WORK/tx.out"
ok "$WORK/tx.out" "Nfts.mint"

echo "== the session key moves 12.34 GLD and NFT 0/7 from //Alice to //Bob"
cli --seed "$SEED" --as $ALICE_ADDR transfer-asset 77 $BOB_ADDR 12.34 > "$WORK/tx.out"
ok "$WORK/tx.out" "proxied asset transfer"
cli asset 77 $BOB_ADDR | grep -q 'balance:    12.34 GLD' || fail "Bob did not get 12.34 GLD"
cli --seed "$SEED" --as $ALICE_ADDR transfer-nft 0 7 $BOB_ADDR > "$WORK/tx.out"
ok "$WORK/tx.out" "proxied NFT transfer"
cli nft 0 7 | grep -q "owner:      $BOB_ADDR" || fail "Bob does not own NFT 0/7"

echo "== a NonTransfer session key cannot move WND: the proxied call fails"
cli session-key > "$WORK/session2"
SEED2=$(field 'seed:' < "$WORK/session2")
SESSION2=$(field 'address:' < "$WORK/session2")
cli --seed $ALICE proxy-add "$SESSION2" NonTransfer 1 > "$WORK/tx.out"
ok "$WORK/tx.out" "proxy-add NonTransfer"
if cli --seed "$SEED2" --as $ALICE_ADDR transfer $BOB_ADDR 1 > "$WORK/tx.out" 2>&1; then
    cat "$WORK/tx.out"; fail "a NonTransfer proxy moved WND"
fi
grep -q 'FAILED: System.CallFiltered (in the proxied call)' "$WORK/tx.out" ||
    { cat "$WORK/tx.out"; fail "wrong failure"; }

echo "== closing the session: leftover WND back to //Alice, proxy removed"
cli --seed "$SEED" transfer-all $ALICE_ADDR > "$WORK/tx.out"
ok "$WORK/tx.out" "transfer-all"
cli balance "$SESSION" | grep -q 'no account' || fail "session account still exists"
cli --seed $ALICE proxy-remove "$SESSION" Any > "$WORK/tx.out"
ok "$WORK/tx.out" "proxy-remove"
cli proxies $ALICE_ADDR > "$WORK/proxies.out"
grep -q "$SESSION" "$WORK/proxies.out" && fail "proxy still listed"
grep -q "$SESSION2 NonTransfer" "$WORK/proxies.out" || fail "NonTransfer proxy gone"

echo "== bad arguments are named"
if cli --seed $ALICE --fee call Balances transfer_keep_alive "[\"$BOB_ADDR\", \"abc\"]" \
        > "$WORK/err.out" 2>&1; then fail "bad call accepted"; fi
grep -q 'transfer_keep_alive.value: expected an unsigned integer' "$WORK/err.out" ||
    { cat "$WORK/err.out"; fail "error does not name the argument"; }

echo "== e2e (Asset Hub) ok"
