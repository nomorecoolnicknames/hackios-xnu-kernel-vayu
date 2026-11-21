#!/bin/bash
# vayu with the standalone image in boot (loader 2.6): from the computer the phone is plugged into, out of iOS into
# the recovery or the stock fastboot and back into iOS, with nobody at the phone.
#
#   phone-mode.sh state         where the phone is: ios (its command port answers), recovery, android, fastboot (the
#                               stock one), or none of them
#   phone-mode.sh ping          iOS's command port answers                                         (writes nothing)
#   phone-mode.sh bcb           misc's command, and what is past it (the status and recovery fields: RescueParty's
#                               "--prompt_and_wipe_data" is said loudly): from the recovery (adb, read only) or from
#                               iOS (the command port)
#   phone-mode.sh bcb-set       misc's command set to "boot-recovery" and nothing else: from the recovery only if it
#                               is empty (its 32 bytes alone, read back; MISC_WRITE=yes), from iOS by the command port
#                               ("bcb set", bcb_reboot=1). The phone is not restarted: a RAM test (fastboot boot) after
#                               it comes back to the recovery.
#   phone-mode.sh recovery      iOS -> the recovery: "recovery" to iOS's command port -- the kernel has misc's command
#                               set to "boot-recovery" (the loader writes it and reads it back), the run ends the
#                               orderly way and the phone restarts; ABL then starts the recovery. Waits for adb there.
#   phone-mode.sh fastboot      -> the stock fastboot: from iOS through the recovery, from the recovery or Android by
#                               "adb reboot bootloader" (the reboot reason in the PMIC, which ABL takes before misc).
#                               Waits for it (getvar product: vayu).
#   phone-mode.sh ios           from the recovery: misc's command cleared -- only if it is "boot-recovery" (or already
#                               empty: nothing written), its 32 bytes alone, read back -- then adb reboot, and waits for
#                               iOS's command port (or Android, while boot is still Android's). MISC_WRITE=yes
#   phone-mode.sh android       -> Android in boot (the multiboot: HakiOS in the recovery partition): from iOS
#                               "android" to its command port (misc's command cleared by the kernel through the
#                               loader, read back; the run ends the orderly way, ABL starts boot); from the recovery
#                               as "ios" does (MISC_WRITE=yes). Waits for adb in Android.
#
# Without the recovery, from a running iOS over the kernel's USB network (no adb, no SERIAL needed):
#   phone-mode.sh log OUT [SECS]    the kernel's log, live (usbnet_log=1: TCP 5140), onto the screen and into OUT,
#                               for SECS (default: until ^C), connecting again after each restart
#   phone-mode.sh sh PROGRAM|BUILTIN ARGS...
#                               a command on the phone through sweetd (sweetd=1, an image built with SWEETD=1:
#                               TCP 5143), its output and status: a diagnostic channel for the bench, e.g.
#                               "sh sysctl kern.bootargs", "sh /bin/launchctl list", "sh sha256 /dev/rufs_boot"
#   phone-mode.sh put LOCAL REMOTE [MODE]   a file onto the phone through sweetd
#   phone-mode.sh touch ARGS...     a tap, swipe or button through /redwood/bin/touch (the IOHID path backboardd
#                               reads, /dev/hakios-touch): "touch tap X Y", "touch button power", ... (src/touch.c)
#   phone-mode.sh stage show|cancel what darwin's staging area holds (userland flash_stage) / its header cleared
#   phone-mode.sh deploy IMAGE      IMAGE into boot from iOS: boot as it is now saved first ($BACKUPS, read twice by
#                               sha256 -- the newest there is what "standalone.sh rollback" puts back), IMAGE onto the
#                               phone, staged in darwin against the present boot (flash_stage stage), the phone
#                               restarted by the command port; the loader (flash_stage=1 in the image now in boot)
#                               writes boot before anything else and restarts; then boot read back against IMAGE.
#                               A stage that is given up on is cancelled. FLASH_BOOT=yes. (A copy of IMAGE goes
#                               into $BACKUPS/deployed.) IMAGE must carry
#                               usbnet_cmd=1 bcb_reboot=1 (the way back out of it), else DEPLOY_BLIND=yes -- its
#                               command line as the loader reads it, out of the gzipped kernel (image-args.py).
#                               The multiboot: an image running from the recovery partition says
#                               flash_stage_target=recovery, and so must IMAGE -- then it all goes into the recovery
#                               (FLASH_RECOVERY=yes, BACKUPS default backups/recovery) and boot (Android) is never
#                               written; an IMAGE whose target is not the running image's is refused.
#
# While misc says "boot-recovery", every start the phone makes by itself ends in the recovery: after a run from RAM
# (fastboot boot) as well, so the host's run scripts work as before the image was flashed. "ios"
# is the way back to iOS from boot.
#
# What it needs: on the phone, the image's kernel arguments usb_handover=1 usbnet_static=1 usbnet_cmd=1 bcb_reboot=1
# (the kernel's USB network, 10.11.0.2/24, and its command port, TCP 5141 on that interface alone); on this computer,
# 10.11.0.1/24 on the phone's USB network interface (cdc_ether, enx02a0b0c0d001: its MAC is the phone's to give) --
# set up once, by root (a NetworkManager profile for that interface, or the same by hand with ip).
#
# For deploy also, on the phone: flash_stage=1 ufs_write=1 sweetd=1 (and the layout's staging area: loader 2.6+),
# /redwood/bin/flash_stage, the token userland/sweetd.token here (SWEETD_TOKEN: another path).
#
# env: SERIAL (the phone's serial, as adb shows it: required for state, bcb from the recovery, recovery, fastboot,
# ios), IP (10.11.0.2), PORT (5141), ADB (adb), FASTBOOT (fastboot), WAIT (seconds to wait for each state, 300),
# BACKUPS (backups/boot). Every action goes into logs/phone-mode.log.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
cd "$TOP"
LAYOUT="python3 -I $HERE/darwin-layout.py --profile ${DARWIN_PROFILE:-vayu}"
eval "$($LAYOUT sh)" || { echo "phone-mode: darwin-layout.py refused its own layout"; exit 1; }
SERIAL=${SERIAL:-}
IP=${IP:-10.11.0.2}
PORT=${PORT:-5141}
WAIT=${WAIT:-300}
BACKUPS_SET=${BACKUPS:-}
BACKUPS=${BACKUPS:-backups/boot}
A="${ADB:-adb} -s $SERIAL"
FB="${FASTBOOT:-fastboot} -s $SERIAL"
LOG=${PHONE_MODE_LOG:-logs/phone-mode.log}
mkdir -p logs
say() { echo "phone-mode: $*"; echo "$(date '+%F %T') $*" >> $LOG; }
die() { say "REFUSED: $*"; exit 1; }

