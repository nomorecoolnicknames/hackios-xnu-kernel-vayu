#!/bin/sh
# K3 host tests: the kernel's own headers (osfmk/arm/vayu_mem_rules.h), compiled by the host's cc with the
# sanitizers, then run. ./run.sh [cc]  (from anywhere; needs a 64-bit host and a C11 compiler: gcc or clang)
set -eu
D=$(cd "$(dirname "$0")" && pwd); T=$(cd "$D/../../../.." && pwd); CC=${1:-${CC:-cc}}
O=${TMPDIR:-/tmp}/vayu-k3-tests.$$; mkdir -p "$O"; trap 'rm -rf "$O"' EXIT
# without address randomization where it can be turned off: clang 14's ASan runtime can crash before main() when the
# kernel randomizes mmap with 32 bits (Ubuntu 22.04 on newer kernels)
R=; if setarch "$(uname -m)" -R true 2>/dev/null; then R="setarch $(uname -m) -R"; fi
rc=0
for t in "$D"/test_*.c; do
	n=$(basename "$t" .c)
	$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
		-I"$T/osfmk" -o "$O/$n" "$t"
	$R "$O/$n" || rc=1
done
exit $rc
