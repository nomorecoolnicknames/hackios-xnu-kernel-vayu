#!/bin/bash
# The data volume of init (userland/src/var_hfs.h) on HFS Plus, in QEMU, every boot ended hard: userland/src/hfs_test.c
# as process 1 of a tiny APFS root, the volume an HFSX image made as devices/poco-x3-pro/tools/var-volume.sh makes the
# phone's, on a writable virtio disk (virtio_blk=2, var_hfs=rvblk0). Each boot checks what the boot before wrote,
# writes its own, says so, and AFTER seconds later QEMU is killed (SIGKILL): no unmount, no sync by hand. A new
# volume each time it runs; after the last boot the volume is checked on the host (fsck.hfsplus -n), if FSCK is there.
# (The idea of a QEMU test of HFS on a virtio disk is Scion-Project's and vayu-dev's qemu/run-hfs-test.sh.)
#
# Every boot after the first must also say, from the volume's header read before the mount, that the boot before
# left it mounted (the hard end), and with SCENARIO=import the fill of a new volume is cut short and failed first:
#   boot 1  var_import=1, the import hangs and QEMU is killed in the middle of it: the volume is left unmarked;
#   boot 2  var_import=1, the import fails: emptied, filled again, still unmarked ("NOT marked");
#   boot 3  var_import=1, the import works: emptied, filled the third time, marked, its file there;
#   boot 4.. no var_import: kept as it is, the imported file still there, the header says "left it mounted".
# SCENARIO=ufs is the phone's way instead of a virtio disk of its own: the loader's block service (ufs=2 ufs_write=1
# on a second core) over the QEMU test disk of 4096-byte blocks that darwin-layout.py --profile qemu makes, the
# volume made by devices/poco-x3-pro/tools/var-volume.sh (PROFILE=qemu) and put at darwin's var, mounted from
# /dev/ufs_var with var_hfs=1 -- the kernel's /dev/ufs_var, its 4096-byte blocks and the loader's checks, as on vayu.
#
# SCENARIO=phone: the volume the phone gets -- devices/poco-x3-pro/tools/var-volume.sh, the layout's size (8 or 12
# GiB, sparse) -- on the virtio disk answering in the phone's block sizes (virtio_blk_bs=4096: native 4096, as
# /dev/ufs_var), for what only a volume of that size meets (its journal, its far end).
#
# usage: qemu/run-hfs-test.sh [BOOTS]              (default 3; SCENARIO=import: at least 4, default 5)
# env:   K       the kernel (default xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837)
#        LOADER  loader.efi (default xnuloader/loader.efi, else prebuilt/loader.efi)
#        QEMU, QEMU_EFI, QEMU_VARS   qemu-system-aarch64 and its UEFI (default: in $PATH, Arch's edk2 paths)
#        MKFS    mkfs.hfsplus (Debian/Ubuntu: hfsprogs), FSCK fsck.hfsplus (optional)
#        W       work directory (default qemu/hfs-test), SECS a boot's budget (default 300), AFTER (default 10)
#        SCENARIO  basic (default), import, ufs or phone; EXTRA  more boot-args (var_hfs_trace=1)
# Exit 0 when every boot says PASS.
#
# After other HakiOS ports (MIT): oosexgang/hakios-xnu-kernel, branch main, commit 83aceb83817c (also in Scion-Project/scion-kernel,
# branch main), and telegdyd/ios-vayu, branch vayu-dev, commit 74cfdfa7d258.
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
SCENARIO=${SCENARIO:-basic}
case $SCENARIO in basic|ufs|phone) BOOTS=${1:-3} ;; import) BOOTS=${1:-5}; [ "$BOOTS" -ge 4 ] || BOOTS=4 ;;
	*) echo "run-hfs-test: SCENARIO is basic, import, ufs or phone"; exit 1 ;; esac
