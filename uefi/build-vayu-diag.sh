#!/bin/bash
# Build a self-contained vayu XNU diagnostic boot image: UEFI FD + loader +
# the prebuilt XNU kernel and a tiny ramdisk in the XNUP payload.
#
# It does not need a USB link or iOS images. The kernel is the prebuilt
# redwood one; use this only to get the first XNU banner/panic on vayu, not as
# a device kernel. Boot it only after the P1 loader image is proven.
set -e
cd "$(dirname "$0")/.."
. common/lib.sh
load_device vayu

UEFI_FD=${UEFI_FD:-$PWD/Mu-Silicium/Build/${UEFI_TARGET}Pkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd}
[ -f "$UEFI_FD" ] || die "no UEFI FD: $UEFI_FD (run uefi/build-vayu.sh)"
[ -f "$DTB" ] || die "no DTB: $DTB"
[ -f prebuilt/mach.development.bcm2837 ] || die "no prebuilt XNU kernel"
[ -f prebuilt/uefi/ramdisk ] || die "no prebuilt ramdisk"

if command -v clang >/dev/null && command -v lld-link >/dev/null; then
	./xnuloader/build.sh >/dev/null
fi
L=xnuloader/loader.efi
[ -f "$L" ] || die "no loader: $L"

mkdir -p out
KERNEL=${KERNEL:-prebuilt/mach.development.bcm2837}
RAMDISK=${RAMDISK:-prebuilt/uefi/ramdisk}

python3 xnuloader/mkpayload.py -o out/vayu-diag-payload.bin \
	loader.efi="$L" kernel="$KERNEL" ramdisk="$RAMDISK" --platform "$UEFI_TARGET"
FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
	python3 mkxnuimg.py -p out/vayu-diag-payload.bin -o out/vayu-diag.img
./uefi/verify-vayu-bootimg.py out/vayu-diag.img --fd "$UEFI_FD" --dtb "$DTB" --platform vayu --kernel
sha256sum out/vayu-diag.img out/vayu-diag-payload.bin
echo "build: out/vayu-diag.img -- RAM-only XNU diagnostic; boot after P1"
