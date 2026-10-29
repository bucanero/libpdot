#!/bin/sh
# Runs a CI script inside a clean container, the same way locally and in
# GitHub Actions:  ci/in-docker.sh PLATFORM SCRIPT [NAME=VALUE...]
#   ci/in-docker.sh linux/amd64 ci/build-test.sh CC=clang SANITIZE=1
# The repository is mounted read-only and copied, so builds never touch it.
# Set OUT to a host directory to collect artifacts in /out.
set -eu
PLATFORM=$1
SCRIPT=$2
shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-}

ENVS=
for kv in "$@"; do
    case $kv in *=*) ENVS="$ENVS -e $kv" ;; esac
done
# shellcheck disable=SC2086
exec docker run --rm --platform "$PLATFORM" -v "$ROOT":/src:ro \
    ${OUT:+-v "$OUT":/out} $ENVS "${CI_IMAGE:-debian:trixie}" \
    sh -ec "cp -a /src /work && cd /work && rm -rf build* && sh $SCRIPT"
