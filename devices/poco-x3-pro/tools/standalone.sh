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
# The pieces (env): KERNEL, LOADER (loader.efi 2.5), ROOT (the small root image built with FLASHBOOT=1: md1),
# RESIDENT (flashboot.apfs, CRYPTEXES=flash: md0), BASE (an image of this phone's made the same way, for its
# boot shim, UEFI, device tree and ramdisk: devices/poco-x3-pro/prebuilt has none of its own; e.g. the lab's
# boot-test4.img), ARGS (the kernel's arguments) or ARGS_FILE (a lab job file: its ARGS="..." line), EXTRA
# (added to them), OUT (default xnu-standalone.img). Without ARGS/ARGS_FILE: the minimal set below. Whatever
# the arguments, the build refuses loader_return_after= (the phone would restart every N seconds) and says
# what it lacks (ufs=2 root_apfs=ufs_darwin: no root from the flash). Values of keys that look secret (Wi-Fi
# passphrase or PMK, SSID, ICCID, IMSI, phone number) are not printed, nor written into the manifest.
#
# The phone: SERIAL (its serial, as adb devices shows it: required for everything but build), ADB (default adb),
# FASTBOOT (default fastboot). boot is checked by
# its GPT entry (sde49 on this phone: darwin-layout.py) and the phone's view of it before every write and again
# in the same adb shell right before dd. Every action goes into logs/standalone.log.
#
# After another HakiOS port (MIT): telegdyd/ios-vayu, branch vayu-dev, commit 546ac05ab957 (its standalone.sh, with loader 2.2).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
cd "$TOP"
export DEVICE=poco-x3-pro
LAYOUT="python3 -I $HERE/darwin-layout.py --profile ${DARWIN_PROFILE:-vayu}"     # (qemu: the test disk's numbers, for tests)
eval "$($LAYOUT sh)" || { echo "standalone: darwin-layout.py refused its own layout"; exit 1; }
SERIAL=${SERIAL:-}
A="${ADB:-adb} -s $SERIAL"
FB="${FASTBOOT:-fastboot} -s $SERIAL"
OUT=${OUT:-xnu-standalone.img}
BACKUPS=${BACKUPS:-backups/boot}
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
	for f in KERNEL LOADER ROOT RESIDENT BASE; do
		[ -n "${!f:-}" ] && [ -f "${!f}" ] || die "$f is not set to a file (${!f:-unset})"
	done
	grep -aq 'standalone: the image boots by itself' "$LOADER" || die "$LOADER is not loader 2.5 or later (no standalone start in it)"
	cmdline=$(args)
	case " $cmdline " in *" loader_return_after="*) die "the arguments have loader_return_after=: a standalone phone would restart every so often" ;; esac
	for f in ufs=2 root_apfs=ufs_darwin; do
		case " $cmdline " in *" $f "*) ;; *) say "NOTE: the arguments lack $f: the root is not taken from the flash" ;; esac
	done
	case " $cmdline " in *" usb_handover=1 "*) say "NOTE: usb_handover=1: the loader starts its USB stack before the kernel (not yet seen on this phone)" ;; esac
	case " $cmdline " in *" flash_stage=1 "*) say "NOTE: flash_stage=1: a boot image staged by iOS is written into boot at the next start" ;; esac
	tmp=$(mktemp -d)
	python3 -I xnuloader/mkpayload.py -o "$tmp/payload.bin" --standalone --platform vayu loader.efi="$LOADER" \
		kernel="$KERNEL" ramdisk="$ROOT" resident="$RESIDENT" --cmdline "$cmdline" >/dev/null || { rm -rf "$tmp"; die "mkpayload.py"; }
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

# boot is this phone's boot: its GPT entry (darwin-layout.py), and the phone's own view of it
boot_check() {
	local facts want
	recovery
	[ "$(sh_phone "blockdev --getss /dev/block/$BOOT_DISK")" = $BS ] || die "$BOOT_DISK does not have blocks of $BS"
	$A exec-out "dd if=/dev/block/$BOOT_DISK bs=$BS count=6 2>/dev/null" | $LAYOUT gpt $BOOT_DISK - >/dev/null ||
		die "the GPT of $BOOT_DISK is not the one darwin-layout.py knows"
	facts=$(sh_phone "readlink -f /dev/block/by-name/boot; cat /sys/class/block/$BOOT_DISK$BOOT_PART/start /sys/class/block/$BOOT_DISK$BOOT_PART/size; grep ^PARTNAME= /sys/class/block/$BOOT_DISK$BOOT_PART/uevent; blockdev --getsize64 $BOOT_NODE" | tr '\n' ' ')
	want="$BOOT_NODE $((BOOT_FIRST * 8)) $((BOOT_BLOCKS * 8)) PARTNAME=boot $BOOT_BYTES "
	[ "$facts" = "$want" ] || die "the phone's view of boot is '$facts', not '$want'"
}
GUARD="[ \"\$(readlink -f /dev/block/by-name/boot)\" = $BOOT_NODE ] && [ \"\$(blockdev --getsize64 $BOOT_NODE)\" = $BOOT_BYTES ]"

