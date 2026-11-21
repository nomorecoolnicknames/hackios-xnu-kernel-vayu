#!/bin/bash
# vayu: the phone boots iOS by itself. One Android boot image carries the UEFI, the loader (2.5), the kernel,
# the small root image (md1) and the resident boot volume (flashboot.apfs, md0); the iOS volume and the
# cryptexes are on darwin already (darwin-install.sh). The loader sees the payload's "standalone" entry: it
# copies the resident image, reads darwin's layout block, runs the kernel with no time limit and resets the
# phone when the kernel comes back or hangs -- the phone then starts the same image again.
#
#   standalone.sh build                 make $OUT from the pieces below                          (no phone)
#   standalone.sh try [IMAGE]           build (or take IMAGE as it is), then start it once from RAM: fastboot
#                                       boot (writes nothing; a staged image is written only with flash_stage=1)
#   standalone.sh save                  from the recovery: boot as it is now, into $BACKUPS,
#                                       read back by sha256                                     (writes nothing)
#   standalone.sh flash [IMAGE]         from the recovery: save first (always, a new copy), then IMAGE (default
#                                       $OUT) into boot, read back by sha256; if it does not read back the same,
#                                       the copy just saved goes back. FLASH_BOOT=yes
#   standalone.sh verify [IMAGE]        from the recovery: does boot begin with IMAGE           (writes nothing)
#   standalone.sh rollback [SAVED]      from the recovery: SAVED (default: the newest of $BACKUPS) into boot,
#                                       read back. FLASH_BOOT=yes
#   standalone.sh rollback-fastboot [SAVED]
#                                       from the phone's own fastboot (volume down + power): fastboot flash boot
#                                       SAVED -- the way back when the flashed image does not start. FLASH_BOOT=yes
#
#   standalone.sh --part recovery save|flash|verify|rollback|rollback-fastboot ...
#                                       the same with the recovery partition (sda28: darwin-layout.py) in boot's
#                                       place -- the multiboot, HakiOS in the recovery and Android in boot. Its own
#                                       safety: FLASH_RECOVERY=yes (FLASH_BOOT=yes does not do), BACKUPS default
#                                       backups/recovery. An image of this port's (its command line read by
#                                       image-args.py) that says flash_stage=1 goes into the recovery only if it says
#                                       flash_stage_target=recovery too -- else a deploy from it would stage into
#                                       Android's boot -- and one that says flash_stage_target=recovery never goes
#                                       into boot. Other images (PBRP, Android's own) are taken as they are.
#
# The pieces (env): KERNEL, LOADER (loader.efi 2.5), ROOT (the small root image built with FLASHBOOT=1: md1),
# RESIDENT (flashboot.apfs, CRYPTEXES=flash: md0), BASE (an image of this phone's made the same way, for its
# boot shim, UEFI, device tree and ramdisk: devices/poco-x3-pro/prebuilt has none of its own; e.g. the lab's
# boot-test4.img), ARGS (the kernel's arguments) or ARGS_FILE (a lab job file: its ARGS="..." line), EXTRA
# (added to them), OUT (default xnu-standalone.img). Without ARGS/ARGS_FILE: the minimal set below. Whatever
# the arguments, the build refuses loader_return_after= (the phone would restart every N seconds) and
# boot_menu_timeout=0 (boot_menu=1: the phone would wait in the menu for a key for good), and says
# what it lacks (ufs=2 root_apfs=ufs_darwin: no root from the flash). boot_menu=refind (the menu drawn by rEFInd)
# needs REFIND, the directory xnuloader/refind/build.sh made: its rEFInd, the theme and LOADER go into the payload
# as one FAT image (xnuloader/refind/mkrefindfs.sh, entry "refindfs"). Values of keys that look secret (Wi-Fi
# passphrase or PMK, SSID, ICCID, IMSI, phone number) are not printed, nor written into the manifest.
#
# The phone: SERIAL (its serial, as adb devices shows it: required for everything but build), ADB (default adb),
# FASTBOOT (default fastboot). boot is checked by
# its GPT entry (sde49 on this phone: darwin-layout.py) and the phone's view of it before every write and again
# in the same adb shell right before dd. A write is never a stream into dd (adb exec-in does not wait for it):
# the image is pushed to the phone (STAGE_DIR, default /tmp), checked there by length and sha256, written from
# that copy by one adb shell that waits and says how it ended, then read back (phone-write.sh). If the phone does
# not say how the write ended, nothing else is written -- no rollback over a writer that may still be running --
# until it says that the writer is gone. Every action goes into logs/standalone.log.
#
# After another HakiOS port (MIT): telegdyd/ios-vayu, branch vayu-dev, commit 546ac05ab957 (its standalone.sh, with loader 2.2).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
cd "$TOP"
export DEVICE=poco-x3-pro
LAYOUT="python3 -I $HERE/darwin-layout.py --profile ${DARWIN_PROFILE:-vayu}"     # (qemu: the test disk's numbers, for tests)
eval "$($LAYOUT sh)" || { echo "standalone: darwin-layout.py refused its own layout"; exit 1; }
PART=boot
if [ "${1:-}" = --part ]; then
	PART=${2:-}; shift 2
	case "$PART" in boot|recovery) ;; *) echo "standalone: --part boot|recovery"; exit 1 ;; esac
