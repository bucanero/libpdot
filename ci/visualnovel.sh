#!/bin/sh
# Builds examples/visualnovel (Suika3 with libpdot's chain tags), so a libpdot
# API change that breaks src/chain.c, or a patch that no longer applies,
# fails here. Needs the Suika3 submodule, which a plain checkout skips:
#   git submodule update --init --checkout --recursive examples/visualnovel/suika3
#   ci/in-docker.sh linux/amd64 ci/visualnovel.sh
set -eu
sh ci/deps.sh git ninja-build libx11-dev libxpm-dev libgl-dev libasound2-dev \
    libwayland-dev libxkbcommon-dev libegl-dev libgles-dev libdecor-0-dev wayland-protocols
cd examples/visualnovel
rm -rf build
cmake --preset linux
cmake --build --preset linux -j "$(nproc)"
test -x build/game/suika3 || test -x build/game/Suika3 || { ls build/game; exit 1; }
test -f build/game/start.novel
echo "== examples/visualnovel built: $(ls build/game)"