boot_sum() { # boot_sum BYTES: sha256 of boot's first BYTES, the phone's caches dropped first
	sh_phone "echo 3 > /proc/sys/vm/drop_caches; dd if=$BOOT_NODE bs=$BS count=$((($1 + BS - 1) / BS)) 2>/dev/null | head -c $1 | sha256sum" | cut -d' ' -f1
}

save() {
	local tmp h1 h2 out
	boot_check
	mkdir -p "$BACKUPS"
	tmp="$BACKUPS/.boot.partial"
	$A exec-out "dd if=$BOOT_NODE bs=1048576 2>/dev/null" > "$tmp" || die "boot could not be read"
	[ "$(stat -c %s "$tmp")" = "$BOOT_BYTES" ] || die "read $(stat -c %s "$tmp") bytes of boot, not $BOOT_BYTES"
	h1=$(sha "$tmp"); h2=$(boot_sum $BOOT_BYTES)
	[ "$h1" = "$h2" ] || die "the copy ($h1) is not what the phone reads ($h2)"
	out="$BACKUPS/boot-$(date +%Y%m%d-%H%M%S)-$(echo $h1 | cut -c1-16).img"
	mv "$tmp" "$out"
	SAVED=$out
	say "save: boot as it is now -> $out (sha256 $h1, the same on the phone)"
}

put_boot() { # put_boot IMAGE: into boot, read back; 0 when it reads back the same
	local img=$1 want
	want=$(sha "$img")
	$A exec-in "$GUARD && dd of=$BOOT_NODE bs=1048576 conv=notrunc,fsync 2>/dev/null" < "$img" || return 1
	[ "$(boot_sum "$(stat -c %s "$img")")" = "$want" ]
}

image_ok() { # image_ok IMAGE: a boot image this boot can take
	[ -f "$1" ] || die "no $1"
	[ "$(head -c 8 "$1")" = "ANDROID!" ] || die "$1 is not an Android boot image"
	[ "$(stat -c %s "$1")" -le "$BOOT_BYTES" ] || die "$1 is larger than boot ($BOOT_BYTES bytes)"
}

flash() {
	local img=${1:-$OUT}
	image_ok "$img"
	[ "${FLASH_BOOT:-}" = yes ] || die "FLASH_BOOT=yes is not set: would write $img into boot"
	save
	boot_check
	say "flash: $img ($(sha "$img" | cut -c1-16)) into boot; boot as it was is $SAVED"
	if put_boot "$img"; then
		say "flash: written, read back the same. Start it: adb -s $SERIAL reboot (or the power key)"
		return 0
	fi
	say "flash: boot does NOT read back as $img: putting $SAVED back"
	put_boot "$SAVED" && die "boot is as it was ($SAVED), read back the same; $img is not in it" ||
		die "boot is NEITHER: write $SAVED again (rollback) before restarting the phone"
}

rollback() {
	local saved=${1:-$(ls -1t "$BACKUPS"/boot-*.img 2>/dev/null | head -1)}
	[ -n "$saved" ] || die "no saved boot in $BACKUPS"
	image_ok "$saved"
	case "$(basename "$saved")" in
	boot-*-*-*.img) [ "$(sha "$saved" | cut -c1-16)" = "$(basename "$saved" .img | sed 's/.*-//')" ] || die "$saved is not what its name says" ;;
	esac
	[ "${FLASH_BOOT:-}" = yes ] || die "FLASH_BOOT=yes is not set: would write $saved into boot"
	boot_check
	say "rollback: $saved ($(sha "$saved" | cut -c1-16)) into boot"
	put_boot "$saved" || die "boot does NOT read back as $saved: try again before restarting the phone"
	say "rollback: written, read back the same"
}

rollback_fastboot() {
	local saved=${1:-$(ls -1t "$BACKUPS"/boot-*.img 2>/dev/null | head -1)}
	[ -n "$saved" ] || die "no saved boot in $BACKUPS"
	image_ok "$saved"
	[ "${FLASH_BOOT:-}" = yes ] || die "FLASH_BOOT=yes is not set: would write $saved into boot"
	stock_fastboot
	say "rollback-fastboot: fastboot flash boot $saved ($(sha "$saved" | cut -c1-16)); no read back is possible from here"
	$FB flash boot "$saved" 2>&1 | tr -d '\r' | tee -a $LOG
	say "rollback-fastboot: done; '$FB reboot' starts it"
}

case ${1:-} in
try|save|flash|verify|rollback|rollback-fastboot)
	[ -n "$SERIAL" ] || die "SERIAL= is not set: the phone's serial (adb devices); nothing is done to a phone not named" ;;
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
	[ "$(boot_sum "$(stat -c %s "${2:-$OUT}")")" = "$(sha "${2:-$OUT}")" ] && say "verify: boot begins with ${2:-$OUT}" ||
		die "boot does not begin with ${2:-$OUT}"
	;;
rollback) rollback "${2:-}" ;;
rollback-fastboot) rollback_fastboot "${2:-}" ;;
*) sed -n '2,34p' "$0"; exit 1 ;;
esac
