#!/bin/bash
# Build and run the tests of the touch-to-pointer gesture state machine (iokit/Kernel/VayuTouchGesture.h)
# on this machine, under the address and undefined-behaviour sanitizers.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
K=${XNU_SRC:-$(cd "$HERE/../.." && pwd)}
B=$HERE/build
rm -rf "$B"
mkdir -p "$B/include"
cp "$K/iokit/Kernel/VayuTouchGesture.h" "$B/include/"
FLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$B/include" -o "$B/gesture_test" "$HERE/gesture_test.c"
echo "vayu-touch-gesture: with $K/iokit/Kernel/VayuTouchGesture.h"
"$B/gesture_test"
