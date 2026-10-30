#!/bin/sh
# Read-only verification against live networks: proves Bulletin storage from
# relay-chain GRANDPA finality, trusting the relay node once for the
# checkpoint. Network-dependent, so CI runs it nightly, not per push.
set -eu
sh ci/deps.sh curl python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPDOT_BUILD_TESTS=OFF -DPDOT_BUILD_EXAMPLES=OFF >/dev/null
cmake --build build -j "$(nproc)" --target pdot-bulletin >/dev/null
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

check "Paseo Bulletin Next" https://pas-rpc.stakeworld.io https://paseo-bulletin-next-rpc.polkadot.io 1501
check "Polkadot Bulletin" https://rpc.polkadot.io https://bulletin-rpc.polkadot.io 1010
echo "== verify-live ok"