# one line to iOS's command port, its one line back (nothing: no answer in 15 s)
port() { timeout 15 bash -c 'exec 3<>/dev/tcp/$0/$1 && printf "%s\n" "$2" >&3 && head -n 1 <&3' "$IP" "$PORT" "$1" 2>/dev/null; }

state() {
	local s
	[ "$(port ping)" = pong ] && { echo ios; return; }
	s=$(timeout 5 $A get-state 2>/dev/null | tr -d '\r')
	case "$s" in
	recovery) echo recovery; return ;;
	device) echo android; return ;;
	esac
	if timeout 5 ${FASTBOOT:-fastboot} devices 2>/dev/null | grep -q "^$SERIAL[[:space:]]"; then
		timeout 5 $FB getvar product 2>&1 | grep -q 'product: vayu' && { echo fastboot; return; }
	fi
	echo none
}

wait_for() { # wait_for STATE...: until state says one of them, WAIT seconds at most
	local t0=$SECONDS s= w
	while [ $((SECONDS - t0)) -lt "$WAIT" ]; do
		s=$(state)
		for w in "$@"; do
			[ "$s" = "$w" ] && { say "the phone is in $s ($((SECONDS - t0)) s)"; return 0; }
		done
		sleep 3
	done
	die "the phone did not come to $* in $WAIT s (last seen: $s)"
}

