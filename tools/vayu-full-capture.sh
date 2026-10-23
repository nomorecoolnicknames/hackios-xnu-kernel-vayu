#!/bin/bash
# Capture a full-system vayu run before the loader's USB link can disappear.
#
# Run this from /home/gun on gunwest, with the phone already in the P1 loader
# (fastboot boot /home/gun/vayu-boot.img). It starts `xl run` in the
# background and takes a screenshot and a log at 45/90/135/180 s, then stops
# and reboots the phone. The full system may keep running; the loader's USB
# can vanish when the battery dies, so the captures are taken early.
#
#   ./vayu-full-capture.sh
#   XL_SECS=240 ./vayu-full-capture.sh
#   XL_CMDLINE="bootcpu=7 cpus=1 vtimer_intid=19" ./vayu-full-capture.sh
set -u
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=logs/${STAMP}-full
SECS=${XL_SECS:-180}
CMDLINE=${XL_CMDLINE:-bootcpu=7 cpus=1 vtimer_intid=19}

state=$(./xl state 2>&1 || true)
if [ "$state" != loader ]; then
	echo "vayu-full-capture: phone is '$state', need the P1 loader" >&2
	echo "vayu-full-capture: fastboot boot /home/gun/vayu-boot.img, wait for xnuloader" >&2
	exit 1
fi

echo "vayu-full-capture: run $SECS s; artifacts ${LOG}.*"
XL_SECS="$SECS" XL_CMDLINE="$CMDLINE" ./xl run > "${LOG}.run" 2>&1 &
RUNPID=$!

for t in 45 90 135 180; do
	sleep 45
	echo "vayu-full-capture: t=${t}s"
	timeout 30 ./xl screenshot "${LOG}-screen-${t}.png" 2>&1 || true
	timeout 30 ./xl log > "${LOG}-kernel-${t}.log" 2>&1 || true
	timeout 10 ./xl state 2>&1 || true
done

# Do not wait forever for a full launchd that never returns.
for _ in 1 2 3 4 5; do
	kill -0 "$RUNPID" 2>/dev/null || break
	sleep 1
done
./xl stop 2>&1 || true
kill "$RUNPID" 2>/dev/null || true
wait "$RUNPID" 2>/dev/null || true
sleep 5
./xl reboot 2>&1 || true
echo "vayu-full-capture: done; files:"
ls -lh "${LOG}".* 2>/dev/null || true
