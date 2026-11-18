#!/bin/bash
# The power and platform functions of the kernel tree (XNU_SRC=<tree>, else xnu/files) on this machine:
# extract.py takes them out of the driver sources as they are, the tests run them on RAM-backed
# "hardware" with host stubs. SMEM/GLINK bounds and the RPMh lock's verdict (vayu_pas.c), the thermal
# throttle (vayu_cpufreq.c), the battery temperature's freshness and the torch's off path, the SPMI
# cache under concurrency and the redwood cpufreq refusal on vayu.
#   CC=... XNU_SRC=/path/to/tree tools/power-test/run.sh
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu/files}
B=$ROOT/xnu-deps/build/power-test
rm -rf "$B"
mkdir -p "$B"
python3 "$HERE/extract.py" "$K" "$B"
echo "power-test: against $K"
for name in pas thermal guards; do
    ${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$B" -I"$HERE" -o "$B/$name" "$HERE/test_$name.c"
    "$B/$name"
done