# misc is this phone's: its GPT entry (darwin-layout.py) and the recovery's view of it
MISC_GUARD="[ \"\$(readlink -f /dev/block/by-name/misc)\" = $MISC_NODE ] && [ \"\$(blockdev --getsize64 $MISC_NODE)\" = $MISC_BYTES ]"
misc_check() {
	local facts want
	[ "$(timeout 5 $A get-state 2>/dev/null | tr -d '\r')" = recovery ] || die "the phone $SERIAL is not in the recovery"
	[ "$($A shell id -u | tr -d '\r')" = 0 ] || die "adb is not root"
	facts=$($A shell "readlink -f /dev/block/by-name/misc; cat /sys/class/block/$MISC_DISK$MISC_PART/start; grep ^PARTNAME= /sys/class/block/$MISC_DISK$MISC_PART/uevent; blockdev --getsize64 $MISC_NODE" | tr -d '\r' | tr '\n' ' ')
	want="$MISC_NODE $((MISC_FIRST * 8)) PARTNAME=misc $MISC_BYTES "
	[ "$facts" = "$want" ] || die "the recovery's view of misc is '$facts', not '$want'"
}
misc_block() { $A exec-out "echo 3 > /proc/sys/vm/drop_caches; dd if=$MISC_NODE bs=$BS count=1 2>/dev/null"; }
command_of() { head -c 32 "$1" | tr -d '\0'; }

show_bcb() {
	local tmp
	if [ "$(state)" = ios ]; then
		say "iOS says: $(port bcb)"
		return
	fi
	misc_check
	tmp=$(mktemp)
	misc_block > "$tmp"
	[ "$(stat -c %s "$tmp")" = "$BS" ] || { rm -f "$tmp"; die "misc's first block could not be read"; }
	say "misc's command: \"$(command_of "$tmp" | tr -c '[:print:]' '.')\""
	say "misc past it: status \"$(tail -c +33 "$tmp" | head -c 32 | tr -d '\0' | tr '\n' '|' | tr -c '[:print:]' '.')\", recovery \"$(tail -c +65 "$tmp" | head -c 768 | tr -d '\0' | tr '\n' '|' | tr -c '[:print:]' '.' | head -c 200)\"; $(tail -c +33 "$tmp" | tr -d '\0' | wc -c) bytes past the command not zero"
	if tail -c +65 "$tmp" | head -c 768 | grep -a -q wipe_data; then
		say "WARNING: the recovery field asks for userdata to be wiped (RescueParty): no TWRP-like recovery may start before it is looked at"
	fi
	rm -f "$tmp"
}

bcb_set() {
	local r
	if [ "$(state)" = ios ]; then
		r=$(port "bcb set"); say "iOS: ${r:-no answer}"
		case "$r" in "bcb: misc's command says boot-recovery"*) return 0 ;; esac
		die "iOS did not set misc's command (bcb_reboot=1?)"
	fi
	misc_write set
}

to_android() {
	local s r
	s=$(state)
	case "$s" in
	android) say "the phone is in Android already"; return 0 ;;
	recovery) misc_write clear; say "adb reboot: boot (Android, in the multiboot)"; $A reboot; wait_for android; return ;;
	ios) ;;
	*) die "the phone is $s: from there only adb or the buttons" ;;
	esac
	r=$(port android)
	say "iOS: ${r:-no answer}"
	case "$r" in
	"android: misc's command is clear"*) ;;
	*) die "iOS did not clear misc's command: nothing done (bcb_reboot=1, and a kernel with the command port's \"android\"?)" ;;
	esac
	wait_for android
}

to_recovery() {
	local s r
	s=$(state)
	case "$s" in
	recovery) say "the phone is in the recovery already"; return 0 ;;
	android) say "Android: adb reboot recovery"; $A reboot recovery; wait_for recovery; return ;;
	fastboot) say "stock fastboot: fastboot reboot-recovery (ABL writes misc's \"boot-recovery\" itself)"
		$FB reboot-recovery; wait_for recovery; return ;;
	ios) ;;
	*) die "the phone is $s: from there only the buttons" ;;
	esac
	r=$(port recovery)
	say "iOS: ${r:-no answer}"
	case "$r" in
	"recovery: misc says boot-recovery"*) ;;
	*) die "iOS did not set misc's command: nothing done (are bcb_reboot=1 and loader 2.6 in the image?)" ;;
	esac
	wait_for recovery
}

to_fastboot() {
	local s
	s=$(state)
	case "$s" in
	fastboot) say "the phone is in the stock fastboot already"; return 0 ;;
	ios) to_recovery || exit 1 ;;
	recovery|android) ;;
	*) die "the phone is $s" ;;
	esac
	say "adb reboot bootloader"
	$A reboot bootloader
	wait_for fastboot
}

