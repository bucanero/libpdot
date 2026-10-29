#!/bin/sh
# Runs the libFuzzer targets in a Linux container (Apple clang has no libFuzzer).
# Crashes and the grown corpus land in $OUT.
#   SECONDS_PER_TARGET=300 tools/fuzz.sh
set -e
OUT=${OUT:-$PWD/build-fuzz}
SECS=${SECONDS_PER_TARGET:-120}
mkdir -p "$OUT"
case $(uname -m) in arm64|aarch64) PLATFORM=linux/arm64 ;; *) PLATFORM=linux/amd64 ;; esac
docker run --rm --platform $PLATFORM -v "$(cd "$(dirname "$0")/.." && pwd)":/src:ro -v "$OUT":/out \
  -e SECS="$SECS" debian:trixie sh -ec '
    apt-get update -qq >/dev/null
    apt-get install -y -qq clang cmake make libmbedtls-dev libcurl4-openssl-dev libcjson-dev >/dev/null 2>&1
    cp -r /src /work && cd /work && rm -rf build*
    CC=clang cmake -S . -B build -DPDOT_FUZZ=ON -DPDOT_BUILD_TESTS=OFF \
      -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -g -O1" >/dev/null
    cmake --build build -j4 --target fuzz_metadata fuzz_decode >/dev/null 2>&1
    for t in metadata decode; do
      mkdir -p /out/corpus-$t /out/crashes-$t
      cp fuzz/corpus/$t/* /out/corpus-$t/
      echo "== fuzz_$t for ${SECS}s"
      ./build/fuzz_$t /out/corpus-$t -max_total_time=$SECS -max_len=262144 \
        -artifact_prefix=/out/crashes-$t/ -print_final_stats=1 2>&1 |
        grep -E "DONE|stat::number_of_executed_units|ERROR|SUMMARY|Test unit written" | tail -6
    done'
