#!/bin/bash
# The data volume of init (userland/src/var_hfs.h) on HFS Plus, in QEMU, every boot ended hard: userland/src/hfs_test.c
# as process 1 of a tiny APFS root, the volume an HFSX image made as devices/poco-x3-pro/tools/var-volume.sh makes the
# phone's, on a writable virtio disk (virtio_blk=2, var_hfs=rvblk0). Each boot checks what the boot before wrote,
# writes its own, says so, and AFTER seconds later QEMU is killed (SIGKILL): no unmount, no sync by hand. A new
# volume each time it runs; after the last boot the volume is checked on the host (fsck.hfsplus -n), if FSCK is there.
# (The idea of a QEMU test of HFS on a virtio disk is Scion-Project's and vayu-dev's qemu/run-hfs-test.sh.)
#
# usage: qemu/run-hfs-test.sh [BOOTS]              (default 3)
# env:   K       the kernel (default xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837)
#        LOADER  loader.efi (default xnuloader/loader.efi, else prebuilt/loader.efi)
#        QEMU, QEMU_EFI, QEMU_VARS   qemu-system-aarch64 and its UEFI (default: in $PATH, Arch's edk2 paths)
#        MKFS    mkfs.hfsplus (Debian/Ubuntu: hfsprogs), FSCK fsck.hfsplus (optional)
#        W       work directory (default qemu/hfs-test), SECS a boot's budget (default 300), AFTER (default 10)
# Exit 0 when every boot says PASS.
#
# After other HakiOS ports (MIT): oosexgang/hakios-xnu-kernel, branch main, commit 83aceb83817c (also in Scion-Project/scion-kernel,
# branch main), and telegdyd/ios-vayu, branch vayu-dev, commit 74cfdfa7d258.
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
BOOTS=${1:-3}
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
rm -rf "$W/root" && mkdir -p "$W/root/dev" "$W/root/private/var" "$W/root/sbin" "$W/esp/EFI/BOOT" || exit 1
xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
	-Iuserland/libmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$W/root/sbin/launchd" \
	userland/src/hfs_test.c userland/libmini/mini.c userland/libmini/start.S || exit 1
python3 tools/apfs/mkapfs.py -o "$W/root.apfs" Root="$W/root" >/dev/null || exit 1
xnuloader/mkpayload.py -o "$W/payload.bin" kernel="$K" ramdisk="$W/root.apfs" \
	--cmdline "cpus=1 rd=md0 virtio_blk=2 var_hfs=rvblk0" >/dev/null || exit 1
cp "$LOADER" "$W/esp/EFI/BOOT/BOOTAA64.EFI"

# the volume: HFSX, journaled, 4096-byte blocks, owned by root, as the phone's (var-volume.sh); 512 MB
rm -f "$W/hfs.img"
truncate -s 512M "$W/hfs.img" && "$MKFS" -s -J -b 4096 -U 0 -G 0 -M 755 -v Data "$W/hfs.img" >/dev/null || exit 1
echo "run-hfs-test: volume $W/hfs.img made ($(du -k "$W/hfs.img" | cut -f1) KB of it written by mkfs)"

passes=0
for n in $(seq 1 "$BOOTS"); do
	log="$W/serial-$n.log"; : > "$log"
	cp "$QEMU_VARS" "$W/vars.fd"
	"$QEMU" -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 4G -accel tcg,tb-size=384 \
		-drive if=pflash,format=raw,readonly=on,file="$QEMU_EFI" -drive if=pflash,format=raw,file="$W/vars.fd" \
		-drive file=fat:rw:"$W/esp",format=raw,if=virtio \
		-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
		-global virtio-mmio.force-legacy=false -drive file="$W/hfs.img",format=raw,if=none,id=hd \
		-device virtio-blk-device,drive=hd \
		-display none -serial file:"$log" -monitor none -no-reboot </dev/null >/dev/null 2>&1 &
	qemu=$!
	waited=0
	while [ $waited -lt "$SECS" ] && kill -0 $qemu 2>/dev/null && ! grep -aq "hfs_test: boot .*: \(PASS\|FAIL\)" "$log"; do
		sleep 1; waited=$((waited + 1))
	done
	if grep -aq "hfs_test: boot .*: \(PASS\|FAIL\)" "$log"; then sleep "$AFTER"; fi
	kill -9 $qemu 2>/dev/null; wait $qemu 2>/dev/null
	echo "== boot $n (QEMU killed $AFTER s after the boot said it had written; $waited s to get there)"
	grep -a "hfs_test\|^hfs:\|^jnl:\|virtio-blk\|panic(" "$log" | cut -c1-200
	grep -aq "hfs_test: boot $n: PASS" "$log" && passes=$((passes + 1))
done
if [ -n "${FSCK:-}" ]; then
	echo "== fsck of the volume after the last hard end (the journal not replayed by fsck: it may say so)"
	"$FSCK" -n -f "$W/hfs.img" 2>&1 | tail -12
fi
if [ $passes -eq "$BOOTS" ]; then
	echo "run-hfs-test: ALL $BOOTS BOOTS PASS"
else
	echo "run-hfs-test: $passes of $BOOTS boots PASS"; exit 1
fi