# from the recovery: misc's command set (set) or cleared (clear), its 32 bytes alone, only from empty / from
# "boot-recovery" (or nothing written when it says that already), read back with the rest of the block as it was
misc_write() {
	local want=$1 before after cmd
	misc_check
	before=$(mktemp); after=$(mktemp)
	misc_block > "$before"
	[ "$(stat -c %s "$before")" = "$BS" ] || { rm -f "$before" "$after"; die "misc's first block could not be read"; }
	cmd=$(command_of "$before")
	case "$want:$cmd" in
	clear:|set:boot-recovery) say "misc's command is \"$cmd\" already: nothing written" ;;
	clear:boot-recovery|set:)
		[ "$(head -c 32 "$before" | tail -c 19 | tr -d '\0' | wc -c)" = 0 ] || { rm -f "$before" "$after"; die "misc's command is boot-recovery and more: left alone"; }
		[ "${MISC_WRITE:-}" = yes ] || { rm -f "$before" "$after"; die "MISC_WRITE=yes is not set: would $want misc's command (32 bytes)"; }
		say "misc's command: $want (\"$cmd\" now: its 32 bytes, nothing else)"
		if [ "$want" = clear ]; then
			$A shell "$MISC_GUARD && dd if=/dev/zero of=$MISC_NODE bs=32 count=1 conv=notrunc,fsync 2>/dev/null" ||
				{ rm -f "$before" "$after"; die "the write did not go"; }
		else
			# (13 bytes: the 19 after them are zeros already -- the command was empty -- and the read back checks all 32)
			$A shell "$MISC_GUARD && printf boot-recovery | dd of=$MISC_NODE bs=13 count=1 conv=notrunc,fsync 2>/dev/null" ||
				{ rm -f "$before" "$after"; die "the write did not go"; }
		fi
		misc_block > "$after"
		if ! cmp -s <(head -c 32 "$after") <(if [ "$want" = clear ]; then head -c 32 /dev/zero; else printf boot-recovery; head -c 19 /dev/zero; fi) ||
		   ! cmp -s <(tail -c +33 "$after") <(tail -c +33 "$before"); then
			rm -f "$before" "$after"
			die "misc does NOT read back as wanted: look at it before the phone restarts ('$0 bcb')"
		fi
		say "misc's command $([ "$want" = clear ] && echo cleared || echo 'set to "boot-recovery"'), read back; the rest of its block as it was"
		;;
	*) rm -f "$before" "$after"; die "misc's command is \"$cmd\", not ours: left alone" ;;
	esac
	rm -f "$before" "$after"
}

to_ios() {
	local s
	s=$(state)
	[ "$s" = ios ] && { say "iOS is up already"; return 0; }
	[ "$s" = recovery ] || die "the phone is $s, not in the recovery (fastboot: 'fastboot reboot' goes there while misc says boot-recovery)"
	misc_write clear
	say "adb reboot: whatever boot holds (iOS with the standalone image; Android while boot is Android's)"
	$A reboot
	wait_for ios android
}

# ---- from a running iOS, over the kernel's USB network ----
sw() { IP=$IP python3 -I "$HERE/sweet-sh.py" "$@"; }
FLASH_STAGE=/redwood/bin/flash_stage
REMOTE_IMG=/private/var/tmp/hakios-deploy.img
sha() { sha256sum "$1" | cut -d' ' -f1; }
# what a deploy writes: boot, or the recovery (an image running from there says flash_stage_target=recovery)
TARGET=boot T_BYTES=$BOOT_BYTES
# the sum of the target's first BYTES as the phone reads them (sweetd's sha256 of /dev/rufs_boot or rufs_recovery)
phone_boot_sum() { sw --timeout 120 sha256 /dev/rufs_$TARGET "$1" 2>/dev/null | sed -n 's/^\([0-9a-f]\{64\}\)  .*/\1/p'; }

stage_cancel() { # (a stage given up on: its header cleared, the image's copy gone)
	local r
	r=$(sw "$FLASH_STAGE" cancel 2>&1); say "flash_stage: ${r:-no answer}"
	sw rm "$REMOTE_IMG" >/dev/null 2>&1
	case "$r" in *"cancelled"*) return 0 ;; esac
	say "the stage's header is NOT known to be cleared: '$0 stage cancel' before anything restarts the phone"
	return 1
}

