#!/bin/bash
# Build and run the host tests of bsd/sys/vayu_sms_qmi.h against the QMI coding as
# it is in the kernel tree.  Nothing of another project is needed: test_sms.c has
# every PDU and QMI message written out by hand.
#
#   XNU_SRC=/mnt/ramdisk/ds-sms tools/sms-test/run.sh
#   CC=clang ... run.sh          another compiler
#   SMS_TEST_BUILD=... run.sh    another build directory
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu/files}
SMS=${SMS_HEADER:-$K/bsd/sys/vayu_sms_qmi.h}
B=${SMS_TEST_BUILD:-/tmp/opencode/sms-test-build}
CC=${CC:-cc}

rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hakios.qmi.h" "$K/bsd/sys/hakios.qrtr.h" "$K/bsd/sys/vayu_sms.h" "$SMS" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CC $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_sms.c" "$K/bsd/dev/hakios.qmi.c"
echo "sms-test: against $K"
"$B/test"