W=$(mkdir -p "${W:-qemu/hfs-test}" && cd "${W:-qemu/hfs-test}" && pwd)
K=${K:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}
LOADER=${LOADER:-xnuloader/loader.efi}; [ -f "$LOADER" ] || LOADER=prebuilt/loader.efi
QEMU=${QEMU:-qemu-system-aarch64}
QEMU_EFI=${QEMU_EFI:-/usr/share/edk2/aarch64/QEMU_EFI.fd}
QEMU_VARS=${QEMU_VARS:-/usr/share/edk2/aarch64/QEMU_VARS.fd}
MKFS=${MKFS:-mkfs.hfsplus}
SECS=${SECS:-300}; AFTER=${AFTER:-10}
for f in "$K" "$LOADER" "$QEMU_EFI" "$QEMU_VARS"; do [ -f "$f" ] || { echo "run-hfs-test: no $f"; exit 1; }; done

# the program, as userland/build.sh builds init (static, libmini)
. ./xnu-env.sh
rm -rf "$W/root" && mkdir -p "$W/root/dev" "$W/root/private/var" "$W/root/sbin" "$W/root/bin" "$W/esp/EFI/BOOT" || exit 1
xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
	-Iuserland/libmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$W/root/sbin/launchd" \
	userland/src/hfs_test.c userland/libmini/mini.c userland/libmini/start.S || exit 1
cp "$W/root/sbin/launchd" "$W/root/bin/var_store"     # the fill's import (var_import=1): hfs_test.c's import_main
python3 tools/apfs/mkapfs.py -o "$W/root.apfs" Root="$W/root" >/dev/null || exit 1
cp "$LOADER" "$W/esp/EFI/BOOT/BOOTAA64.EFI"

# what boot N imports (SCENARIO=import), and so its command line
import_of() {
	[ "$SCENARIO" = import ] || return 0
	case $1 in 1) echo hang ;; 2) echo fail ;; 3) echo ok ;; esac
}

# the volume: HFSX, journaled, 4096-byte blocks, owned by root, as the phone's (var-volume.sh); 512 MB
rm -f "$W/hfs.img" "$W/disk.img"
if [ "$SCENARIO" = ufs ]; then
	# darwin's var of the QEMU test disk, the volume made by the phone's own script and put there sparse, as
	# darwin-install.sh var puts it (only what mkfs wrote)
	eval "$(python3 -I devices/poco-x3-pro/tools/darwin-layout.py --profile qemu sh)"
	python3 -I devices/poco-x3-pro/tools/darwin-layout.py --profile qemu disk "$W/disk.img" >/dev/null || exit 1
	PATH=$(dirname "$MKFS"):$PATH PROFILE=qemu bash devices/poco-x3-pro/tools/var-volume.sh "$W/hfs.img" || exit 1
	dd if="$W/hfs.img" of="$W/disk.img" bs=$BS seek=$((DARWIN_FIRST + VAR_FIRST)) conv=notrunc,sparse 2>/dev/null || exit 1
elif [ "$SCENARIO" = phone ]; then
	PATH=$(dirname "$MKFS"):$PATH bash devices/poco-x3-pro/tools/var-volume.sh "$W/hfs.img" || exit 1
else
	truncate -s 512M "$W/hfs.img" && "$MKFS" -s -J -b 4096 -U 0 -G 0 -M 755 -v Data "$W/hfs.img" >/dev/null || exit 1
fi
echo "run-hfs-test: volume $W/hfs.img made ($(du -k "$W/hfs.img" | cut -f1) KB of it written by mkfs)"

