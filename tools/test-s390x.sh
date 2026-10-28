#!/bin/sh
# Builds libpdot and runs the unit tests on big-endian s390x, under Docker's
# QEMU emulation. Signatures go to $OUT/siglog-s390x.txt, so sp-core can check
# them: tools/fixtures-gen verify < $OUT/siglog-s390x.txt
set -e
OUT=${OUT:-$PWD/build-s390x}
mkdir -p "$OUT"
docker run --rm --platform linux/s390x \
  -v "$(cd "$(dirname "$0")/.." && pwd)":/src:ro -v "$OUT":/out \
  debian:trixie sh -ec '
    apt-get update -qq >/dev/null
    apt-get install -y -qq cmake gcc make libmbedtls-dev libcurl4-openssl-dev libcjson-dev >/dev/null 2>&1
    cp -r /src /work && cd /work && rm -rf build*
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build -j4 >/dev/null
    ctest --test-dir build --output-on-failure
    PDOT_SIGLOG=/out/siglog-s390x.txt ./build/test_signer'
