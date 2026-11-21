#!/bin/bash
# Build and run the tests of the kernel's answer to Apple's QMI Audio Service 0xE1
# (bsd/dev/vayu_voice_bb_proto.c, sys/vayu_voice_bb.h) on this machine, under the address and
# undefined-behaviour sanitizers. The requests are written out by hand in test_voice_bb.c, in the
# layouts read from the 20F66 CommCenter (notes in sys/vayu_voice_bb.h).
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/dev/vayu_voice_bb_proto.c" ] || K=$ROOT/xnu/files
B=${TMPDIR:-$ROOT/xnu-deps/build}/voice-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/vayu_voice_bb.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_voice_bb.c" "$K/bsd/dev/vayu_voice_bb_proto.c"
echo "voice-test: with $K/bsd/dev/vayu_voice_bb_proto.c"
"$B/test"