save_boot_ios() { # boot as it is now into $BACKUPS, read twice; SAVED
	local tmp h1 h2
	mkdir -p "$BACKUPS"
	tmp="$BACKUPS/.$TARGET.partial"
	sw --timeout 300 cat /dev/rufs_$TARGET > "$tmp" 2>/dev/null
	[ "$(stat -c %s "$tmp")" -gt "$T_BYTES" ] || { rm -f "$tmp"; die "$TARGET could not be read through sweetd"; }
	truncate -s "$T_BYTES" "$tmp"         # (sweet-sh.py ends the output with a newline of its own)
	h1=$(sha "$tmp"); h2=$(phone_boot_sum "$T_BYTES")
	[ -n "$h2" ] && [ "$h1" = "$h2" ] || { rm -f "$tmp"; die "the copy of $TARGET ($h1) is not what the phone reads (${h2:-nothing})"; }
	SAVED="$BACKUPS/$TARGET-$(date +%Y%m%d-%H%M%S)-$(echo "$h1" | cut -c1-16).img"
	mv "$tmp" "$SAVED"
	say "save: $TARGET as it is now -> $SAVED (sha256 $h1, the same on the phone)"
}

deploy() {
	local img=${1:-} want size out after w args running switch
	[ -n "$img" ] && [ -f "$img" ] || die "deploy IMAGE: no ${img:-IMAGE}"
	[ "$(head -c 8 "$img")" = "ANDROID!" ] || die "$img is not an Android boot image"
	if args=$(python3 -I "$HERE/image-args.py" "$img"); then
		args=" $args "
	elif [ "${DEPLOY_BLIND:-}" = yes ]; then
		say "note: $img's command line could not be read (image-args.py): DEPLOY_BLIND=yes, taken for one into boot"
		args=" "
	else
		die "$img's command line could not be read (image-args.py: not an image of mkxnuimg.py?); DEPLOY_BLIND=yes to go on blind"
	fi
	case "$args" in *" flash_stage_target=recovery "*) TARGET=recovery T_BYTES=$RECOVERY_BYTES ;; esac
	BACKUPS=${BACKUPS_SET:-backups/$TARGET}
	switch=FLASH_BOOT; [ $TARGET = recovery ] && switch=FLASH_RECOVERY
	size=$(stat -c %s "$img")
	[ "$size" -le "$T_BYTES" ] || die "$img is larger than $TARGET ($T_BYTES bytes)"
	if [ "${DEPLOY_BLIND:-}" != yes ]; then
		for w in usbnet_cmd=1 bcb_reboot=1; do
			case "$args" in *" $w "*) ;; *) die "$img does not say $w: nothing would bring the phone out of it but the buttons (DEPLOY_BLIND=yes)" ;; esac
		done
	fi
	for w in flash_stage=1 sweetd=1 usbnet_log=1; do
		case "$args" in *" $w "*) ;; *) say "note: $img does not say $w (the next deploy, or its live log, will not work from it)" ;; esac
	done
	[ "${!switch:-}" = yes ] || die "$switch=yes is not set: would write $img into $TARGET"
	[ "$(port ping)" = pong ] || die "iOS's command port does not answer at $IP:$PORT"
	out=$(sw "$FLASH_STAGE" show 2>&1); say "$out"
	case "$out" in *"staging area darwin blocks"*) ;; *) die "flash_stage show failed on the phone (sweetd=1? the layout? $FLASH_STAGE?)" ;; esac
	# where the running image's loader writes a stage (an older flash_stage, which does not say: boot)
	running=$(echo "$out" | sed -n 's/.*; target \([a-z]*\) (.*/\1/p' | head -1)
	[ "${running:-boot}" = "$TARGET" ] || die "the image running on the phone stages into ${running:-boot}, and $img says $TARGET (flash_stage_target): refused -- an image for the other partition"
	want=$(sha "$img")
	save_boot_ios
	mkdir -p "$BACKUPS/deployed"            # (a record of what went in; not what rollback takes: that is $SAVED)
	cp "$img" "$BACKUPS/deployed/deployed-$(date +%Y%m%d-%H%M%S)-$(echo "$want" | cut -c1-16).img"
	say "deploy: $img (sha256 $want, $size bytes) into $TARGET; $TARGET as it was is $SAVED"
	sw --timeout 300 --put "$img" "$REMOTE_IMG" 644 >/dev/null 2>&1
	[ "$(sw --timeout 120 sha256 "$REMOTE_IMG" 2>/dev/null | cut -c1-64)" = "$want" ] ||
		{ stage_cancel; die "the image did not reach the phone whole"; }
	out=$(sw --timeout 600 "$FLASH_STAGE" stage "$REMOTE_IMG" 2>&1); say "$out"
	echo "$out" | grep -q "^flash_stage: staged" || { stage_cancel; die "not staged: nothing will be flashed"; }
	out=$(sw --timeout 300 "$FLASH_STAGE" show 2>&1); say "$out"
	echo "$out" | grep -q "against this $TARGET" || { stage_cancel; die "the stage is not one the loader will take: cancelled"; }
	sw rm "$REMOTE_IMG" >/dev/null 2>&1
	out=$(port reboot); say "iOS: ${out:-no answer}"
	case "$out" in "reboot: the run ends"*) ;; *) stage_cancel; die "the command port did not restart the phone: the stage is cancelled" ;; esac
	# down, then (the loader writes boot, restarts) up again
	local t0=$SECONDS
	while [ "$(port ping)" = pong ]; do
		[ $((SECONDS - t0)) -lt "${DOWN_WAIT:-120}" ] || { stage_cancel; die "the phone did not restart in ${DOWN_WAIT:-120} s: the stage is cancelled"; }
		sleep 2
	done
	say "the phone went down ($((SECONDS - t0)) s); waiting for iOS (the loader writes $TARGET first)"
	t0=$SECONDS
	until [ "$(port ping)" = pong ]; do
		[ $((SECONDS - t0)) -lt "$WAIT" ] || die "iOS did not come back in $WAIT s: boot_tries= sends it to the recovery after its count; from there 'standalone.sh rollback' puts $SAVED back"
		sleep 3
	done
	say "iOS is up again ($((SECONDS - t0)) s)"
	t0=$SECONDS
	until after=$(phone_boot_sum "$size") && [ -n "$after" ]; do
		[ $((SECONDS - t0)) -lt 120 ] || die "sweetd does not answer after the restart: boot not checked (the image says sweetd=1?)"
		sleep 3
	done
	if [ "$after" = "$want" ]; then
		out=$(sw --timeout 300 "$FLASH_STAGE" show 2>&1)
		echo "$out" | grep -q "nothing staged" || { say "$out"; stage_cancel; }
		say "deploy: $TARGET holds $img (read back, sha256 $want); back to the old one: '$0 deploy $SAVED' from iOS, or 'standalone.sh --part $TARGET rollback $SAVED' from the recovery"
		return 0
	fi
	stage_cancel
	if [ "$(phone_boot_sum "$T_BYTES")" = "$(sha "$SAVED")" ]; then
		die "$TARGET is as it was ($SAVED): the loader did not write it (its log: flash_stage=1 in the image in $TARGET? 'staged flash:' lines)"
	fi
	die "$TARGET is NEITHER $img nor $SAVED: put one of them back before the phone restarts again ('$0 deploy', or 'standalone.sh --part $TARGET rollback' from the recovery)"
}

