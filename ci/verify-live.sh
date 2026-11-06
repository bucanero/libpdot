#!/bin/sh
# Read-only verification against live networks: proves Bulletin storage and
# Polkadot Asset Hub state from relay-chain GRANDPA finality, trusting the
# relay node once for the checkpoint, and checks that each runtime accepts
# libpdot's RFC-0078 metadata hash. Network-dependent, so CI runs it only on manual dispatch;
# locally: ci/in-docker.sh linux/amd64 ci/verify-live.sh
set -eu
sh ci/deps.sh curl python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPDOT_BUILD_TESTS=ON -DPDOT_BUILD_EXAMPLES=OFF >/dev/null
cmake --build build -j "$(nproc)" --target pdot-bulletin check_mdhash >/dev/null
CLI=build/pdot-bulletin
ABSENT=bafk2bzacea3q4eyq7pnoumsahvo6dvqbupmuydbzeso2c4xjfkekvytu6m4xe

# A CID stored right now, found through the (untrusted) node; the proof decides.
stored_cid() {
    curl -fsS -m 60 -H 'Content-Type: application/json' -d \
      '{"id":1,"jsonrpc":"2.0","method":"state_getKeysPaged","params":["0x0e7b504e5df47062be129a8958a7a1270c617bffba7da44e5fd383575a9258b4",1]}' \
      "$1" | python3 -c '
import base64, json, sys
keys = json.load(sys.stdin)["result"]
if keys:
    digest = bytes.fromhex(keys[0][-64:])
    cid = bytes([1, 0x55, 0xa0, 0xe4, 0x02, 32]) + digest
    print("b" + base64.b32encode(cid).decode().lower().rstrip("="))'
}

check() {  # name relay para para_id
    echo "== $1"
    ck=$(mktemp)
    "$CLI" --relay "$2" --checkpoint "$ck" checkpoint
    "$CLI" --rpc "$3" --relay "$2" --para-id "$4" --checkpoint "$ck" \
        authorized 5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY | grep -q 'verified:     yes'
    if "$CLI" --rpc "$3" --relay "$2" --para-id "$4" --checkpoint "$ck" verify $ABSENT; then
        echo "expected a proof of absence"; exit 1
    fi
    cid=$(stored_cid "$3")
    if [ -n "$cid" ]; then
        "$CLI" --rpc "$3" --relay "$2" --para-id "$4" --checkpoint "$ck" verify "$cid"
    else
        echo "nothing stored on $1 right now; presence not checked"
    fi
    rm -f "$ck"
}

echo "== metadata hash accepted by each runtime (validate_transaction, nothing submitted)"
build/check_mdhash https://westend-bulletin-rpc.polkadot.io \
    https://paseo-bulletin-next-rpc.polkadot.io https://bulletin-rpc.polkadot.io

echo "== Polkadot Asset Hub: proven reads and a proven watch"
ck=$(mktemp)
"$CLI" --relay https://rpc.polkadot.io --checkpoint "$ck" checkpoint
AH="--rpc https://polkadot-asset-hub-rpc.polkadot.io --relay https://rpc.polkadot.io --para-id 1000 --checkpoint $ck"
# shellcheck disable=SC2086
"$CLI" $AH asset 1984 | tee ah.out
grep -q 'symbol:     USDt' ah.out
grep -q 'proven from relay' ah.out
# shellcheck disable=SC2086
"$CLI" $AH nft 752 56 | grep -q 'proven from relay'
# shellcheck disable=SC2086
"$CLI" $AH --count 2 watch head | tee watch.out
[ "$(grep -c 'proven from relay' watch.out)" -eq 2 ]
# An account with a Staking proxy (Proxy.Proxies) when this was written, Nov 2026.
PROXIED=1zugcag7cJVBtVRnFxv5Qftn7xKAnR6YJ9x4x3XLgGgmNnS
# shellcheck disable=SC2086
"$CLI" $AH --prefix 0 proxies $PROXIED | tee proxies.out
grep -q 'proxy: .* Staking' proxies.out
grep -q 'proven from relay' proxies.out
rm -f "$ck"

echo "== Polkadot Asset Hub: fee estimates (signed with //Alice's dev key, never submitted)"
DEV=e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a
"$CLI" --rpc https://polkadot-asset-hub-rpc.polkadot.io --seed $DEV --fee \
    transfer $PROXIED 1 | grep -q 'DOT (+ tip)'
"$CLI" --rpc https://polkadot-asset-hub-rpc.polkadot.io --seed $DEV --fee --as $PROXIED \
    transfer-asset 1984 $PROXIED 1.5 | grep -q 'DOT (+ tip)'

check "Paseo Bulletin Next" https://pas-rpc.stakeworld.io https://paseo-bulletin-next-rpc.polkadot.io 1501
check "Polkadot Bulletin" https://rpc.polkadot.io https://bulletin-rpc.polkadot.io 1010
echo "== verify-live ok"
