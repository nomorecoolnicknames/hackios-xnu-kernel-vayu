#!/bin/bash
# Build and run the host tests of the QMux<->QRTR bridge's protocol core
# (bsd/dev/vayu_qmux_proto.c, the file the kernel links) under the address and
# undefined-behaviour sanitizers. No kernel and no modem: byte vectors in, frames parsed back.
# The test is the lab's host/test_qmux.c (ds-qmux1, qmux1-0002-vayu-qmux-userland.patch), plus the bridge's
# side of the kernel's own services (0xE1 through bsd/dev/vayu_voice_bb_proto.c, 0xE2) and the per-client port routing.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in
[ -f "$K/bsd/dev/vayu_qmux_proto.c" ] || { echo "qmux-test: XNU_SRC has no bsd/dev/vayu_qmux_proto.c"; exit 1; }
B=${TMPDIR:-/tmp}/qmux-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/vayu_qmux.h" "$K/bsd/sys/vayu_voice_bb.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/inc" -I"$K/bsd/dev" -o "$B/test" "$HERE/test_qmux.c" "$K/bsd/dev/vayu_qmux_proto.c" \
    "$K/bsd/dev/vayu_voice_bb_proto.c"
echo "qmux-test: with $K/bsd/dev/vayu_qmux_proto.c"
"$B/test"
