#!/bin/bash
# Build and run the tests of the Venus HFI wire format (sys/vayu_venus_hfi.h) on this machine,
# under the address and undefined-behaviour sanitizers.  The header is copied into a private
# include directory: XNU's bsd/sys must not shadow libc's <sys/...>.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
K=${XNU_SRC:-$(cd "$HERE/../.." && pwd)}
B=$HERE/build
rm -rf "$B"
mkdir -p "$B/include/sys"
cp "$K/bsd/sys/vayu_venus_hfi.h" "$B/include/sys/"
cp "$K/bsd/sys/vayu_venus_pwr.h" "$B/include/sys/"
FLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/include" -o "$B/hfi_test" "$HERE/hfi_test.c"
echo "venus-hfi: with $K/bsd/sys/vayu_venus_hfi.h and $K/bsd/sys/vayu_venus_pwr.h"
"$B/hfi_test"
