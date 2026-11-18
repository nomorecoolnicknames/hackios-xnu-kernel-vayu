#!/bin/bash
# A standalone start under QEMU, with no host and no phone (loader 2.5): the image's "standalone" and
# "resident" payload entries, darwin's layout block, and a boot image staged the way iOS stages one
# (userland flash_stage) written into "boot" by the loader -- on a test disk of 4096-byte blocks whose
# partition table and layout block devices/poco-x3-pro/tools/darwin-layout.py --profile qemu makes (the
# loader's platform qemu-virt knows that disk as its darwin_qemu and boot_qemu). The kernel is any kernel
# that boots `hello` (qemu-smoke's); hello is the root, from the resident area.
#
#   qemu/run-standalone-test.sh KERNEL [SECS]      (from the checkout's root; xnuloader/loader.efi built)
#
# env: QEMU (QEMU's prefix, which holds bin/qemu-system-aarch64 and share/qemu/edk2-*; default: the one of the
# qemu-system-aarch64 in $PATH), HELLO
# (default userland/hello). Runs, each a fresh QEMU with -no-reboot (the loader's reset ends it):
#   A  a staged image, no flash_stage=1: left alone, boot untouched, the header still there; hello runs
#   B  flash_stage=1, the header's sum wrong: not written, the header cleared, boot untouched; hello runs
#   C  flash_stage=1, a right header: boot holds the image afterwards, the header cleared; the loader resets
#   D  ufs=2 ufs_write=1 on a second core: the layout's boot-args, darwin's write window, the kernel's table
#   F  the image's own ufs_var_first= other than the layout's: said, and the layout's comes first for the kernel
#   E  the layout block with one bit changed: not used, nothing of it on the command line; hello runs
# Prints "standalone-test: PASS" when every check holds.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
K=${1:?usage: qemu/run-standalone-test.sh KERNEL [SECS]}; SECS=${2:-90}
Q=${QEMU:-$(dirname "$(dirname "$(command -v qemu-system-aarch64 || echo /usr/bin/qemu-system-aarch64)")")}
HELLO=${HELLO:-userland/hello}
L=xnuloader/loader.efi
LAYOUT=devices/poco-x3-pro/tools/darwin-layout.py
for f in "$K" "$L" "$HELLO" "$LAYOUT"; do [ -f "$f" ] || { echo "standalone-test: no $f"; exit 1; }; done
W=$(mktemp -d /tmp/standalone-test.XXXXXX); trap 'rm -rf "$W"' EXIT
mkdir -p $W/esp/EFI/BOOT logs
cp "$L" $W/esp/EFI/BOOT/BOOTAA64.EFI
eval "$(python3 -I $LAYOUT --profile qemu sh)"
fails=0
check() {       # check WHAT CONDITION...
	local what=$1; shift
	if "$@"; then echo "  ok   $what"; else echo "  FAIL $what"; fails=$((fails + 1)); fi
}
has() { grep -a -q -- "$1" "$W/serial-$run.log"; }
hasnt() { ! has "$1"; }
# blocks of the test disk
dd_get() { dd if=$W/disk.img bs=$BS skip=$1 count=$2 2>/dev/null; }
dd_put() { dd of=$W/disk.img bs=$BS seek=$1 conv=notrunc 2>/dev/null; }
same_as() { cmp -s <(dd_get "$1" "$2") "$3"; }
boot_holds() { cmp -s <(head -c "$(stat -c %s "$1")" < <(dd_get $BOOT_FIRST $BOOT_BLOCKS)) "$1"; }
head_magic() { [ "$(dd_get $((DARWIN_FIRST + STAGEHEAD_FIRST)) 1 | head -c 8 | tr -d '\0')" = XNUFLASH ]; }
hex() { printf 0x%016x "$1"; }
# (the kernel's own "Boot args:" line: the loader's console lines are wrapped by UEFI's, with escapes in them)
kernel_args_have() { grep -a "^Boot args: " "$W/serial-$run.log" | grep -q -- "$1"; }

boot() {        # boot RUN CMDLINE [QEMU ARGS...]: one start of the standalone image, until it resets or SECS
	run=$1; local cmdline=$2; shift 2
	cp "$Q/share/qemu/edk2-arm-vars.fd" $W/vars.fd
	python3 -I xnuloader/mkpayload.py -o $W/payload.bin --standalone kernel="$K" ramdisk="$HELLO" resident="$HELLO" \
		--cmdline "$cmdline" >/dev/null
	timeout "$SECS" "$Q/bin/qemu-system-aarch64" -M virt,gic-version=3 -cpu cortex-a76 -m 6G -accel tcg,tb-size=384 \
		-drive if=pflash,format=raw,readonly=on,file="$Q/share/qemu/edk2-aarch64-code.fd" \
		-drive if=pflash,format=raw,file=$W/vars.fd \
		-drive file=$W/disk.img,format=raw,if=none,id=hd \
		-device virtio-blk-pci,drive=hd,addr=0x4,logical_block_size=4096,physical_block_size=4096 \
		-drive file=fat:rw:$W/esp,format=raw,if=none,id=esp -device virtio-blk-pci,drive=esp,addr=0x5 \
		-device loader,file=$W/payload.bin,addr=0xF0000000,force-raw=on \
		-display none -serial file:$W/serial-$run.log -monitor none -no-reboot "$@" </dev/null >/dev/null 2>&1
	cp $W/serial-$run.log logs/standalone-test-$run.log
	strings $W/serial-$run.log | grep -a -E "^standalone|^staged flash|^layout|ufs: darwin|ufs: /dev/rufs_darwin|ufs: unit|Hello from user space" |
		cut -c1-200 | sed "s/^/    $run| /"
}

