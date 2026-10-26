#!/bin/bash
# Build and run the tests of the Venus HFI wire format (sys/vayu_venus_hfi.h) on this machine,
# with the header as it is in the kernel tree (or the copy in xnu/files), under the address and
# undefined-behaviour sanitizers.  Nothing of another project is needed or kept: the expected
# bytes in hfi_test.c are written out by hand from Linux's HFI 4xx sources.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/sys/vayu_venus_hfi.h" ] || K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/venus-hfi
rm -rf "$B"
mkdir -p "$B"
FLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$K/bsd" -o "$B/hfi_test" "$HERE/hfi_test.c"
echo "venus-hfi: with $K/bsd/sys/vayu_venus_hfi.h"
"$B/hfi_test"
