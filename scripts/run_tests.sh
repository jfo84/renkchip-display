#!/bin/sh
# run_tests.sh — host-build and run the demux tests. No hardware, no
# vendor headers: the parse path is pure C, this is the point.
# For the real value, rebuild with -fsanitize=address,undefined and
# give the fuzzer more iterations.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
BIN=/tmp/test_demux
$CC -Wall -Wextra -O2 -o "$BIN" tests/test_demux.c src/demux.c -Isrc
"$BIN" && "$BIN" --fuzz 50000
