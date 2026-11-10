#!/bin/sh
# build_bump.sh — cross-compile bump for the stick (armv7a, API 21).
# Needs the vendor's legacy VPU headers and libvpu.so next to bump.c —
# pull them from your own device or SDK; they are not in this repo.
set -e
cd "$(dirname "$0")/../src"
CC="${NDK_CC:-armv7a-linux-androideabi21-clang}"
$CC -O2 -Wall -Wextra -o bump bump.c demux.c -I. -L. -lvpu
file bump 2>/dev/null || ls -la bump
