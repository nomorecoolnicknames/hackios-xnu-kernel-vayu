#!/bin/bash
# Build and run the tests of bsd/dev/vayu_gnss.c's QMI LOC messages (sys/vayu_gnss_qmi.h) on this
# machine, against the QMI coding as it is in the kernel (bsd/dev/hakios.qmi.c), under the
# address and undefined-behaviour sanitizers. Nothing of another project is needed or kept:
# tools/gnss-test/test_gnss.c has the messages written out by hand.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/dev/hakios.qmi.c" ] || K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/gnss-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hakios.qmi.h" "$K/bsd/sys/hakios.qrtr.h" "$K/bsd/sys/vayu_gnss.h" \
	"$K/bsd/sys/vayu_gnss_qmi.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_gnss.c" "$K/bsd/dev/hakios.qmi.c"
echo "gnss-test: with $K/bsd/dev/hakios.qmi.c"
"$B/test"