# the disk, an "old" boot (a pattern, to tell it from the new one) and a staged image as flash_stage leaves one
python3 -I $LAYOUT --profile qemu disk $W/disk.img >/dev/null || exit 1
python3 -I - $W <<'PY'
import os, random, sys
w = sys.argv[1]
r = random.Random(20261117)
open(w + '/old-boot.img', 'wb').write(b'ANDROID!' + b'old boot ' * 100000)
img = b'ANDROID!' + bytes(r.getrandbits(8) for _ in range(3 * 1024 * 1024 + 4321))
open(w + '/staged.img', 'wb').write(img)
PY
dd_put $BOOT_FIRST < $W/old-boot.img
dd_put $((DARWIN_FIRST + STAGE_FIRST)) < $W/staged.img
python3 -I $LAYOUT --profile qemu stagehead $W/head.bin $W/staged.img || exit 1
python3 -I - $W/head.bin $W/head-bad.bin <<'PY'
import sys
h = bytearray(open(sys.argv[1], 'rb').read()); h[24] ^= 0xff; open(sys.argv[2], 'wb').write(h)
PY
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head.bin

echo "== A: staged, without flash_stage=1"
boot A "cpus=1"
check "A: the loader says a standalone start" has "standalone: the image boots by itself"
check "A: darwin's layout taken" has "layout: darwin's block $LAYOUT_BLOCK: var $VAR_FIRST+$VAR_BLOCKS, stage $STAGE_FIRST+$STAGE_BLOCKS"
check "A: the staged image left alone" has "flash_stage=1 is not on this image's command line: left alone"
check "A: boot untouched" boot_holds $W/old-boot.img
check "A: the header still there" head_magic
check "A: the resident image copied" has "standalone: resident image"
check "A: no time limit" has "standalone: no time limit"
check "A: hello ran (root from the resident area)" has "Hello from user space on Darwin"

echo "== B: flash_stage=1, the header's sum wrong"
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-bad.bin
boot B "cpus=1 flash_stage=1"
check "B: not flashed (sum)" has "not flashed: the staged image's sum is not the header's"
check "B: boot untouched" boot_holds $W/old-boot.img
check "B: the header cleared" eval '! head_magic'
check "B: hello ran" has "Hello from user space on Darwin"

echo "== C: flash_stage=1, a right header"
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head.bin
boot C "cpus=1 flash_stage=1"
check "C: boot written and read back" has "staged flash: boot written, read back the same; restarting"
check "C: boot holds the staged image" boot_holds $W/staged.img
check "C: the rest of its last block zeros" eval 'n=$(stat -c %s $W/staged.img); r=$(( (n + BS - 1) / BS * BS )); [ "$(dd_get $BOOT_FIRST $BOOT_BLOCKS | head -c $r | tail -c $((r - n)) | tr -d "\0" | wc -c)" = 0 ]'
check "C: the header cleared" eval '! head_magic'
check "C: the kernel did not start (reset first)" hasnt "Hello from user space on Darwin"

echo "== D: ufs=2 ufs_write=1 on a second core"
boot D "cpus=1 bootcpu=1 ufs=2 ufs_write=1" -smp 2
check "D: darwin found by its identity on unit 0" has "ufs: darwin (LBA $DARWIN_FIRST..$DARWIN_LAST of unit 0): its blocks from $WRITE_FIRST on may be written"
check "D: the layout's boot-args reach the kernel" kernel_args_have "ufs_var_first=$(hex $VAR_FIRST) ufs_var_blocks=$(hex $VAR_BLOCKS) ufs_stage_first=$(hex $STAGE_FIRST) ufs_stage_blocks=$(hex $STAGE_BLOCKS) ufs_stage_head=$(hex $STAGEHEAD_FIRST) "
check "D: the kernel takes darwin's table" has "ufs: /dev/rufs_darwin takes writes, whole blocks from $WRITE_FIRST on"
check "D: hello ran" has "Hello from user space on Darwin"

echo "== F: the image's own ufs_var_first= differs from the layout"
boot F "cpus=1 bootcpu=1 ufs=2 ufs_write=1 ufs_var_first=0x999" -smp 2
check "F: the loader says so" eval 'strings $W/serial-F.log | tr -d "\n" | grep -q "differs from darwin.s layout"'
check "F: the kernel's first ufs_var_first= is the layout's" eval 'grep -a "^Boot args: " $W/serial-F.log | grep -q "ufs_var_first=$(hex $VAR_FIRST) .*ufs_var_first=0x999"'
check "F: hello ran" has "Hello from user space on Darwin"

echo "== E: the layout block with a bit changed"
python3 -I - $W/disk.img $(( (DARWIN_FIRST + LAYOUT_BLOCK) * BS + 100 )) <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(int(sys.argv[2])); b = f.read(1); f.seek(int(sys.argv[2])); f.write(bytes([b[0] ^ 1]))
PY
boot E "cpus=1 bootcpu=1 ufs=2 ufs_write=1" -smp 2
check "E: not used" has "layout: darwin's block $LAYOUT_BLOCK not used: its CRC is wrong"
check "E: nothing of it for the kernel" eval '! kernel_args_have ufs_var_first' 
check "E: hello ran" has "Hello from user space on Darwin"

[ $fails = 0 ] && echo "standalone-test: PASS (logs/standalone-test-*.log)" || { echo "standalone-test: $fails FAILED (logs/standalone-test-*.log)"; exit 1; }
