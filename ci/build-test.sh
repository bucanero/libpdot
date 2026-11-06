#!/bin/sh
# Builds with warnings as errors and runs the unit tests.
#   CC=gcc|clang  SANITIZE=0|1|thread  BUILD_TYPE=Release|Debug
set -eu
CC=${CC:-gcc}
SANITIZE=${SANITIZE:-0}
BUILD_TYPE=${BUILD_TYPE:-Release}
EXTRA=
[ "$CC" = clang ] && EXTRA="clang libclang-rt-dev"
# shellcheck disable=SC2086
sh ci/deps.sh $EXTRA
echo "== $(uname -m), $($CC --version | head -1), sanitize=$SANITIZE"
if [ "$SANITIZE" = thread ]; then
    CC=$CC cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPDOT_WERROR=ON \
        -DCMAKE_C_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
else
    CC=$CC cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DPDOT_WERROR=ON -DPDOT_SANITIZE="$SANITIZE"
fi
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
