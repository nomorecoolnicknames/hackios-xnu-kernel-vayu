#!/bin/bash
# Build and run the tests of the two auto-join answers of bsd/dev/hakios.apple80211.c (sys/hakios.apple80211.h:
# RSSI_BOUNDS, vayu_wifi_bounds; the noise floor and SNR, vayu_wifi_snr) on this machine, under the sanitizers.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}     # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/sys/hakios.apple80211.h" ] || K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/a80211-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hakios.apple80211.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_a80211.c"
echo "a80211-test: with $K/bsd/sys/hakios.apple80211.h"
"$B/test"
