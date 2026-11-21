#!/bin/bash
# Host tests of K5 (the display): iokit/Kernel/arm/vayu_fb_rules.h, gcc and clang with ASan/UBSan where present.
set -u   # CCS="gcc clang" by default; a missing one is skipped, none at all is a failure
H=$(cd "$(dirname "$0")" && pwd); S=$H/../../../..; O=${TMPDIR:-/tmp}/vayu-ak5-tests.$$; mkdir -p $O; rc=0; ran=0
for cc in ${CCS:-gcc clang}; do
	command -v $cc >/dev/null || continue
	$cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all \
		-I$S/iokit/Kernel/arm $H/test_fb_rules.c -o $O/test_fb_rules.$cc || { echo "$cc: does not build"; rc=1; continue; }
	echo -n "$cc: "; ran=1; setarch $(uname -m) -R $O/test_fb_rules.$cc || rc=1
done
rm -rf $O
[ $ran = 1 ] || { echo "no compiler ran"; rc=1; }
exit $rc
