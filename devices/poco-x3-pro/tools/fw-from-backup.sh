#!/bin/bash
# The phone's own firmware out of a copy of its partitions (the backup on the build machine:
# a backup of the partitions, made from the recovery; nothing here talks to the phone). Read only: the copies are
# checked against the backup's SHA256SUMS and read through a temporary copy, never in place.
#
#   fw-from-backup.sh check BACKUP          what the kernel's fw_part=1 will find in the phone's "modem" and
#                                           "bluetooth" partitions (image/...): every file the drivers ask for,
#                                           its size and SHA-256 -- the same partitions are on the phone itself
#   fw-from-backup.sh extract BACKUP OUT    image/ of both into OUT (flat names as the drivers ask: modem.mdt,
#                                           modem_pr/..., crbtfw21.tlv), with OUT/SHA256SUMS: for fw_var=1
#                                           (/private/var/hakios/fw on the phone) or a RAM image's fw/
#
# A flash boot needs neither: with fw_part=1 the kernel reads these partitions on the phone itself
# (xnu: bsd/dev/hakios.image.c, bsd/dev/hakios.fat.c). OUT is the vendor's firmware: never in git, never in
# ../fw-poco-x3-pro (userland/build.sh copies all of that into the boot image, whose payload is at most 63 MB).
# Needs mtools.
set -euo pipefail
export MTOOLS_SKIP_CHECK=1

usage() { echo "usage: $0 check BACKUP | extract BACKUP OUT" >&2; exit 2; }
[ $# -ge 2 ] || usage
MODE=$1 B=$2
[ -d "$B" ] || { echo "fw-from-backup.sh: $B: no such directory" >&2; exit 1; }
case $MODE in
check) [ $# -eq 2 ] || usage ;;
extract)
	[ $# -eq 3 ] || usage
	OUT=$3
	case $(basename "$(readlink -m "$OUT")") in
	fw-poco-x3-pro) echo "fw-from-backup.sh: not into $OUT: build.sh would put 80 MB of it in the boot image" >&2; exit 1 ;;
	esac
	;;
*) usage ;;
esac

T=$(mktemp -d "${TMPDIR:-/tmp}/fw-from-backup.XXXXXX")
trap 'rm -rf "$T"' EXIT

# a partition's copy, checked against the backup's sums and copied (mtools then never opens the backup itself)
take() {
	local p=$1
	[ -f "$B/$p.img" ] || { echo "fw-from-backup.sh: $B/$p.img: missing" >&2; exit 1; }
	if [ -f "$B/SHA256SUMS" ]; then
		(cd "$B" && grep -E "  $p\.img\$" SHA256SUMS | sha256sum -c --quiet -) ||
			{ echo "fw-from-backup.sh: $p.img does not match the backup's SHA256SUMS" >&2; exit 1; }
	else
		echo "fw-from-backup.sh: no SHA256SUMS in $B: $p.img not checked" >&2
	fi
	cp "$B/$p.img" "$T/$p.img"
	file -b "$T/$p.img" | grep -q 'FAT (16 bit)\|FAT (32 bit)' ||
		{ echo "fw-from-backup.sh: $p.img is not a FAT volume" >&2; exit 1; }
}
take modem
take bluetooth

# what the drivers ask for (vayu_pas.c: NAME.mdt and every NAME.bNN; vayu_wifi.c's TFTP: wlanmdsp.mbn, modem_pr/;
# its BDF; vayu_bt.c: the rampatch and NVM of the WCN3990 3.2 it finds, crbtfw21.tlv/crnv21.bin on vayu)
need_modem="modem.mdt wlanmdsp.mbn bdwlan.bin adsp.mdt slpi_vy.mdt"
need_bt="crbtfw21.tlv crnv21.bin"
bad=0
for f in $need_modem; do
	mdir -b -i "$T/modem.img" "::/image/$f" >/dev/null 2>&1 || { echo "MISSING: modem:image/$f"; bad=1; }
done
for f in $need_bt; do
	mdir -b -i "$T/bluetooth.img" "::/image/$f" >/dev/null 2>&1 || { echo "MISSING: bluetooth:image/$f"; bad=1; }
done
mkdir -p "$T/x/modem" "$T/x/bluetooth"
mcopy -s -n -i "$T/modem.img" ::/image "$T/x/modem/"
mcopy -s -n -i "$T/bluetooth.img" ::/image "$T/x/bluetooth/"
for n in modem adsp slpi_vy; do
	segs=$(ls "$T/x/modem/image/" | grep -c "^$n\.b[0-9][0-9]\$" || true)
	echo "modem:image/$n: $n.mdt and $segs segment files"
done
echo "modem:image/modem_pr: $(find "$T/x/modem/image/modem_pr" -type f | wc -l) files (MCFG, served by TFTP)"
echo "modem: $(du -sb "$T/x/modem/image" | cut -f1) bytes in $(find "$T/x/modem/image" -type f | wc -l) files;" \
	"bluetooth: $(find "$T/x/bluetooth/image" -type f | wc -l) files"
if [ "$MODE" = check ]; then
	(cd "$T/x/modem/image" && sha256sum modem.mdt modem.b?? wlanmdsp.mbn bdwlan.bin)
	(cd "$T/x/bluetooth/image" && sha256sum $need_bt)
else
	mkdir -p "$OUT"
	cp -a "$T/x/modem/image/." "$OUT/"
	cp -a "$T/x/bluetooth/image/." "$OUT/"
	(cd "$OUT" && find . -type f ! -name SHA256SUMS -printf '%P\n' | sort | xargs -d '\n' sha256sum > SHA256SUMS)
	echo "fw-from-backup.sh: $OUT: $(wc -l < "$OUT/SHA256SUMS") files, $(du -sh "$OUT" | cut -f1)"
fi
[ $bad -eq 0 ] || { echo "fw-from-backup.sh: FAIL: files missing"; exit 1; }
echo "fw-from-backup.sh: OK"
