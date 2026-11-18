#!/bin/bash
# vayu: the port's partition darwin (sda33, 24 GiB beside Android's userdata, which stays as it is), from the
# recovery (PBRP, adb as root), on the computer the phone is plugged into. Never partitions anything: darwin
# is there already. Where everything inside it goes is darwin-layout.py's (the one place the numbers are made);
# what darwin is -- its GPT entry, GUIDs, LBAs, and the phone's own view of it in sysfs -- is checked before
# every write, again inside the same adb shell right before dd, and every write is read back by sha256.
#
#   darwin-install.sh check                darwin is this phone's darwin, now                            (writes nothing)
#   darwin-install.sh status               check, then what darwin holds: the layout block, the containers at
#                                          root/oscryptex/appcryptex, the volume at var, var_store's slots,
#                                          a staged boot image                                           (writes nothing)
#   darwin-install.sh dump-gpt DIR         the first and last 6 blocks of sda and sde into DIR, checked   (writes nothing)
#   darwin-install.sh sum BLOCK COUNT      sha256 of COUNT blocks of darwin from BLOCK                   (writes nothing)
#   darwin-install.sh layout               darwin-layout.py's block into darwin's block LAYOUT_BLOCK (first, before
#                                          anything else: the other writes want it there)
#   darwin-install.sh write REGION IMAGE   REGION = root | oscryptex | appcryptex: IMAGE, an APFS container that
#                                          fills the region exactly (the kernel finds each container where the one
#                                          before ends), at the region's first block
#   darwin-install.sh var IMAGE            /private/var's HFS Plus volume (var-volume.sh makes it) into the var
#                                          region: only what mkfs wrote, each piece read back. Refused if a volume
#                                          is there already (VAR_REINIT=yes: that erases /private/var)
#
# Every write needs DARWIN_WRITE=yes. Nothing here ever writes var_store's slots, the stage, any partition but
# darwin (by its node, /dev/block/sda33, checked to be darwin), or below darwin's start. The serial is explicit:
# SERIAL (the phone's, as adb devices shows it: required, there is no default); ADB (default adb). Every attempt
# goes into logs/darwin-install.log.
#
# After another HakiOS port (MIT): telegdyd/ios-vayu, branch vayu-dev, commits 56f0a3adbd41 (its darwin-install.sh) and
# 274f9827f52f (the var region).
set -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
LAYOUT="python3 -I $HERE/darwin-layout.py --profile ${DARWIN_PROFILE:-vayu}"     # (qemu: the test disk's numbers, for tests)
eval "$($LAYOUT sh)" || { echo "darwin-install: darwin-layout.py refused its own layout"; exit 1; }
SERIAL=${SERIAL:-}
A="${ADB:-adb} -s $SERIAL"
LOG=$TOP/logs/darwin-install.log
mkdir -p "$TOP/logs"
log() { echo "$(date '+%F %T') $*" >> "$LOG"; echo "darwin-install: $*"; }
die() { log "REFUSED: $*"; exit 1; }
sh_phone() { $A shell "$1" | tr -d '\r'; }
# the node, checked in the same shell right before the command that writes it
GUARD="[ \"\$(readlink -f /dev/block/by-name/darwin)\" = $DARWIN_NODE ] && [ \"\$(blockdev --getsize64 $DARWIN_NODE)\" = $((DARWIN_BLOCKS * BS)) ]"