case "${1:-}" in
log) [ -n "${2:-}" ] || die "log OUT [SECS]"; IP=$IP exec python3 -I "$HERE/netlog.py" "$2" ${3:+--for "$3"} ;;
sh) shift; sw "$@"; exit ;;
put) [ $# -ge 3 ] || die "put LOCAL REMOTE [MODE]"; sw --put "$2" "$3" ${4:-}; exit ;;
touch) shift; sw /redwood/bin/touch "$@"; exit ;;
stage)
	case "${2:-}" in
	show) sw --timeout 300 "$FLASH_STAGE" show; exit ;;
	cancel) stage_cancel; exit ;;
	*) die "stage show|cancel" ;;
	esac ;;
deploy) deploy "${2:-}"; exit ;;
esac

[ -n "$SERIAL" ] || die "SERIAL is not set (the phone's serial, as adb devices shows it)"
case "${1:-}" in
state) state ;;
ping) r=$(port ping); echo "${r:-no answer}"; [ "$r" = pong ] ;;
bcb) show_bcb ;;
bcb-set) bcb_set ;;
android) to_android ;;
recovery) to_recovery ;;
fastboot) to_fastboot ;;
ios) to_ios ;;
*) sed -n '2,/^set -uo/p' "$HERE/$(basename "$0")" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
