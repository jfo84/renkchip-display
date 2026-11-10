#!/bin/sh
# run_tests.sh — host-build and run the demux tests. No hardware, no
# vendor headers: the parse path is pure C, this is the point.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
$CC -Wall -Wextra -O2 -o /tmp/test_demux tests/test_demux.c src/demux.c -Isrc
/tmp/test_demux