check() {
	local facts want
	[ "$($A get-state 2>/dev/null | tr -d '\r')" = recovery ] || die "the phone ($SERIAL) is not in the recovery"
	[ "$(sh_phone "id -u")" = 0 ] || die "adb is not root"
	[ "$(sh_phone "blockdev --getss /dev/block/$DARWIN_DISK")" = $BS ] || die "$DARWIN_DISK does not have blocks of $BS"
	$A exec-out "dd if=/dev/block/$DARWIN_DISK bs=$BS count=6 2>/dev/null" | $LAYOUT gpt $DARWIN_DISK - >/dev/null ||
		die "the GPT of $DARWIN_DISK is not the one darwin-layout.py knows (run: $0 dump-gpt DIR, and compare)"
	facts=$(sh_phone "readlink -f /dev/block/by-name/darwin; cat /sys/class/block/$DARWIN_DISK$DARWIN_PART/partition /sys/class/block/$DARWIN_DISK$DARWIN_PART/start /sys/class/block/$DARWIN_DISK$DARWIN_PART/size; grep ^PARTNAME= /sys/class/block/$DARWIN_DISK$DARWIN_PART/uevent; blockdev --getsize64 $DARWIN_NODE; blockdev --getsize64 /dev/block/$DARWIN_DISK; grep -cw '$DARWIN_DISK$DARWIN_PART' /proc/mounts" | tr '\n' ' ')
	want="$DARWIN_NODE $DARWIN_PART $((DARWIN_FIRST * 8)) $((DARWIN_BLOCKS * 8)) PARTNAME=darwin $((DARWIN_BLOCKS * BS)) $((DARWIN_LUN_BLOCKS * BS)) 0 "
	[ "$facts" = "$want" ] || die "the phone's view of darwin is '$facts', not '$want' (the last 0: not mounted)"
	log "check: darwin is $DARWIN_NODE, LBA $DARWIN_FIRST..$DARWIN_LAST ($((DARWIN_BLOCKS * BS >> 30)) GiB), as set up; userdata beside it as it was"
}

blocks() { $A exec-out "dd if=$DARWIN_NODE bs=$BS skip=$1 count=$2 2>/dev/null"; }

sum() { # sum BLOCK COUNT: sha256 on the phone, its caches dropped first
	sh_phone "echo 3 > /proc/sys/vm/drop_caches; dd if=$DARWIN_NODE bs=$BS skip=$1 count=$2 2>/dev/null | sha256sum" | cut -d' ' -f1
}

# the phone's layout block is this file's (the other writes want it)
layout_is_mine() {
	local want have
	want=$($LAYOUT block - | sha256sum | cut -d' ' -f1)
	have=$(blocks $LAYOUT_BLOCK 1 | sha256sum | cut -d' ' -f1)
	[ "$want" = "$have" ]
}

apfs_size() { # apfs_size FILE|-: the container's own size (nx_block_count x nx_block_size), or nothing
	python3 -I -c '
import struct, sys
b = (sys.stdin.buffer if sys.argv[1] == "-" else open(sys.argv[1], "rb")).read(4096)
if len(b) >= 48 and b[32:36] == b"NXSB":
    bs, n = struct.unpack_from("<IQ", b, 36)
    print(bs * n if bs == 4096 else "")' "$1"
}

status() {
	local name first count size
	check
	echo "-- the layout block ($LAYOUT_BLOCK)"
	blocks $LAYOUT_BLOCK 1 | $LAYOUT show - || echo "   (not this file's layout: '$0 layout' writes it)"
	for name in root oscryptex appcryptex; do
		eval "first=\$${name^^}_FIRST count=\$${name^^}_BLOCKS"
		size=$(blocks $first 1 | apfs_size -)
		if [ -z "$size" ]; then size="no APFS container"
		elif [ "$size" = $((count * BS)) ]; then size="an APFS container of $size bytes, filling it"
		else size="an APFS container of $size bytes, NOT its size"; fi
		echo "-- $name ($first + $count blocks): $size"
	done
	case "$(blocks $VAR_FIRST 1 | dd bs=1 skip=1024 count=2 2>/dev/null | tr -d '\0')" in
	H+|HX) echo "-- var ($VAR_FIRST + $VAR_BLOCKS blocks): an HFS Plus volume (/private/var)" ;;
	*) echo "-- var ($VAR_FIRST + $VAR_BLOCKS blocks): no HFS Plus volume" ;;
	esac
	for first in $VARSTORE_FIRST $((VARSTORE_FIRST + VARSTORE_BLOCKS / 2)); do
		echo "-- var_store slot at $first: $( [ "$(blocks $first 1 | head -c 6 | tr -d '\0')" = RDWVAR ] && echo 'an archive (RDWVAR)' || echo 'empty or no archive')"
	done
	echo "-- stage header ($STAGEHEAD_FIRST): $( [ "$(blocks $STAGEHEAD_FIRST 1 | head -c 8 | tr -d '\0')" = XNUFLASH ] && echo 'a boot image is STAGED' || echo 'nothing staged')"
}

