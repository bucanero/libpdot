#!/bin/sh
# Runs each libFuzzer target for FUZZ_SECONDS (default 60). Crashing inputs
# are copied to /out when it is mounted.
set -eu
SECS=${FUZZ_SECONDS:-60}
sh ci/deps.sh clang libclang-rt-dev
CC=clang cmake -S . -B build -DPDOT_FUZZ=ON -DPDOT_BUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -g -O1" >/dev/null
cmake --build build -j "$(nproc)" --target fuzz_metadata fuzz_decode >/dev/null 2>&1
status=0
for t in metadata decode; do
    mkdir -p "corpus-$t" "crashes-$t"
    cp fuzz/corpus/$t/* "corpus-$t/"
    echo "== fuzz_$t for ${SECS}s"
    if ! ./build/fuzz_$t "corpus-$t" -max_total_time="$SECS" -max_len=262144 \
            -artifact_prefix="crashes-$t/" -print_final_stats=1 > "fuzz-$t.log" 2>&1; then
        status=1
        tail -40 "fuzz-$t.log"
    fi
    grep -E "DONE|stat::number_of_executed_units" "fuzz-$t.log" || true
    if [ -d /out ]; then cp -r "crashes-$t" /out/; fi
done
exit $status
