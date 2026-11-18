#!/bin/bash
# Build and run the host tests of userland/src/flash_stage.h: the layout flash_stage takes from the command line,
# the image it stages, and the header the loader reads.
#
#   tools/flash-stage-test/run.sh
#   CC=clang ... run.sh                    another compiler
#   FLASH_STAGE_TEST_BUILD=... run.sh      another build directory
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=${FLASH_STAGE_TEST_BUILD:-${TMPDIR:-/tmp}/flash-stage-test-build}
CC=${CC:-cc}

rm -rf "$B"
mkdir -p "$B"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CC $FLAGS -I"$ROOT/userland/src" -o "$B/test" "$HERE/test_flash_stage.c"
"$B/test"