dump_gpt() {
	local dir=$1 d n
	[ -n "$dir" ] || exit 2
	mkdir -p "$dir"
	[ "$($A get-state 2>/dev/null | tr -d '\r')" = recovery ] || die "the phone ($SERIAL) is not in the recovery"
	for d in $DARWIN_DISK $BOOT_DISK; do
		n=$(sh_phone "blockdev --getsize64 /dev/block/$d")
		$A exec-out "dd if=/dev/block/$d bs=$BS count=6 2>/dev/null" > "$dir/gpt-$d.head.bin"
		$A exec-out "dd if=/dev/block/$d bs=$BS skip=$((n / BS - 6)) count=6 2>/dev/null" > "$dir/gpt-$d.tail.bin"
		$LAYOUT gpt-tail $d "$dir/gpt-$d.head.bin" "$dir/gpt-$d.tail.bin" || die "$d: its GPT is not the one darwin-layout.py knows"
	done
	(cd "$dir" && sha256sum gpt-*.bin) | tee -a "$LOG"
}

# the bytes go in through adb's exec-in, into the node by name after the guard; then read back
put() { # put FIRST_BLOCK (stdin: the bytes; a whole number of MiB from a MiB boundary, or BS-blocks with BS=4096)
	$A exec-in "$GUARD && dd of=$DARWIN_NODE bs=$2 seek=$(($1 * BS / $2)) conv=notrunc,fsync 2>/dev/null"
}

layout() {
	local tmp=$(mktemp) cur
	$LAYOUT block "$tmp" >/dev/null || die "darwin-layout.py made no block"
	[ "$DARWIN_WRITE" = yes ] || die "DARWIN_WRITE=yes is not set: would write the layout block into darwin's block $LAYOUT_BLOCK"
	check
	cur=$(blocks $LAYOUT_BLOCK 1 | sha256sum | cut -d' ' -f1)
	if [ "$cur" = "$(sha256sum < "$tmp" | cut -d' ' -f1)" ]; then log "layout: darwin has this layout already"; rm -f "$tmp"; return 0; fi
	if [ "$cur" != "$(head -c $BS /dev/zero | sha256sum | cut -d' ' -f1)" ] && [ "$LAYOUT_REPLACE" != yes ]; then
		rm -f "$tmp"; die "darwin's block $LAYOUT_BLOCK holds something else (LAYOUT_REPLACE=yes to replace it; '$0 status' shows it)"
	fi
	log "layout: writing it into block $LAYOUT_BLOCK"
	put $LAYOUT_BLOCK $BS < "$tmp" || { rm -f "$tmp"; die "the write did not finish"; }
	[ "$(sum $LAYOUT_BLOCK 1)" = "$(sha256sum < "$tmp" | cut -d' ' -f1)" ] || { rm -f "$tmp"; die "read back different"; }
	rm -f "$tmp"
	log "layout: written, read back the same"
}

write() {
	local region=$1 img=$2 first count bytes want t0 dt
	case "$region" in root|oscryptex|appcryptex) ;; *) die "no region '$region' to write (root, oscryptex, appcryptex)" ;; esac
	eval "first=\$${region^^}_FIRST count=\$${region^^}_BLOCKS"
	[ -f "$img" ] || die "no $img"
	bytes=$(stat -c %s "$img")
	[ "$bytes" = $((count * BS)) ] || die "$img is $bytes bytes; $region is exactly $((count * BS)) (a container that does not fill it moves the next one)"
	[ "$(apfs_size "$img")" = "$bytes" ] || die "$img is not an APFS container of its own size (NXSB, blocks of 4096)"
	[ $((first % 256)) = 0 ] && [ $((bytes % 1048576)) = 0 ] || die "$region is not whole MiB"
	[ $((first + count)) -le $WRITE_FIRST ] || die "$region reaches past block $WRITE_FIRST (what iOS writes)"
	[ "$DARWIN_WRITE" = yes ] || die "DARWIN_WRITE=yes is not set: would write $img ($bytes bytes) at darwin's block $first ($region)"
	check
	layout_is_mine || die "darwin's layout block is not darwin-layout.py's: '$0 layout' first"
	want=$(sha256sum "$img" | cut -d' ' -f1)
	log "write: $region: $img, $bytes bytes, sha256 $want, at block $first"
	t0=$(date +%s)
	put $first 1048576 < "$img" || die "the write did not finish"
	dt=$(( $(date +%s) - t0 + 1 ))
	[ "$(sum $first $count)" = "$want" ] || die "$region read back different from $img"
	log "write: $region done in $dt s ($((bytes / 1048576 / dt)) MB/s), read back the same"
}

