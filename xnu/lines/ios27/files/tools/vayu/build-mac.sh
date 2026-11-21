#!/bin/bash
# tools/vayu/build-mac.sh [-jN] [MACHINE] [CONFIG]: build this tree's kernel for VAYU (default; STARLTE etc. also work) on a Mac,
# with Xcode (DEVELOPER_DIR) and the Kernel Debug Kit 26.5 (KDKROOT), using what Scion's own iOS-target build has already
# made in its darwin-xnu-build directory (DXB, default ~/hakios-build/scion-kernel/build/dxb-12377): the fakeroot with mig,
# the CTF tools, availability.pl and the firehose headers, and the iOS libfirehose_kernel.a (build/firehose-ios). Those are
# made once by Scion's build of its STARLTE reference:
#   git clone https://github.com/Scion-Project/scion-kernel && cd scion-kernel && OS_PROFILE=ios-27 NOWIFI=1 tools/ios27/build-xnu-ios.sh -j4
# The same make command line as Scion's (tools/ios27/dxb-ios.patch of scion-kernel), with this tree as SRCROOT and the
# objects in BUILD/obj of it. The kernel: BUILD/obj/<CONFIG>_ARM64_<MACHINE>/mach.<config>.<machine>.
# darwin-xnu-build is blacktop's (MIT); the KDK and Xcode are Apple's and are not in this tree.
set -euo pipefail
jobs=4
case "${1:-}" in -j*) jobs=${1#-j}; shift ;; esac
MACHINE=${1:-VAYU}; CONFIG=${2:-DEVELOPMENT}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
DXB=${DXB:-$HOME/hakios-build/scion-kernel/build/dxb-12377}
: "${KDKROOT:?KDKROOT: the unpacked Kernel Debug Kit 26.5 (25F71)}" "${DEVELOPER_DIR:?DEVELOPER_DIR: an Xcode}"
FR=$DXB/fakeroot
for f in "$FR/usr/local/bin/ctfconvert" "$DXB/build/firehose-ios/libfirehose_kernel.a"; do
	[ -e "$f" ] || { echo "build-mac: no $f: run Scion's STARLTE build first (see the top of this file)" >&2; exit 1; }
done
# CoreEntitlements V2 from the KDK (scion-kernel xnu/stage/00_setup_coreentitlements.sh does the same): not in git
CE=$KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements/V2
mkdir -p "$SRC/EXTERNAL_HEADERS/CoreEntitlements/V2"
cp "$CE"/*.h "$SRC/EXTERNAL_HEADERS/CoreEntitlements/V2/"
cp "$KDKROOT/System/Library/KernelSupport/CoreEntitlements/V2/Kernel.h" "$SRC/EXTERNAL_HEADERS/CoreEntitlements/V2/Kernel.h"
EXCL=$(git -C "$SRC" rev-parse --git-path info/exclude); mkdir -p "$(dirname "$EXCL")"
for p in /EXTERNAL_HEADERS/CoreEntitlements/V2/ /BUILD/; do grep -qxF "$p" "$EXCL" 2>/dev/null || echo "$p" >> "$EXCL"; done
OBJ=$SRC/BUILD/obj SYM=$SRC/BUILD/sym DST=$SRC/BUILD/dst
mkdir -p "$OBJ" "$SYM" "$DST"
K=$OBJ/${CONFIG}_ARM64_${MACHINE}/mach.$(echo "$CONFIG" | tr A-Z a-z).$(echo "$MACHINE" | tr A-Z a-z)
rm -f "$K"
cd "$SRC"
export KERNEL_BUILD_DATE="${KERNEL_BUILD_DATE:-$(TZ=UTC git log -1 --format=%cd --date=format-local:'%a %b %e %T %Z %Y')}"
export KERNEL_BUILD_OBJROOT="${KERNEL_BUILD_OBJROOT:-xnu-12377.121.6-vayu}"
rc=0
make install -k -j"$jobs" VERBOSE=${VERBOSE:-NO} \
	CTFCONVERT=$FR/usr/local/bin/ctfconvert CTFMERGE=$FR/usr/local/bin/ctfmerge CTFDUMP=$FR/usr/local/bin/ctfdump \
	SDKROOT=iphoneos UBSAN_CHECKS= UBSAN_RUNTIME= "LDFLAGS_KERNEL_SDK=-L$DXB/build/firehose-ios -lfirehose_kernel" \
	TARGET_CONFIGS="$CONFIG ARM64 $MACHINE" CONCISE=0 LOGCOLORS=n BUILD_WERROR=0 BUILD_LTO=0 \
	SRCROOT="$SRC" OBJROOT="$OBJ" SYMROOT="$SYM" DSTROOT="$DST" FAKEROOT_DIR="$FR" KDKROOT="$KDKROOT" \
	TIGHTBEAMC=tightbeamc-not-supported RC_DARWIN_KERNEL_VERSION=25.5.0 \
	MEMORY_SIZE=$(sysctl -n hw.memsize) SYSCTL_HW_PHYSICALCPU=$jobs SYSCTL_HW_LOGICALCPU=$jobs KERNEL_BUILDS_IN_PARALLEL=1 || rc=$?
if [ -f "$K" ]; then
	echo "build-mac: kernel $K $(shasum -a 256 "$K" | cut -c1-16) (make rc=$rc)"
else
	echo "build-mac: NO KERNEL (make rc=$rc)"; exit 1
fi
