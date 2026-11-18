#!/bin/bash
# Build and run the tests of the WPA2 handshake's arithmetic (wifi/lx/src/lx_wpa.c) on this
# machine, under the address and undefined-behaviour sanitizers. Needs Python 3 with the
# "cryptography" package: the access point the handshakes are held with is reference.py.
# usage: tools/wpa-test/run.sh [SEED]      another seed, other made-up networks
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=$ROOT/xnu-deps/build/wpa-test
rm -rf "$B"
mkdir -p "$B"
"$HERE/reference.py" "${1:-1}" > "$B/cases.inc"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B" -I"$ROOT/wifi/lx/src" -o "$B/test" "$HERE/test_wpa.c"
"$B/test"

# The vayu station's own handshake (bsd/dev/vayu_wifi_rsn.c of the kernel tree, XNU_SRC=<tree>, else the copy in
# git) against the same access point: its logic as it is, only its XNU includes replaced by host stand-ins.
K=${XNU_SRC:-$ROOT/xnu/files}
sed '/^#include /d' "$K/bsd/dev/vayu_wifi_rsn.c" > "$B/vayu_rsn.inc"
cp "$K/bsd/sys/vayu_wifi_rsn.h" "$B/"           # (not -I bsd/sys: the kernel's headers there would shadow libc's)
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B" -I"$ROOT/wifi/lx/src" -o "$B/test_vayu" "$HERE/test_vayu_rsn.c"
echo "wpa-test: vayu RSN from $K"
"$B/test_vayu"