fi
# the partition the writes are for: boot, or the recovery (--part recovery: its own numbers, its own FLASH_ switch)
if [ $PART = recovery ]; then
	T_DISK=$RECOVERY_DISK T_PART=$RECOVERY_PART T_NODE=$RECOVERY_NODE T_FIRST=$RECOVERY_FIRST T_BLOCKS=$RECOVERY_BLOCKS
	T_BYTES=$RECOVERY_BYTES T_SWITCH=FLASH_RECOVERY
else
	T_DISK=$BOOT_DISK T_PART=$BOOT_PART T_NODE=$BOOT_NODE T_FIRST=$BOOT_FIRST T_BLOCKS=$BOOT_BLOCKS
	T_BYTES=$BOOT_BYTES T_SWITCH=FLASH_BOOT
fi
SERIAL=${SERIAL:-}
A="${ADB:-adb} -s $SERIAL"
FB="${FASTBOOT:-fastboot} -s $SERIAL"
OUT=${OUT:-xnu-standalone.img}
BACKUPS=${BACKUPS:-backups/$PART}
PAYLOAD_MAX=$((0x3f00000))      # the loader's max-download-size, and what the boot shim copies
# The minimal set: the lab's 1078 baseline (its job file 1078-g-k30-r29) cut down to the boot, the screen, touch,
# the GPU, power and buttons -- no radio, camera or sound -- plus the root from the flash and var_store as the
# lab keeps /private/var now (ufs_write=1 var_store=1). One variable at a time: the rest comes with ARGS/EXTRA.
MIN_ARGS="ufs=2 root_apfs=ufs_darwin ufs_write=1 var_store=1 bootcpu=7 cpus=8 vtimer_intid=19 fb_threads=6 touchscreen=1 nvt_qup=1 gpu=1 vayu_gpu=1 vayu_cmd_db=0x85f20000 battery=1 buttons=1 cpufreq=1 cpufreq_vayu=1 vayu_wled=2"
LOG=logs/standalone.log
mkdir -p logs
say() { echo "standalone: $*"; echo "$(date '+%F %T') $*" >> $LOG; }
die() { say "REFUSED: $*"; exit 1; }
redact() { sed -E 's/((^| )[A-Za-z0-9_.]*(pmk|psk|pass|ssid|iccid|imsi|msisdn|phone|number)[A-Za-z0-9_.]*=)[^ ]*/\1<hidden>/Ig'; }
sh_phone() { $A shell "$1" | tr -d '\r'; }
PW_SAY=say; . "$HERE/phone-write.sh"
sha() { sha256sum "$1" | cut -d' ' -f1; }

args() {
	local a
	if [ -n "${ARGS_FILE:-}" ]; then
		a=$(sed -n 's/^ARGS="\(.*\)"$/\1/p' "$ARGS_FILE" | tail -1)
		[ -n "$a" ] || die "no ARGS=\"...\" line in $ARGS_FILE"
	else
		a=${ARGS:-$MIN_ARGS}
	fi
	echo "$a${EXTRA:+ $EXTRA}"
}

