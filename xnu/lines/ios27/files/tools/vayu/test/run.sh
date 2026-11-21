#!/bin/bash
# Host tests of the VAYU machine layer (K24): run from anywhere with a C compiler (the build VM has one).
#   gic_host_test   osfmk/arm64/gic_v3_irq.c on a fake distributor (gic_host_test.c says what)
#   psci_host_test  osfmk/arm64/psci_smp.c's CPU state machine and the fail-closed way back (if present)
set -u
H=$(cd "$(dirname "$0")" && pwd); O=${TMPDIR:-/tmp}/vayu-host-tests.$$; mkdir -p $O; rc=0
CC=${CC:-cc}
for t in gic_host_test psci_host_test; do
	[ -f $H/$t.c ] || continue
	$CC -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fsanitize=address,undefined -pthread \
		-I$H $H/$t.c -o $O/$t || { echo "$t: does not build"; rc=1; continue; }
	$O/$t || rc=1
done
rm -rf $O
exit $rc
