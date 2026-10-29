#!/bin/sh
# Debian packages for building and testing libpdot, plus any extras given.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates cmake make gcc libc6-dev pkg-config \
    libmbedtls-dev libcurl4-openssl-dev libcjson-dev "$@" >/dev/null