build() {
	local f cmdline psize isize tmp
	cmdline=$(args)
	# boot_menu_timeout=0 would hold a phone with nobody at it in the menu for good (the loader takes 8 s instead)
	for f in $cmdline; do
		case $f in boot_menu_timeout=*) case ${f#*=} in ''|*[!0-9]*) die "$f: boot_menu_timeout= takes a number of seconds" ;; esac
			[ $((10#${f#*=})) -gt 0 ] || die "boot_menu_timeout=0: the phone would wait in the menu for a key for good" ;;
		esac
	done
	for f in KERNEL LOADER ROOT RESIDENT BASE; do
		[ -n "${!f:-}" ] && [ -f "${!f}" ] || die "$f is not set to a file (${!f:-unset})"
	done
	grep -aq 'standalone: the image boots by itself' "$LOADER" || die "$LOADER is not loader 2.5 or later (no standalone start in it)"
	case " $cmdline " in *" boot_menu=1 "*) grep -aq 'boot_menu: shown' "$LOADER" || die "boot_menu=1: $LOADER has no boot menu" ;; esac
	refind=
	case " $cmdline " in *" boot_menu=refind "*)
		grep -aq 'refind: rEFInd started' "$LOADER" || die "boot_menu=refind: $LOADER cannot start rEFInd"
		[ -n "${REFIND:-}" ] && [ -f "$REFIND/refind_aa64.efi" ] || die "boot_menu=refind: REFIND is not a directory of xnuloader/refind/build.sh's (${REFIND:-unset})"
		refind=1 ;;
	esac
	case " $cmdline " in *" loader_return_after="*) die "the arguments have loader_return_after=: a standalone phone would restart every so often" ;; esac
	for f in ufs=2 root_apfs=ufs_darwin; do
		case " $cmdline " in *" $f "*) ;; *) say "NOTE: the arguments lack $f: the root is not taken from the flash" ;; esac
	done
	case " $cmdline " in *" usb_handover=1 "*) say "NOTE: usb_handover=1: the loader starts its USB stack before the kernel (not yet seen on this phone)" ;; esac
	case " $cmdline " in *" flash_stage=1 "*) say "NOTE: flash_stage=1: a boot image staged by iOS is written into boot at the next start" ;; esac
	tmp=$(mktemp -d)
	local items=() t=8
	if [ -n "$refind" ]; then
		for f in $cmdline; do case $f in boot_menu_timeout=*) t=${f#*=} ;; esac; done
		[ $((10#$t)) -le 600 ] || t=600         # (the loader's limit)
		xnuloader/refind/mkrefindfs.sh "$tmp/refindfs.img" "$REFIND" "$LOADER" "$t" >/dev/null || { rm -rf "$tmp"; die "mkrefindfs.sh"; }
		items=(refindfs="$tmp/refindfs.img")
		refind="$(sha "$tmp/refindfs.img")  rEFInd $(sha "$REFIND/refind_aa64.efi" | cut -c1-16) from $REFIND, timeout $((10#$t)) s"
	fi
	python3 -I xnuloader/mkpayload.py -o "$tmp/payload.bin" --standalone --platform vayu loader.efi="$LOADER" \
		kernel="$KERNEL" ramdisk="$ROOT" resident="$RESIDENT" "${items[@]}" --cmdline "$cmdline" >/dev/null || { rm -rf "$tmp"; die "mkpayload.py"; }
	psize=$(stat -c %s "$tmp/payload.bin")
	[ "$psize" -le $PAYLOAD_MAX ] || { rm -rf "$tmp"; die "the payload is $psize bytes, more than the loader's $PAYLOAD_MAX"; }
	python3 -I mkxnuimg.py --uefi-from "$BASE" -p "$tmp/payload.bin" -o "$OUT" >/dev/null || { rm -rf "$tmp"; die "mkxnuimg.py"; }
	rm -rf "$tmp"
	isize=$(stat -c %s "$OUT")
	[ "$isize" -le "$BOOT_BYTES" ] || die "$OUT is $isize bytes, more than boot's $BOOT_BYTES"
	[ "$(head -c 8 "$OUT")" = "ANDROID!" ] || die "$OUT is not an Android boot image"
	{
		echo "image    $(sha "$OUT")  $OUT ($isize bytes; payload $psize)"
		for f in KERNEL LOADER ROOT RESIDENT BASE; do echo "$(printf %-8s $f) $(sha "${!f}")  ${!f}"; done
		[ -z "$refind" ] || echo "REFINDFS $refind"
		echo "cmdline  $(echo "$cmdline" | redact)"
	} > "$OUT.manifest"
	sed 's/^/  /' "$OUT.manifest"
	say "built $OUT: $isize bytes, sha256 $(sha "$OUT" | cut -c1-16), payload $psize bytes (manifest: $OUT.manifest)"
}

stock_fastboot() {
	$FB devices 2>/dev/null | grep -q "^$SERIAL" || die "the phone $SERIAL is not in fastboot"
	$FB getvar product 2>&1 | grep -q 'product: vayu' || die "this is not vayu's own fastboot (product is not vayu: the loader's?)"
}

recovery() {
	[ "$($A get-state 2>/dev/null | tr -d '\r')" = recovery ] || die "the phone $SERIAL is not in the recovery"
	[ "$(sh_phone "id -u")" = 0 ] || die "adb is not root"
}

# boot (or the recovery: --part) is this phone's: its GPT entry (darwin-layout.py), and the phone's own view of it
boot_check() {
	local facts want
	recovery
	[ "$(sh_phone "blockdev --getss /dev/block/$T_DISK")" = $BS ] || die "$T_DISK does not have blocks of $BS"
	$A exec-out "dd if=/dev/block/$T_DISK bs=$BS count=6 2>/dev/null" | $LAYOUT gpt $T_DISK - >/dev/null ||
		die "the GPT of $T_DISK is not the one darwin-layout.py knows"
	facts=$(sh_phone "readlink -f /dev/block/by-name/$PART; cat /sys/class/block/$T_DISK$T_PART/start /sys/class/block/$T_DISK$T_PART/size; grep ^PARTNAME= /sys/class/block/$T_DISK$T_PART/uevent; blockdev --getsize64 $T_NODE" | tr '\n' ' ')
	want="$T_NODE $((T_FIRST * 8)) $((T_BLOCKS * 8)) PARTNAME=$PART $T_BYTES "
	[ "$facts" = "$want" ] || die "the phone's view of $PART is '$facts', not '$want'"
}
GUARD="[ \"\$(readlink -f /dev/block/by-name/$PART)\" = $T_NODE ] && [ \"\$(blockdev --getsize64 $T_NODE)\" = $T_BYTES ]"

boot_sum() { # boot_sum BYTES: sha256 of the partition's first BYTES, the phone's caches dropped first
	sh_phone "echo 3 > /proc/sys/vm/drop_caches; dd if=$T_NODE bs=$BS count=$((($1 + BS - 1) / BS)) 2>/dev/null | head -c $1 | sha256sum" | cut -d' ' -f1
}

save() {
	local tmp h1 h2 out
	boot_check
	mkdir -p "$BACKUPS"
	tmp="$BACKUPS/.$PART.partial"
	$A exec-out "dd if=$T_NODE bs=1048576 2>/dev/null" > "$tmp" || die "$PART could not be read"
	[ "$(stat -c %s "$tmp")" = "$T_BYTES" ] || die "read $(stat -c %s "$tmp") bytes of $PART, not $T_BYTES"
	h1=$(sha "$tmp"); h2=$(boot_sum $T_BYTES)
	[ "$h1" = "$h2" ] || die "the copy ($h1) is not what the phone reads ($h2)"
	out="$BACKUPS/$PART-$(date +%Y%m%d-%H%M%S)-$(echo $h1 | cut -c1-16).img"
	mv "$tmp" "$out"
	SAVED=$out
	say "save: $PART as it is now -> $out (sha256 $h1, the same on the phone)"
}

put_boot() { # put_boot IMAGE: into boot or the recovery (phone-write.sh), read back once more on its own
	# 0: it begins with IMAGE.  1: it does not, and nothing is writing it now.  3: UNKNOWN, a writer may be running
	local img=$1 r
	pw_put "$img" $T_NODE "$GUARD" 4096 0; r=$?
	[ $r = 3 ] && return 3
	[ $r = 0 ] && [ "$(boot_sum "$(stat -c %s "$img")")" = "$(sha "$img")" ] && return 0
	return 1
}

unknown() { # unknown WHAT: a write whose end the phone did not report; nothing else is written
	die "$1: the phone did not say how the write ended and a writer may still be running. Do NOT restart the phone. When adb is back: 'adb -s $SERIAL shell ps -A | grep -w dd' shows no dd, then '$0 --part $PART verify ...' / '$T_SWITCH=yes $0 --part $PART rollback'"
}

image_ok() { # image_ok IMAGE: a boot image this partition can take
	local a
	[ -f "$1" ] || die "no $1"
	[ "$(head -c 8 "$1")" = "ANDROID!" ] || die "$1 is not an Android boot image"
	[ "$(stat -c %s "$1")" -le "$T_BYTES" ] || die "$1 is larger than $PART ($T_BYTES bytes)"
	# an image of this port's: where its staged updates go must be where it is (Android's boot is not HakiOS's)
	a=" $(python3 -I "$HERE/image-args.py" "$1" 2>/dev/null) " || return 0
	case "$PART:$a" in
	boot:*" flash_stage_target=recovery "*) die "$1 says flash_stage_target=recovery: it belongs in the recovery (--part recovery)" ;;
	recovery:*" flash_stage=1 "*) case "$a" in *" flash_stage_target=recovery "*) ;; *)
		die "$1 says flash_stage=1 without flash_stage_target=recovery: in the recovery, a deploy from it would stage into boot (Android's)" ;; esac ;;
	esac
	return 0
}