passes=0
for n in $(seq 1 "$BOOTS"); do
	log="$W/serial-$n.log"; : > "$log"
	imp=$(import_of "$n")
	if [ "$SCENARIO" = ufs ]; then
		args="cpus=1 bootcpu=1 rd=md0 ufs=2 ufs_write=1 var_hfs=1"
		disk=(-smp 2 -drive file="$W/disk.img",format=raw,if=none,id=hd
		      -device virtio-blk-pci,drive=hd,addr=0x4,logical_block_size=4096,physical_block_size=4096
		      -drive file=fat:rw:"$W/esp",format=raw,if=none,id=esp -device virtio-blk-pci,drive=esp,addr=0x5)
	else
		args="cpus=1 rd=md0 virtio_blk=2 var_hfs=rvblk0"
		[ "$SCENARIO" = phone ] && args="$args virtio_blk_bs=4096"
		disk=(-smp 1 -drive file=fat:rw:"$W/esp",format=raw,if=virtio
		      -global virtio-mmio.force-legacy=false -drive file="$W/hfs.img",format=raw,if=none,id=hd
		      -device virtio-blk-device,drive=hd)
	fi
	xnuloader/mkpayload.py -o "$W/payload.bin" kernel="$K" ramdisk="$W/root.apfs" \
		--cmdline "$args${imp:+ var_import=1 hfs_test_import=$imp}${EXTRA:+ $EXTRA}" >/dev/null || exit 1
	cp "$QEMU_VARS" "$W/vars.fd"
	"$QEMU" -M virt,gic-version=3 -cpu cortex-a76 -m 4G -accel tcg,tb-size=384 \
		-drive if=pflash,format=raw,readonly=on,file="$QEMU_EFI" -drive if=pflash,format=raw,file="$W/vars.fd" \
		"${disk[@]}" \
		-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
		-display none -serial file:"$log" -monitor none -no-reboot </dev/null >/dev/null 2>&1 &
	qemu=$!
	waited=0
	done_re="hfs_test: boot .*: \(PASS\|FAIL\)"
	[ "$imp" = hang ] && done_re="hfs_import: hanging"
	while [ $waited -lt "$SECS" ] && kill -0 $qemu 2>/dev/null && ! grep -aq "$done_re" "$log"; do
		sleep 1; waited=$((waited + 1))
	done
	if grep -aq "$done_re" "$log"; then sleep "$AFTER"; fi
	kill -9 $qemu 2>/dev/null; wait $qemu 2>/dev/null
	echo "== boot $n${imp:+ (import: $imp)} (QEMU killed $AFTER s after the boot said it had written; $waited s to get there)"
	strings "$log" | grep -a "hfs_test\|hfs_import\|hfs:\|jnl:\|virtio-blk\|panic(\|the volume\|the journal\|HFS Plus on\|the storage during\|read-only it\|the loader's\|hfs trace\|ufs: /dev/ufs_var\|ufs: .*REFUSED\|DIFFERENT\|ufs: a " | cut -c1-200
	# what this boot must have said, besides PASS
	want=()
	[ "$n" -gt 1 ] && want+=("the last boot left it mounted")
	case $SCENARIO:$n in
	import:1) want=("hfs_import: hanging" "new: it is filled now, once") ;;
	import:2) want+=("emptied and filled again" "fill 2 of at most 3" "NOT marked") ;;
	import:3) want+=("emptied and filled again" "fill 3 of at most 3" "filled with the skeleton and var_store's newest save") ;;
	import:*) want+=("as the last boot left it" "ok: what the fill imported, still there") ;;
	esac
	ok=1
	if [ "$imp" != hang ] && ! grep -aq "hfs_test: boot [0-9]*: PASS" "$log"; then ok=0; fi
	for w in "${want[@]}"; do
		grep -aqF "$w" "$log" || { echo "run-hfs-test: boot $n did not say: $w"; ok=0; }
	done
	[ $ok = 1 ] && passes=$((passes + 1))
done
if [ "$SCENARIO" = ufs ]; then
	dd if="$W/disk.img" of="$W/hfs.img" bs=$BS skip=$((DARWIN_FIRST + VAR_FIRST)) count=$VAR_BLOCKS 2>/dev/null
fi
if [ -n "${FSCK:-}" ]; then
	echo "== fsck of the volume after the last hard end (the journal not replayed by fsck: it may say so)"
	"$FSCK" -n -f "$W/hfs.img" 2>&1 | tail -12
fi
if [ $passes -eq "$BOOTS" ]; then
	echo "run-hfs-test: ALL $BOOTS BOOTS PASS"
else
	echo "run-hfs-test: $passes of $BOOTS boots PASS"; exit 1
fi
