#!/bin/sh
# End-to-end run against a dev node from the prebuilt polkadot-omni-node
# release (x86_64 only), with a Kubo gateway.
set -eu
POLKADOT_RELEASE=${POLKADOT_RELEASE:-polkadot-stable2609}
KUBO_VERSION=${KUBO_VERSION:-v0.40.1}
sh ci/deps.sh curl xxd procps

BIN=$(mktemp -d)
base=https://github.com/paritytech/polkadot-sdk/releases/download/$POLKADOT_RELEASE
curl -fsSL -o "$BIN/polkadot-omni-node" "$base/polkadot-omni-node"
curl -fsSL -o "$BIN/omni.sha256" "$base/polkadot-omni-node.sha256"
(cd "$BIN" && sha256sum -c omni.sha256)
chmod +x "$BIN/polkadot-omni-node"
"$BIN/polkadot-omni-node" --version

curl -fsSL "https://github.com/ipfs/kubo/releases/download/$KUBO_VERSION/kubo_${KUBO_VERSION}_linux-amd64.tar.gz" |
    tar -xz -C "$BIN" kubo/ipfs
export PATH="$BIN/kubo:$PATH"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPDOT_BUILD_TESTS=OFF >/dev/null
cmake --build build -j "$(nproc)" --target pdot-bulletin >/dev/null
OMNI_NODE="$BIN/polkadot-omni-node" KUBO=1 sh tools/e2e-devnode.sh build