flash() {
	local img=${1:-$OUT} r
	image_ok "$img"
	[ "${!T_SWITCH:-}" = yes ] || die "$T_SWITCH=yes is not set: would write $img into $PART"
	save
	boot_check
	say "flash: $img ($(sha "$img" | cut -c1-16)) into $PART; $PART as it was is $SAVED"
	put_boot "$img"; r=$?
	if [ $r = 0 ]; then
		say "flash: written, read back the same. Start it: adb -s $SERIAL reboot (or the power key)"
		return 0
	fi
	[ $r = 3 ] && unknown "flash: $img into $PART"
	say "flash: $PART does NOT read back as $img (and nothing is writing it now): putting $SAVED back"
	put_boot "$SAVED"; r=$?
	[ $r = 0 ] && die "$PART is as it was ($SAVED), read back the same; $img is not in it"
	[ $r = 3 ] && unknown "flash: putting $SAVED back"
	die "$PART is NEITHER: write $SAVED again (rollback) before restarting the phone"
}

rollback() {
	local saved=${1:-$(ls -1t "$BACKUPS"/$PART-*.img 2>/dev/null | head -1)}
	[ -n "$saved" ] || die "no saved $PART in $BACKUPS"
	image_ok "$saved"
	case "$(basename "$saved")" in
	$PART-*-*-*.img) [ "$(sha "$saved" | cut -c1-16)" = "$(basename "$saved" .img | sed 's/.*-//')" ] || die "$saved is not what its name says" ;;
	esac
	[ "${!T_SWITCH:-}" = yes ] || die "$T_SWITCH=yes is not set: would write $saved into $PART"
	boot_check
	say "rollback: $saved ($(sha "$saved" | cut -c1-16)) into $PART"
	put_boot "$saved"; case $? in
	0) ;;
	3) unknown "rollback: $saved into $PART" ;;
	*) die "$PART does NOT read back as $saved: try again before restarting the phone" ;;
	esac
	say "rollback: written, read back the same"
}

