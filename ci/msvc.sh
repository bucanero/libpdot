#!/bin/sh
# Builds libpdot with MSVC on Windows and runs the unit tests. Runs in Git
# Bash on a GitHub windows runner (CMake and Visual Studio preinstalled).
# mbedTLS, curl (with Schannel, Windows' own TLS) and cJSON are built first
# from the same pinned release tarballs as ci/mingw.sh, into a static prefix.
#   ci/msvc.sh [WERROR=1]   WERROR=1: MSVC warnings in libpdot's own code are errors
set -eu
for kv in "$@"; do
    case $kv in WERROR=*) WERROR=${kv#WERROR=} ;; esac
done
WERROR=${WERROR:-0}
WORK=$(pwd)
DEPS=$WORK/build-deps
PREFIX=$DEPS/prefix
mkdir -p "$DEPS" "$PREFIX"

fetch() {  # url file sha256
    curl -fsSL -m 300 -o "$DEPS/$2" "$1"
    echo "$3  $DEPS/$2" | sha256sum -c - >/dev/null
    tar -xf "$DEPS/$2" -C "$DEPS"
}

dep() {  # source-dir cmake-args...
    src=$1
    shift
    cmake -S "$src" -B "$src/build-msvc" -A x64 -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
    cmake --build "$src/build-msvc" --config Release -j 4 -- -v:q -nologo >/dev/null
    cmake --install "$src/build-msvc" --config Release >/dev/null
}

echo "== dependencies (MSVC x64)"
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 \
    mbedtls.tar.bz2 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
dep "$DEPS/mbedtls-3.6.7" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
fetch https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz \
    cjson.tar.gz 7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
dep "$DEPS/cJSON-1.7.19" -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
fetch https://curl.se/download/curl-8.23.0.tar.xz \
    curl.tar.xz 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
dep "$DEPS/curl-8.23.0" -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_ZLIB=OFF \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF \
    -DCURL_DISABLE_LDAP=ON -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF

echo "== libpdot (WERROR=$WERROR)"
cmake -S . -B build -A x64 -DCMAKE_PREFIX_PATH="$PREFIX" -DPDOT_WERROR="$WERROR"
# Each distinct warning is listed once (vendored code's included, never errors).
cmake --build build --config Release -j 4 -- -nologo 2>&1 | tee build/msbuild.log
grep -E "warning C[0-9]+" build/msbuild.log | sed 's/^ *//' | sort -u > build/warnings.txt || true
echo "== $(wc -l < build/warnings.txt) distinct MSVC warnings"
cat build/warnings.txt
if grep -qE "error C[0-9]+|error LNK[0-9]+|: fatal error" build/msbuild.log; then
    echo "build failed"
    exit 1
fi

echo "== unit tests"
ctest --test-dir build -C Release --output-on-failure
