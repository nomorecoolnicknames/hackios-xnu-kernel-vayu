#!/bin/bash
# Build and run the host tests of bsd/sys/hakios.ufs_area.h -- where in darwin the kernel lets a write go
# (bsd/dev/hakios.ufs.c) -- as it is in the kernel tree.
#
#   XNU_SRC=/path/to/kernel-tree tools/ufs-area-test/run.sh
#   CC=clang ... run.sh                 another compiler
#   UFS_AREA_TEST_BUILD=... run.sh      another build directory
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu/files}
B=${UFS_AREA_TEST_BUILD:-${TMPDIR:-/tmp}/ufs-area-test-build}
CC=${CC:-cc}

rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hakios.ufs_area.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CC $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_ufs_area.c"
echo "ufs-area-test: against $K"
"$B/test"