# The data volume: an HFS Plus image as a sparse file, the size of the slice. What mkfs left as holes is free space
# (HFS Plus never reads it as anything until it has written it), so only the file's data is written, piece by piece.
var() {
	local img=$1 bytes pieces n=0 total=0 have want a c
	[ -f "$img" ] || die "no $img"
	bytes=$(stat -c %s "$img")
	[ "$bytes" = $((VAR_BLOCKS * BS)) ] || die "$img is $bytes bytes, not the var region's $((VAR_BLOCKS * BS))"
	case "$(dd if="$img" bs=1 skip=1024 count=2 2>/dev/null)" in H+|HX) ;; *) die "$img is not an HFS Plus volume" ;; esac
	pieces=$(python3 -I -c '
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY); end = os.fstat(fd).st_size; at = 0
while at < end:
    try: d = os.lseek(fd, at, os.SEEK_DATA)
    except OSError: break
    h = os.lseek(fd, d, os.SEEK_HOLE)
    a, b = d // 4096, (h + 4095) // 4096
    print(a, b - a); at = h
' "$img") || die "could not find the image's data"
	while read -r a c; do total=$((total + c)); done <<< "$pieces"
	[ "$DARWIN_WRITE" = yes ] || die "DARWIN_WRITE=yes is not set: would write $(wc -l <<< "$pieces") pieces, $((total * BS >> 20)) MB, of $img into darwin's blocks $VAR_FIRST..$((VAR_FIRST + VAR_BLOCKS - 1))"
	check
	layout_is_mine || die "darwin's layout block is not darwin-layout.py's: '$0 layout' first"
	case "$(blocks $VAR_FIRST 1 | dd bs=1 skip=1024 count=2 2>/dev/null | tr -d '\0')" in
	H+|HX) [ "$VAR_REINIT" = yes ] || die "the var region holds an HFS Plus volume already: /private/var. VAR_REINIT=yes erases it" ;;
	esac
	log "var: $img: $(wc -l <<< "$pieces") pieces, $((total * BS)) bytes, into darwin's blocks $VAR_FIRST..$((VAR_FIRST + VAR_BLOCKS - 1))"
	while read -r a c; do
		n=$((n + 1))
		dd if="$img" bs=$BS skip=$a count=$c 2>/dev/null | put $((VAR_FIRST + a)) $BS || die "piece $n (blocks $a+$c) did not finish"
	done <<< "$pieces"
	n=0
	while read -r a c; do
		n=$((n + 1))
		want=$(dd if="$img" bs=$BS skip=$a count=$c 2>/dev/null | sha256sum | cut -d' ' -f1)
		[ "$(sum $((VAR_FIRST + a)) $c)" = "$want" ] || die "piece $n (blocks $a+$c) read back different"
	done <<< "$pieces"
	log "var: written, every piece read back the same"
}

case $1 in
check|status|dump-gpt|sum|layout|write|var)
	[ -n "$SERIAL" ] || die "SERIAL= is not set: the phone's serial (adb devices); nothing is done to a phone not named" ;;
esac
case $1 in
check) [ $# = 1 ] || exit 2; check ;;
status) [ $# = 1 ] || exit 2; status ;;
dump-gpt) [ $# = 2 ] || exit 2; dump_gpt "$2" ;;
sum) [ $# = 3 ] || exit 2; check; sum "$2" "$3" ;;
layout) [ $# = 1 ] || exit 2; layout ;;
write) [ $# = 3 ] || exit 2; write "$2" "$3" ;;
var) [ $# = 2 ] || exit 2; var "$2" ;;
*) sed -n '2,25p' "$0"; exit 2 ;;
esac