rollback_fastboot() {
	local saved=${1:-$(ls -1t "$BACKUPS"/$PART-*.img 2>/dev/null | head -1)}
	[ -n "$saved" ] || die "no saved $PART in $BACKUPS"
	image_ok "$saved"
	[ "${!T_SWITCH:-}" = yes ] || die "$T_SWITCH=yes is not set: would write $saved into $PART"
	stock_fastboot
	say "rollback-fastboot: fastboot flash $PART $saved ($(sha "$saved" | cut -c1-16)); no read back is possible from here"
	$FB flash $PART "$saved" 2>&1 | tr -d '\r' | tee -a $LOG
	say "rollback-fastboot: done; '$FB reboot' starts it"
}

case ${1:-} in
try|save|flash|verify|rollback|rollback-fastboot)
	[ -n "$SERIAL" ] || die "SERIAL= is not set: the phone's serial (adb devices); nothing is done to a phone not named" ;;
esac
case "$PART:${1:-}" in
recovery:build|recovery:try) die "--part recovery: build and try are the partition's business of no one: leave --part out" ;;
esac
case ${1:-} in
build) build ;;
try)
	if [ -n "${2:-}" ]; then image_ok "$2"; OUT=$2; else build; fi
	stock_fastboot
	say "try: fastboot boot $OUT ($(sha "$OUT" | cut -c1-16); RAM only: boot is not written)"
	$FB boot "$OUT" 2>&1 | tr -d '\r' | tee -a $LOG
	;;
save) save ;;
flash) flash "${2:-}" ;;
verify)
	image_ok "${2:-$OUT}"; boot_check
	[ "$(boot_sum "$(stat -c %s "${2:-$OUT}")")" = "$(sha "${2:-$OUT}")" ] && say "verify: $PART begins with ${2:-$OUT}" ||
		die "$PART does not begin with ${2:-$OUT}"
	;;
rollback) rollback "${2:-}" ;;
rollback-fastboot) rollback_fastboot "${2:-}" ;;
*) sed -n '2,48p' "$0"; exit 1 ;;
esac
