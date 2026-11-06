#!/bin/sh
# Cross-builds libpdot for 64-bit Windows with MinGW-w64 and runs the unit
# tests under Wine. Debian has no MinGW builds of mbedTLS, curl or cJSON, so
# they are built first from pinned release tarballs into a static prefix.
# curl uses Schannel, Windows' own TLS, as a Windows app would.
#
# LIVE=1 adds read-only checks against live Polkadot. Wine's Schannel fails
# every TLS handshake (SEC_E_INTERNAL_ERROR, plain curl.exe included), so for
# those curl is built with mbedTLS and the container's CA bundle instead.
#   ci/in-docker.sh linux/amd64 ci/mingw.sh [LIVE=1]
set -eu
LIVE=${LIVE:-0}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates curl bzip2 xz-utils cmake make gcc-mingw-w64-x86-64 wine wine64 \
    >/dev/null

PREFIX=/opt/win64
DEPS=/tmp/deps
mkdir -p "$PREFIX" "$DEPS"
HOST=x86_64-w64-mingw32
TOOLCHAIN=$DEPS/mingw.cmake
cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $HOST-gcc)
set(CMAKE_RC_COMPILER $HOST-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$HOST $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
EOF

fetch() {  # url file sha256
    curl -fsSL -m 300 -o "$DEPS/$2" "$1"
    echo "$3  $DEPS/$2" | sha256sum -c - >/dev/null
    tar -xf "$DEPS/$2" -C "$DEPS"
}

dep() {  # source-dir cmake-args...
    src=$1
    shift
    cmake -S "$src" -B "$src/build-win" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
    cmake --build "$src/build-win" -j "$(nproc)" >/dev/null
    cmake --install "$src/build-win" >/dev/null
}

echo "== dependencies for $HOST"
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 \
    mbedtls.tar.bz2 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
dep "$DEPS/mbedtls-3.6.7" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
fetch https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz \
    cjson.tar.gz 7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
dep "$DEPS/cJSON-1.7.19" -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
fetch https://curl.se/download/curl-8.23.0.tar.xz \
    curl.tar.xz 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
if [ "$LIVE" = 1 ]; then
    TLS="-DCURL_USE_SCHANNEL=OFF -DCURL_USE_MBEDTLS=ON -DCURL_CA_PATH=none
         -DCURL_CA_BUNDLE=Z:/etc/ssl/certs/ca-certificates.crt"
else
    TLS=-DCURL_USE_SCHANNEL=ON
fi
# shellcheck disable=SC2086
dep "$DEPS/curl-8.23.0" -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
    $TLS -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_ZLIB=OFF \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF \
    -DCURL_DISABLE_LDAP=ON -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF

echo "== libpdot, $($HOST-gcc --version | head -1)"
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" -DPDOT_WERROR=ON -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build -j "$(nproc)"

echo "== unit tests under $(wine --version)"
export WINEDEBUG=-all WINEPREFIX=/tmp/wine XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
wineboot --init >/dev/null 2>&1 || true
ctest --test-dir build --output-on-failure

# Read-only and network-dependent: the CLI over HTTPS and a WebSocket watch
# against live Polkadot, proven from relay-chain finality.
if [ "$LIVE" = 1 ]; then
    echo "== live, read-only: pdot-bulletin.exe against Polkadot (curl with mbedTLS)"
    CLI="wine build/pdot-bulletin.exe"
    $CLI --rpc https://polkadot-asset-hub-rpc.polkadot.io balance 1zugcag7cJVBtVRnFxv5Qftn7xKAnR6YJ9x4x3XLgGgmNnS
    $CLI --relay https://rpc.polkadot.io --checkpoint dot.ck checkpoint
    V="--rpc https://polkadot-asset-hub-rpc.polkadot.io --relay https://rpc.polkadot.io --para-id 1000 --checkpoint dot.ck"
    # shellcheck disable=SC2086
    $CLI $V proxies 1zugcag7cJVBtVRnFxv5Qftn7xKAnR6YJ9x4x3XLgGgmNnS
    # shellcheck disable=SC2086
    $CLI $V --count 2 watch head
fi
