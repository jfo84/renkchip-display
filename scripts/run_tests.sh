#!/bin/sh
# run_tests.sh — host-build and run the demux tests. No hardware, no
# vendor headers: the parse path is pure C, this is the point.
# For the real value, rebuild with -fsanitize=address,undefined and
# give the fuzzer more iterations. Coverage-guided tier (found the PMT
# over-read; needs clang with the libFuzzer runtime, e.g. Linux):
#   clang -g -O1 -fsanitize=fuzzer,address,undefined \
#     -fno-sanitize-recover=all -o /tmp/fuzz_demux tests/lfuzz.c src/demux.c -Isrc
#   mkdir -p /tmp/seeds && /tmp/test_demux --seeds /tmp/seeds
#   /tmp/fuzz_demux -use_value_profile=1 /tmp/corpus /tmp/seeds
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
BIN=/tmp/test_demux
$CC -Wall -Wextra -O2 -o "$BIN" tests/test_demux.c src/demux.c -Isrc
"$BIN" && "$BIN" --fuzz 50000
