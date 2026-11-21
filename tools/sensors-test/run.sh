#!/bin/bash
# Build and run the host tests of the sensor shims, devices/poco-x3-pro/userland/src/{sensors,motion}.c:
# each file's own thread and loop, compiled for this machine against stand-ins for CoreFoundation and
# the HID event server (stub/, fake.h), driven through scenarios on a fake clock -- the sensor core
# coming late, one device after the other, a device failing and coming back, an ALS change inside the
# throttling interval -- under the address and undefined-behaviour sanitizers.
#   SRC=<dir with sensors.c and motion.c> to test other copies (default: this checkout's).
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${SRC:-$ROOT/devices/poco-x3-pro/userland/src}
B=${TMPDIR:-/tmp}/sensors-test.$$
mkdir -p "$B"; trap 'rm -rf "$B"' EXIT
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -U_FORTIFY_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$HERE/stub" -I"$HERE" -include "$HERE/shim.h" -DSENSORS_C="\"$SRC/sensors.c\"" \
	-o "$B/sensors" "$HERE/test_sensors.c"
${CC:-cc} $FLAGS -I"$HERE/stub" -I"$HERE" -include "$HERE/shim.h" -DMOTION_C="\"$SRC/motion.c\"" \
	-o "$B/motion" "$HERE/test_motion.c"
echo "sensors-test: $SRC"
# the services live as long as backboardd: the scenarios end by a longjmp out of the loop, leaving them
export ASAN_OPTIONS=detect_leaks=0
rc=0
"$B/sensors" || rc=1
"$B/motion" || rc=1
exit $rc
