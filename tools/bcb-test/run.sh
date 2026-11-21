#!/bin/bash
# Build and run the host tests of xnuloader/bcb.h: what the loader lets be written into misc's boot control block,
# what it writes there itself, and darwin's count of starts (boot_tries=N).
#
#   tools/bcb-test/run.sh
#   CC=clang ... run.sh             another compiler
#   BCB_TEST_BUILD=... run.sh       another build directory
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=${BCB_TEST_BUILD:-${TMPDIR:-/tmp}/bcb-test-build}
CC=${CC:-cc}

rm -rf "$B"
mkdir -p "$B"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CC $FLAGS -I"$ROOT/xnuloader" -o "$B/test" "$HERE/test_bcb.c"
"$B/test"
