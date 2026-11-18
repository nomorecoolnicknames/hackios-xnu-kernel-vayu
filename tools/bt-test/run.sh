#!/bin/bash
# Build and run the tests of the Bluetooth UART's rate change (bsd/dev/vayu_bt.c, boot-arg vayu_bt_baud;
# the arithmetic is sys/vayu_bt_baud.h) on this machine, under the address and undefined-behaviour
# sanitizers. Nothing of another project is needed: tools/bt-test/test_bt.c has the numbers written out.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/sys/vayu_bt_baud.h" ] || K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/bt-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/vayu_bt_baud.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_bt.c"
echo "bt-test: with $K/bsd/sys/vayu_bt_baud.h"
"$B/test"
