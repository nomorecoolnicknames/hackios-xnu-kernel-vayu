#!/bin/bash
# The disk that boot_menu=refind starts rEFInd from (the payload entry "refindfs": xnuloader/refind_menu.c):
# a FAT image labelled HAKIOSMENU with
#   \EFI\refind\refind_aa64.efi, refind.conf (refind.conf.in, the timeout from boot_menu_timeout=), theme\
#   \EFI\hakios\loader.efi (the loader given: the same as the payload's)
#
#   mkrefindfs.sh OUT.img REFIND_DIR LOADER [TIMEOUT]
#
# REFIND_DIR: what xnuloader/refind/build.sh made (refind_aa64.efi, theme/). Needs mkfs.fat and mtools (mcopy, mmd).
set -euo pipefail
[ $# -ge 3 ] || { echo "usage: mkrefindfs.sh OUT.img REFIND_DIR LOADER [TIMEOUT]" >&2; exit 2; }
OUT=$1 R=$2 LOADER=$3 TIMEOUT=${4:-8}
HERE=$(cd "$(dirname "$0")" && pwd)
MKFS=$(command -v mkfs.fat || echo /usr/sbin/mkfs.fat)
for f in "$R/refind_aa64.efi" "$R/theme/background.png" "$LOADER"; do [ -f "$f" ] || { echo "mkrefindfs.sh: no $f" >&2; exit 1; }; done
case $TIMEOUT in ''|*[!0-9]*|0) echo "mkrefindfs.sh: timeout $TIMEOUT: a number of seconds above 0" >&2; exit 1 ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed "s/@TIMEOUT@/$((10#$TIMEOUT))/" "$HERE/refind.conf.in" > "$tmp/refind.conf"
# the loader's refind_conf=NAME (refind_menu.c): the same with less of the graphics, one suspect each
sed '/^banner/d' "$tmp/refind.conf" > "$tmp/refind-nobanner.conf"
sed -e 's/theme\\background\.png/theme\\background-half.bmp/' -e 's/selection_\(big\|small\) theme\\selection_big\.png/selection_\1 theme\\selection_big.bmp/' \
	-e 's/^\(\ticon .*\)\.png$/\1.bmp/' -e '/^font /d' "$tmp/refind.conf" > "$tmp/refind-bmp.conf"
sed -e '/^banner/d' -e '/^selection_/d' -e '/^font /d' -e '/^icons_dir /d' -e '/^\ticon /d' "$tmp/refind.conf" > "$tmp/refind-plain.conf"
{ cat "$tmp/refind-plain.conf"; echo; echo "textonly true"; } > "$tmp/refind-text.conf"
# room: what goes in, and half as much again (at least 2 MB), in whole MB
kb=$(( ( $(du -skL "$R/refind_aa64.efi" "$R/theme" "$LOADER" | awk '{s += $1} END {print s}') * 5 / 4 + 1023) / 1024 * 1024 ))
[ $kb -ge 2048 ] || kb=2048
rm -f "$OUT"
truncate -s ${kb}K "$OUT"
"$MKFS" -n HAKIOSMENU -i 48414b49 -S 512 "$OUT" >/dev/null
export MTOOLS_SKIP_CHECK=1
mmd -i "$OUT" ::/EFI ::/EFI/refind ::/EFI/hakios
mcopy -i "$OUT" "$R/refind_aa64.efi" "$tmp"/refind*.conf ::/EFI/refind/
mcopy -i "$OUT" -s "$R/theme" ::/EFI/refind/
mcopy -i "$OUT" "$LOADER" ::/EFI/hakios/loader.efi
echo "$OUT: ${kb} KB, rEFInd $(sha256sum "$R/refind_aa64.efi" | cut -c1-16), loader $(sha256sum "$LOADER" | cut -c1-16), timeout $((10#$TIMEOUT)) s"
