#!/bin/bash
# rEFInd for vayu's boot_menu=refind, from its sources (it is GPL v3: these sources and patches/ are what the
# binary is made of):
#   rEFInd   https://git.code.sf.net/p/refind/code  tag v.0.14.2  commit 6c66e90b3d36c1b4c7b0067203be2ba99a0380fb
#   gnu-efi  https://github.com/ncroxon/gnu-efi     tag 3.0.18    commit 2b0c490219f613fdd7f4049799f04bfabd7b1fab
#   patches/0001-vertical-menu.patch (ours): "vertical_menu true" -- the main menu as one column of rows for a
#            portrait screen, Vol+/Vol- (up/down) through every entry with wrap; rEFInd's own variables never
#            written to NVRAM when its volume is read-only and use_nvram is false
# and the theme drawn by mktheme.py (Pillow; DejaVu fonts).
#
#   build.sh OUTDIR        -> OUTDIR/refind_aa64.efi, OUTDIR/theme/, OUTDIR/SOURCES (what was built from)
#
# Needs: git, make, aarch64-linux-gnu-gcc with libc6-dev-arm64-cross (rEFInd's lodepng includes libc headers),
# python3 with Pillow (PYTHON=, e.g. a venv's), the DejaVu fonts. WORK= (default OUTDIR/src) keeps the clones.
set -euo pipefail
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$OUT/src}
PYTHON=${PYTHON:-python3}
REFIND_GIT=https://git.code.sf.net/p/refind/code REFIND_COMMIT=6c66e90b3d36c1b4c7b0067203be2ba99a0380fb
GNUEFI_GIT=https://github.com/ncroxon/gnu-efi GNUEFI_COMMIT=2b0c490219f613fdd7f4049799f04bfabd7b1fab
CROSS=aarch64-linux-gnu-
mkdir -p "$WORK"
get() {         # get DIR URL COMMIT
	[ -d "$WORK/$1/.git" ] || git clone -q "$2" "$WORK/$1"
	git -C "$WORK/$1" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$WORK/$1" fetch -q origin
	git -C "$WORK/$1" checkout -q -f "$3"
	git -C "$WORK/$1" clean -q -fdx
}
get gnu-efi $GNUEFI_GIT $GNUEFI_COMMIT
get refind $REFIND_GIT $REFIND_COMMIT
G=$WORK/gnuefi-aa64
make -s -C "$WORK/gnu-efi" ARCH=aarch64 CROSS_COMPILE=$CROSS -j"$(nproc)" >/dev/null
make -s -C "$WORK/gnu-efi" ARCH=aarch64 CROSS_COMPILE=$CROSS PREFIX="$G" install >/dev/null
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/refind" -p1 < "$p"; done
make -s -C "$WORK/refind" gnuefi ARCH=aarch64 GNUEFI_ARM64_TARGET_SUPPORT=y EFIINC="$G/include/efi" \
	GNUEFILIB="$G/lib" EFILIB="$G/lib" EFICRT0="$G/lib" >"$WORK/refind-build.log" 2>&1 ||
	{ tail -20 "$WORK/refind-build.log"; exit 1; }
cp "$WORK/refind/refind/refind_aa64.efi" "$OUT/"
rm -rf "$OUT/theme"
"$PYTHON" -I "$HERE/mktheme.py" "$OUT/theme" >/dev/null
{
	echo "rEFInd  $REFIND_GIT $REFIND_COMMIT (v.0.14.2), GPL v3"
	echo "gnu-efi $GNUEFI_GIT $GNUEFI_COMMIT (3.0.18)"
	for p in "$HERE"/patches/*.patch; do echo "patch   $(basename "$p") $(sha256sum "$p" | cut -c1-16)"; done
	echo "theme   mktheme.py $(sha256sum "$HERE/mktheme.py" | cut -c1-16)"
	echo "refind_aa64.efi $(sha256sum "$OUT/refind_aa64.efi" | cut -d' ' -f1)"
} > "$OUT/SOURCES"
cat "$OUT/SOURCES"
