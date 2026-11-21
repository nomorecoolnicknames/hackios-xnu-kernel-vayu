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
# (default userland/hello), INIT (process 1 for P and Q's long runs: default userland/src/init.c built here,
# with the toolchain of XNU_ENV, default ./xnu-env.sh). Runs, each a fresh QEMU with -no-reboot (the loader's reset ends it):
#   A  a staged image, no flash_stage=1: left alone, boot untouched, the header still there; hello runs
#   B  flash_stage=1, the header's sum wrong: not written, the header cleared, boot untouched; hello runs
#   K  flash_stage=1, a header of the first form (tied to no boot): left alone, boot untouched; hello runs
#   L  flash_stage=1, a right header staged against another boot (a stage left behind): not written, the header
#      cleared, boot untouched; hello runs
#   C  flash_stage=1, a right header staged against this boot: boot holds the image afterwards, the header cleared;
#      the loader resets
#   D  ufs=2 ufs_write=1 on a second core: the layout's boot-args, darwin's write window, the kernel's table
#   F  the image's own ufs_var_first= other than the layout's: said, and the layout's comes first for the kernel
#   G  bcb_reboot=1 bcb_selftest=1 (loader 2.6): misc's command "boot-recovery" written by the kernel through the
#      loader and read back, the rest of misc as it was; a block with more changed, and misc's second block, refused
#   H  bcb_selftest=1 without bcb_reboot=1: misc not offered, nothing of it written
#   I  boot_tries=3 after 2 starts that never came good: this one counted (3), the kernel told where, and the
#      count back to 0 by the kernel (boot_tries_ok=0: at once -- hello's root ends the run a moment later)
#   J  boot_tries=3 after 3 such starts: misc's command "boot-recovery" written by the loader, the count 0 again,
#      a reset before the kernel
#   M  loader_budget_reset=1 with the loader's loader_return_after=2 (the kernel's own: the last of two, 100000) and
#      loader_budget_margin=0: a kernel still out 2 s after it started is given up on -- its log, a reset, no hello
#   N  loader_budget_reset=1 without loader_return_after: off, hello runs
#   O  boot_tries=3 boot_tries_user=1 (boot_tries_ok=0 as in I): the storage alone no longer makes a start good;
#      hello's root ends the run with no SpringBoard, and the count stays 1
#   P  boot_tries_user=1 user_ready_proc=launchd user_ready_secs=3, init as the root (process 1 is "launchd"),
#      held by var_hfs=1 var_hfs_strict=1 (no HFS on the test disk: init waits for good, iOS never starts): the
#      count back to 0 once launchd has lived 3 s
#   Q  the same root, boot_tries_user=1 user_ready_deadline=20 stop_deadline=3 stop_test_hang=1: no SpringBoard, so
#      the kernel ends the run itself 20 s after the storage, past a step of the way out that never finishes;
#      the start stays counted
#   R  hb_test_stop=20, no root (rd=disk9: the kernel lives on, waiting for it), the kernel on a second core: the
#      heartbeat stops, and the loader calls the kernel hung 50 s later
#   BC0  (m3-bcb) boot_tries_clear_bcb=1 without flash_stage_target=recovery: not taken (the recovery as ever)
#   BC1..BC3  boot_tries_clear_bcb=1 flash_stage_target=recovery: the starts used up clear misc's command (Android) instead of setting it,
#      twice; then boot_tries_max_cycles=2 holds HakiOS back: straight on to Android, no kernel, the count not written
#   BC4  boot_tries_max_cycles=2 without boot_tries_clear_bcb=1: nothing held; the kernel's good start zeroes the
#      count and the trips (cycles)
#   BC5  bcb_reboot=1: misc's block in the log -- RescueParty's "--prompt_and_wipe_data" said loudly, nothing written
#   BC6  flash_stage=1 flash_stage_target=recovery, a header tied to the recovery: the recovery holds the image, boot
#      untouched
#   BC7  flash_stage_target=recovery, a header tied to boot: cleared, neither written; hello runs
#   BC8  flash_stage=1 (boot), a header tied to the recovery: cleared, neither written; hello runs
#   BC9  flash_stage_target= neither: left alone, nothing written; hello runs
#   E  the layout block with one bit changed: not used, nothing of it on the command line; hello runs
#   EFI1  boot_menu_efi=1 (L1, efi_chainload.c), a third disk as the phone's logfs (GPT of 4096-byte blocks, a
#      partition named logfs, FAT12 of logfs' geometry) holding qemu/efi/hello_ret.efi, a file named .efi that is no
#      image, and \EFI\hakios\entries.cfg: the firmware's tables said, the loader's own BOOTAA64.EFI on the ESP left
#      out, hello_ret listed with the options of entries.cfg; hello runs
#   EFI2  boot_menu_efi=1 boot_efi_run=1: hello_ret started with its options and a DeviceHandle its own file opens
#      from, Exit(EFI_ABORTED) with exit data: said, a reset (no hello); the FAT disk not written
#   EFI3  boot_efi_run=1 without boot_menu_efi=1: nothing looked for; hello runs
#   EFI4  boot_menu_efi=1 boot_efi_run=5 (there is one): said, the start goes on; hello runs
# The boot menu (boot_menu=1), keys typed on the serial console (the loader's ConIn: arrows and Enter for the
# phone's Vol+, Vol- and Power), the layout block put right again first:
#   MA boot_menu_timeout=3, Enter typed before the menu is up: dropped; HakiOS by the timer, hello runs; no menu in A
#   MB boot_menu_timeout=0: not taken by the loader (8 s), refused by standalone.sh build; hello runs
#   MC boot_menu_default=android: the cursor starts on Android; down, up, up (the list wraps), Enter on HakiOS: the
#      timer off at the first key, hello runs
#   MD boot_menu_default=android boot_menu_timeout=2 bcb_reboot=1, misc's command "boot-recovery": Android by the
#      timer, the command cleared by m3's hook (the rest of misc as it was), a reset, no hello
#   ME boot_menu_poweroff=1: Power Off asked, a volume key takes it back, asked again and confirmed: power off
#   MF boot_menu_efi=1 boot_menu_bcb=1, EFI1's logfs: its application an entry, Default Boot, Diagnostics (misc,
#      darwin's count of starts, the EFI found, the keys), Default Boot chosen, then HakiOS; nothing started
#   MH boot_menu_efi=1: the EFI entry chosen: hello_ret started with its options, a reset, the FAT disk untouched
#   MI boot_menu=1 boot_menu_efi=1 boot_efi_run=1: boot_efi_run is the menu-less probe: HakiOS by the timer, nothing
#      started
#   MG boot_menu_idle=3: a key stops the timer, 3 s with no other: HakiOS after all
# boot_menu=refind (m4: rEFInd as the menu, refind_menu.c), with ramfb (the phone's 1080x2400 of it) and REFIND= (the
# directory xnuloader/refind/build.sh made; without it these are skipped). SHOTS=DIR: PNG screenshots of RA into DIR:
#   RA three entries (rEFInd scans its own volume alone); down x3 (the column wraps to HakiOS), up (back to the
#      last), down, Enter: HakiOS by hand through the hand-off, hello runs; rEFInd's marks in the log
#   RH refind_scan=all: a fourth entry (the ESP's \EFI\BOOT\BOOTAA64.EFI); up (wraps to it), down, Enter: HakiOS
#   RB rEFInd's timer (3 s): HakiOS, hello runs
#   RC Android (down, Enter), bcb_reboot=1, misc's command "boot-recovery": cleared by the loader that started rEFInd,
#      a reset, no hello
#   RD Diagnostics (down, down, Enter): the text menu on its diagnostics page, a key back, Enter: HakiOS, hello runs
#   RE boot_menu=refind but no rEFInd in the image: the text menu, its timer, hello runs
#   RG boot_menu_idle=5: a key stops rEFInd's timer, nothing chosen: the loader resets the phone 5 s after the key
#   RI refind_test_hang=1 refind_hang=15: rEFInd stops after its "graphics" mark; no sign of life for 15 s: a reset
#   RJ refind_safe=1 refind_conf=bmp: the BMP theme, no screen read-back, fills by rows; marks of each picture; the
#      timer: HakiOS (a screenshot)
#   RK refind_conf=text: rEFInd's text menu; the timer: HakiOS
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
INIT=${INIT:-}
if [ -z "$INIT" ]; then         # (userland/build.sh's way, for init alone)
	INIT=$W/init
	( source "${XNU_ENV:-./xnu-env.sh}" >/dev/null 2>&1 && cd userland &&
		CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin" &&
		$CC -Ilibmini -c libmini/mini.c -o $W/mini.o && $CC -c libmini/start.S -o $W/start.o &&
		$CC -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o $INIT src/init.c $W/mini.o $W/start.o ) ||
		echo "standalone-test: init not built (XNU_ENV=, or INIT=): P and Q fail"
fi
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
flat_has() { strings "$W/serial-$run.log" | tr -d '\n' | grep -q -F -- "$1"; }
# misc's first block: its command (the 32 bytes, zeros dropped), and the rest of it as a sum
misc_command() { dd_get $MISC_FIRST 1 | head -c 32 | tr -d '\0'; }
misc_rest() { dd_get $MISC_FIRST 2 | tail -c +33 | sha256sum | cut -c1-16; }
# darwin's "tries" block (xnuloader/bcb.h): put COUNT STARTS RECOVERIES [CYCLES], or read them back ("none" if it is not
# one); tries_cycles: its trips since the last good start
tries_put() {
	python3 -I -c 'import struct,sys,zlib; b=bytearray(4096); struct.pack_into("<8sIIQQI",b,0,b"HKTRIES",1,*map(int,(sys.argv[1:5] + ["0"])[:4])); struct.pack_into("<I",b,4092,zlib.crc32(bytes(b[:4092]))); sys.stdout.buffer.write(b)' "$@" |
		dd_put $((DARWIN_FIRST + TRIES_FIRST))
}
tries_cycles() {
	dd_get $((DARWIN_FIRST + TRIES_FIRST)) 1 | python3 -I -c 'import struct,sys; print(struct.unpack_from("<I",sys.stdin.buffer.read(),32)[0])'
}
tries_get() {
	dd_get $((DARWIN_FIRST + TRIES_FIRST)) 1 | python3 -I -c 'import struct,sys,zlib; b=sys.stdin.buffer.read(); m,v,c,s,r=struct.unpack_from("<8sIIQQ",b,0); print(" ".join(map(str,(c,s,r))) if m==b"HKTRIES\0" and v==1 and zlib.crc32(b[:4092])==struct.unpack_from("<I",b,4092)[0] else "none")'
}

boot() {        # boot RUN CMDLINE [QEMU ARGS...]: one start of the standalone image, until it resets or SECS
	run=$1; local cmdline=$2; shift 2
	local serial=file:$W/serial-$run.log keys=
	if [ -n "${KEYS:-}" ]; then     # KEYS=: typed on the serial console, the loader's ConIn (keys.py below)
		rm -f $W/serial.sock; serial=unix:$W/serial.sock,server=on,wait=on
		python3 -I $W/keys.py $W/serial.sock $W/serial-$run.log "$KEYS" & keys=$!
	fi
	cp "$Q/share/qemu/edk2-arm-vars.fd" $W/vars.fd
	# shellcheck disable=SC2086
	python3 -I xnuloader/mkpayload.py -o $W/payload.bin --standalone kernel="$K" ramdisk="$HELLO" resident="$HELLO" \
		${PAYLOAD_EXTRA:-} --cmdline "$cmdline" >/dev/null
	timeout "$SECS" "$Q/bin/qemu-system-aarch64" -M virt,gic-version=3 -cpu cortex-a76 -m 6G -accel tcg,tb-size=384 \
		-drive if=pflash,format=raw,readonly=on,file="$Q/share/qemu/edk2-aarch64-code.fd" \
		-drive if=pflash,format=raw,file=$W/vars.fd \
		-drive file=$W/disk.img,format=raw,if=none,id=hd \
		-device virtio-blk-pci,drive=hd,addr=0x4,logical_block_size=4096,physical_block_size=4096 \
		-drive file=fat:rw:$W/esp,format=raw,if=none,id=esp -device virtio-blk-pci,drive=esp,addr=0x5 \
		-device loader,file=$W/payload.bin,addr=0xF0000000,force-raw=on \
		-display none -serial $serial -monitor "${MONITOR:-none}" -no-reboot "$@" </dev/null >/dev/null 2>&1
	[ -z "$keys" ] || wait $keys
	cp $W/serial-$run.log logs/standalone-test-$run.log
	strings $W/serial-$run.log | grep -a -E "^standalone|^staged flash|^layout|ufs: darwin|ufs: /dev/rufs_darwin|ufs: unit|ufs: misc|^bcb|^boot_tries|Hello from user space|^hb_test_stop|^loader: |THE KERNEL HUNG|user_ready|^efi: |^hello_ret|^boot_menu|^menu: |^refind: " |
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
# the header flash_stage writes (userland/src/flash_stage.h): stagehead OUT IMAGE [BOOT [TIE]]: staged against BOOT (all
# of the partition, as read; TIE FORBOOT!, or FORRECV! for the recovery); without BOOT the first form, tied to no boot
stagehead() {
	python3 -I - "$STAGE_FIRST" "$BS" "$@" <<'PY'
import struct, sys
def fnv(d):
    h = 0xcbf29ce484222325
    for c in d:
        h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return h
first, bs, out, image = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
data = open(image, 'rb').read()
h = struct.pack('<8sQQQ', b'XNUFLASH', first, len(data), fnv(data))
if len(sys.argv) > 5:
    boot = open(sys.argv[5], 'rb').read()
    h += struct.pack('<8sQQ', sys.argv[6].encode() if len(sys.argv) > 6 else b'FORBOOT!', len(boot), fnv(boot))
open(out, 'wb').write(h + bytes(bs - len(h)))
PY
}
dd_get $BOOT_FIRST $BOOT_BLOCKS > $W/boot-now.img
python3 -I -c 'import sys; b = bytearray(open(sys.argv[1], "rb").read()); b[-1] ^= 1; open(sys.argv[2], "wb").write(b)' \
	$W/boot-now.img $W/boot-other.img
stagehead $W/head.bin $W/staged.img $W/boot-now.img || exit 1
stagehead $W/head-v1.bin $W/staged.img || exit 1
stagehead $W/head-other.bin $W/staged.img $W/boot-other.img || exit 1
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

echo "== K: flash_stage=1, a header tied to no boot (the first form)"
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-v1.bin
boot K "cpus=1 flash_stage=1"
check "K: left alone" has "the staged header is not tied to a boot (an older flash_stage's): left alone"
check "K: boot untouched" boot_holds $W/old-boot.img
check "K: the header still there" head_magic
check "K: hello ran" has "Hello from user space on Darwin"

echo "== L: flash_stage=1, a right header staged against another boot"
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-other.bin
boot L "cpus=1 flash_stage=1"
check "L: not flashed (another boot)" has "the image was staged against another boot"
check "L: boot untouched" boot_holds $W/old-boot.img
check "L: the header cleared" eval '! head_magic'
check "L: hello ran" has "Hello from user space on Darwin"

echo "== C: flash_stage=1, a right header"
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head.bin
boot C "cpus=1 flash_stage=1"
check "C: staged against this boot" has "staged against this boot; the header is cleared"
check "C: its first block last" has "each piece read back, its first block last"
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

# misc: a pattern past the command in its first block and in its second, which must stay as they are
python3 -I -c 'import sys; sys.stdout.buffer.write(bytes(32) + bytes((i * 7 + 3) & 0xff for i in range(8192 - 32)))' | dd_put $MISC_FIRST
rest=$(misc_rest)

echo "== G: bcb_reboot=1 bcb_selftest=1"
boot G "cpus=1 bootcpu=1 ufs=2 ufs_write=1 bcb_reboot=1 bcb_selftest=1" -smp 2
check "G: misc found by its identity on unit 0" flat_has "ufs: misc (LBA $MISC_FIRST..$((MISC_FIRST + MISC_BLOCKS - 1)) of unit 0): its first block's command may be set"
check "G: the kernel takes the offer" has "bcb: misc's boot control block may be set"
check "G: more than the command changed: refused by the loader" flat_has "REFUSED: it changes more than the command"
check "G: ... and the kernel heard so" has "bcb selftest: a byte past the command changed: refused"
check "G: misc's second block: refused" has "bcb selftest: misc's second block: refused"
check "G: boot-recovery written, read back (the loader)" flat_has "the boot control block's command is now \"boot-recovery\""
check "G: boot-recovery written, read back (the kernel)" has "bcb: misc's command set to \"boot-recovery\": written, read back the same"
check "G: misc's command on the disk" eval '[ "$(misc_command)" = boot-recovery ]'
check "G: the rest of misc as it was" eval '[ "$(misc_rest)" = "$rest" ]'
check "G: hello ran" has "Hello from user space on Darwin"

dd if=/dev/zero bs=32 count=1 2>/dev/null | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
echo "== H: bcb_selftest=1 without bcb_reboot=1"
boot H "cpus=1 bootcpu=1 ufs=2 ufs_write=1 bcb_selftest=1" -smp 2
check "H: misc not offered" eval '! flat_has "command may be set to"'
check "H: nothing asked of it" hasnt "bcb selftest"
check "H: misc untouched" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "H: hello ran" has "Hello from user space on Darwin"

echo "== I: boot_tries=3 after 2 starts that never came good"
tries_put 2 5 0
boot I "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_ok=0" -smp 2
check "I: this start counted" flat_has "boot_tries=3: this is start 3 since the last good one (6 in all, 0 sent to the recovery)"
check "I: the kernel told where" kernel_args_have "ufs_tries_block=$(hex $TRIES_FIRST)"
check "I: the kernel calls it good" has "boot_tries: 0 s up, a good start: the count of starts in darwin's block $TRIES_FIRST is 0 again"
check "I: the count on the disk: 0, 6 starts" eval '[ "$(tries_get)" = "0 6 0" ]'
check "I: misc untouched" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "I: hello ran" has "Hello from user space on Darwin"

echo "== J: boot_tries=3 after 3 starts that never came good"
tries_put 3 9 0
boot J "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3" -smp 2
check "J: given up" flat_has "the 3 starts before this one never came good: into the recovery"
check "J: misc written by the loader" flat_has "says \"boot-recovery\", read back the same; restarting into the recovery (1 times so far)"
check "J: misc's command on the disk" eval '[ "$(misc_command)" = boot-recovery ]'
check "J: the rest of misc as it was" eval '[ "$(misc_rest)" = "$rest" ]'
check "J: the count: 0, 10 starts, 1 recovery" eval '[ "$(tries_get)" = "0 10 1" ]'
check "J: the kernel did not start (reset first)" hasnt "Hello from user space on Darwin"

echo "== M: loader_budget_reset=1, the loader's budget 2 s, no margin"
boot M "cpus=1 bootcpu=1 loader_return_after=2 loader_budget_reset=1 loader_budget_margin=0 loader_return_after=100000" -smp 2
check "M: the loader says so" flat_has "loader_budget_reset=1: a kernel still out 2 s after it started (its 2 and 0 more) is reset"
check "M: given up on" flat_has "THE KERNEL DID NOT COME BACK on core"
check "M: the kernel's log follows, its result over-budget" eval 'strings $W/serial-M.log | grep -a -A2 "THE KERNEL DID NOT COME BACK" | grep -q "^over-budget " && has "== KERNEL LOG =="'
check "M: no hello (reset first)" hasnt "Hello from user space on Darwin"

echo "== N: loader_budget_reset=1 without loader_return_after"
boot N "cpus=1 bootcpu=1 loader_budget_reset=1" -smp 2
check "N: off" eval '! flat_has "loader_budget_reset=1: a kernel still out"'
check "N: hello ran" has "Hello from user space on Darwin"
dd if=/dev/zero bs=32 count=1 2>/dev/null | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
echo "== O: boot_tries_user=1: hello's root, no SpringBoard"
tries_put 0 0 0
boot O "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_ok=0 boot_tries_user=1" -smp 2
check "O: this start counted" flat_has "boot_tries=3: this is start 1 since the last good one (1 in all, 0 sent to the recovery)"
check "O: the kernel waits for SpringBoard" has "boot_tries: this start is counted (darwin's block $TRIES_FIRST); it is a good one once SpringBoard has lived 60 s (boot_tries_user=1), no deadline"
check "O: not called good" hasnt "is 0 again"
check "O: the count on the disk: 1, 1 start" eval '[ "$(tries_get)" = "1 1 0" ]'
check "O: hello ran" has "Hello from user space on Darwin"

echo "== P: boot_tries_user=1 user_ready_proc=launchd, init held before iOS"
tries_put 0 1 0
HELLO=$INIT SECS=$((SECS > 150 ? SECS : 150)) boot P "cpus=1 bootcpu=1 ufs=2 ufs_write=1 var_hfs=1 var_hfs_strict=1 boot_tries=3 boot_tries_user=1 user_ready_proc=launchd user_ready_secs=3 user_ready_deadline=60 loader_return_after=70" -smp 2
check "P: init held (var_hfs_strict=1)" has "=== init: the data volume on the flash did not mount (var_hfs_strict=1): iOS is not started ==="
check "P: launchd seen" has "boot_tries: launchd\[1\] is up"
check "P: called good once it lived 3 s" has "boot_tries: launchd\[1\] has lived 3 s, userland is up: a good start: the count of starts in darwin's block $TRIES_FIRST is 0 again"
check "P: no deadline taken" hasnt "boot_tries: user_ready_deadline=60:"
check "P: the count on the disk: 0, 2 starts" eval '[ "$(tries_get)" = "0 2 0" ]'

echo "== Q: boot_tries_user=1 user_ready_deadline=20, no SpringBoard, a step of the way out that hangs"
HELLO=$INIT SECS=$((SECS > 150 ? SECS : 150)) boot Q "cpus=1 bootcpu=1 ufs=2 ufs_write=1 var_hfs=1 var_hfs_strict=1 boot_tries=3 boot_tries_user=1 user_ready_secs=3 user_ready_deadline=20 stop_deadline=3 stop_test_hang=1" -smp 2
check "Q: init held (var_hfs_strict=1)" has "=== init: the data volume on the flash did not mount (var_hfs_strict=1): iOS is not started ==="
check "Q: the deadline taken" has "boot_tries: user_ready_deadline=20: SpringBoard has not lived 3 s in this start (it never came up): the run ends"
check "Q: the orderly way out" has "the host asked for the end of the run: back to the loader"
check "Q: the hung step left behind" has "loader: stop_test_hang did NOT finish in 3000 ms (stop_deadline=3): left as it is, the way out goes on"
check "Q: ... and the way out went on" has "loader: the end of the run is here"
check "Q: the count on the disk: 1, 3 starts" eval '[ "$(tries_get)" = "1 3 0" ]'

echo "== R: hb_test_stop=20, no root, the kernel on a second core"
SECS=$((SECS > 200 ? SECS : 200)) boot R "cpus=1 bootcpu=1 rd=disk9 hb_test_stop=20" -smp 2
check "R: the heartbeat stopped" has "hb_test_stop=20: the heartbeat stops here"
check "R: the loader calls the kernel hung" flat_has "THE KERNEL HUNG on core"
check "R: its log handed out (QEMU: no logdump, on the console)" flat_has "== KERNEL LOG =="
check "R: no root, no hello" hasnt "Hello from user space on Darwin"

# ---- m3-bcb: the multiboot's misc and recovery ----
misc_set() { printf boot-recovery | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null; }
misc_set
echo "== BC0: boot_tries_clear_bcb=1 from an image that does not say flash_stage_target=recovery: not taken"
tries_put 3 5 0
boot BC0 "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_clear_bcb=1" -smp 2
check "BC0: not taken" flat_has "boot_tries_clear_bcb=1 without flash_stage_target=recovery"
check "BC0: into the recovery as ever" flat_has "the 3 starts before this one never came good: into the recovery"
check "BC0: misc says boot-recovery" eval '[ "$(misc_command)" = boot-recovery ] && [ "$(misc_rest)" = "$rest" ]'
check "BC0: no kernel" hasnt "Hello from user space on Darwin"
echo "== BC1: boot_tries=3 boot_tries_clear_bcb=1 after 3 starts that never came good, misc says boot-recovery"
tries_put 3 9 0
boot BC1 "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_clear_bcb=1 flash_stage_target=recovery" -smp 2
check "BC1: given up, into Android" flat_has "the 3 starts before this one never came good: into Android (boot_tries_clear_bcb=1)"
check "BC1: misc cleared by the loader" flat_has "misc's command is clear, read back the same; restarting into Android (1 times so far, 1 since the last good start)"
check "BC1: misc's command on the disk: empty" eval '[ -z "$(misc_command)" ]'
check "BC1: the rest of misc as it was" eval '[ "$(misc_rest)" = "$rest" ]'
check "BC1: the count: 0, 10 starts, 1 trip; 1 cycle" eval '[ "$(tries_get)" = "0 10 1" ] && [ "$(tries_cycles)" = 1 ]'
check "BC1: the kernel did not start (reset first)" hasnt "Hello from user space on Darwin"

misc_set        # (Android set it again as it started: the menu at every power-on)
echo "== BC2: the same once more"
tries_put 3 13 1 1
boot BC2 "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_clear_bcb=1 boot_tries_max_cycles=2 flash_stage_target=recovery" -smp 2
check "BC2: into Android again" flat_has "restarting into Android (2 times so far, 2 since the last good start)"
check "BC2: misc's command: empty" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "BC2: the count: 0, 14 starts, 2 trips; 2 cycles" eval '[ "$(tries_get)" = "0 14 2" ] && [ "$(tries_cycles)" = 2 ]'
check "BC2: no kernel" hasnt "Hello from user space on Darwin"

misc_set
echo "== BC3: boot_tries_max_cycles=2 with 2 trips since the last good start: held"
boot BC3 "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_clear_bcb=1 boot_tries_max_cycles=2 flash_stage_target=recovery" -smp 2
check "BC3: held" flat_has "boot_tries_max_cycles=2: 2 trips to Android since the last good start: HakiOS is not started by itself"
check "BC3: on to Android" flat_has "misc's command is clear, read back the same; restarting into Android"
check "BC3: misc's command: empty, the rest as it was" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "BC3: the count not written" eval '[ "$(tries_get)" = "0 14 2" ] && [ "$(tries_cycles)" = 2 ]'
check "BC3: no kernel" hasnt "Hello from user space on Darwin"

echo "== BC4: boot_tries_max_cycles=2 without boot_tries_clear_bcb=1; the kernel's good start"
boot BC4 "cpus=1 bootcpu=1 ufs=2 ufs_write=1 boot_tries=3 boot_tries_ok=0 boot_tries_max_cycles=2" -smp 2
check "BC4: not held" eval '! flat_has "HakiOS is not started by itself"'
check "BC4: counted" flat_has "boot_tries=3: this is start 1 since the last good one (15 in all, 2 sent to the recovery)"
check "BC4: the kernel calls it good" has "is 0 again"
check "BC4: count and cycles 0 (the kernel's)" eval '[ "$(tries_get)" = "0 15 2" ] && [ "$(tries_cycles)" = 0 ]'
check "BC4: misc untouched" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "BC4: hello ran" has "Hello from user space on Darwin"

echo "== BC5: bcb_reboot=1, RescueParty's wipe in misc's recovery field"
dd_get $MISC_FIRST 2 > $W/misc-keep.bin
python3 -I -c 'import sys; b = bytearray(8192); b[0:13] = b"boot-recovery"; r = b"recovery\n--prompt_and_wipe_data\n--reason=test\n"; b[64:64 + len(r)] = r; sys.stdout.buffer.write(b)' | dd_put $MISC_FIRST
dd_get $MISC_FIRST 2 | sha256sum > $W/misc-rp.sum
boot BC5 "cpus=1 bootcpu=1 bcb_reboot=1" -smp 2
check "BC5: the command and the recovery field in the log" flat_has "standalone: misc's command \"boot-recovery\", status \"\", recovery \"recovery|--prompt_and_wipe_data|--reason=test|\""
check "BC5: the wipe said loudly" flat_has "ASKS FOR A WIPE OF USERDATA"
check "BC5: nothing of misc written" eval '[ "$(dd_get $MISC_FIRST 2 | sha256sum)" = "$(cat $W/misc-rp.sum)" ]'
check "BC5: hello ran" has "Hello from user space on Darwin"
dd_put $MISC_FIRST < $W/misc-keep.bin

echo "== BC6: flash_stage=1 flash_stage_target=recovery, a header tied to the recovery"
python3 -I -c 'import sys; sys.stdout.buffer.write(b"ANDROID!" + b"old recovery " * 100000)' > $W/old-recovery.img
dd_put $RECOVERY_FIRST < $W/old-recovery.img
dd_get $RECOVERY_FIRST $RECOVERY_BLOCKS > $W/recovery-now.img
dd_get $BOOT_FIRST $BOOT_BLOCKS > $W/boot-now2.img
stagehead $W/head-rec.bin $W/staged.img $W/recovery-now.img FORRECV! || exit 1
stagehead $W/head-boot2.bin $W/staged.img $W/boot-now2.img || exit 1
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-rec.bin
boot BC6 "cpus=1 flash_stage=1 flash_stage_target=recovery"
check "BC6: into the recovery, not boot" has "staged flash: flash_stage_target=recovery: the image goes into the recovery (LBA $RECOVERY_FIRST..$RECOVERY_LAST of unit 0), boot is not written"
check "BC6: staged against this recovery" has "staged against this recovery; the header is cleared"
check "BC6: written, read back" has "staged flash: recovery written, read back the same; restarting"
check "BC6: the recovery holds the staged image" eval 'cmp -s <(dd_get $RECOVERY_FIRST $RECOVERY_BLOCKS | head -c $(stat -c %s $W/staged.img)) $W/staged.img'
check "BC6: boot untouched" same_as $BOOT_FIRST $BOOT_BLOCKS $W/boot-now2.img
check "BC6: the header cleared" eval '! head_magic'

dd_put $RECOVERY_FIRST < $W/recovery-now.img
dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-boot2.bin
echo "== BC7: flash_stage_target=recovery, a header tied to boot"
boot BC7 "cpus=1 flash_stage=1 flash_stage_target=recovery"
check "BC7: refused, cleared" has "staged flash: the image was staged for boot, and this one's flash_stage_target is recovery: not flashed, the header is cleared"
check "BC7: boot untouched" same_as $BOOT_FIRST $BOOT_BLOCKS $W/boot-now2.img
check "BC7: the recovery untouched" same_as $RECOVERY_FIRST $RECOVERY_BLOCKS $W/recovery-now.img
check "BC7: the header cleared" eval '! head_magic'
check "BC7: hello ran" has "Hello from user space on Darwin"

dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-rec.bin
echo "== BC8: flash_stage=1 (boot), a header tied to the recovery"
boot BC8 "cpus=1 flash_stage=1"
check "BC8: refused, cleared" has "staged flash: the image was staged for the recovery, and this one's flash_stage_target is boot: not flashed, the header is cleared"
check "BC8: boot untouched" same_as $BOOT_FIRST $BOOT_BLOCKS $W/boot-now2.img
check "BC8: the recovery untouched" same_as $RECOVERY_FIRST $RECOVERY_BLOCKS $W/recovery-now.img
check "BC8: the header cleared" eval '! head_magic'
check "BC8: hello ran" has "Hello from user space on Darwin"

dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST)) < $W/head-boot2.bin
echo "== BC9: flash_stage_target= neither boot nor recovery"
boot BC9 "cpus=1 flash_stage=1 flash_stage_target=system"
check "BC9: left alone" has "staged flash: flash_stage_target= is neither boot nor recovery: left alone, nothing written"
check "BC9: boot untouched" same_as $BOOT_FIRST $BOOT_BLOCKS $W/boot-now2.img
check "BC9: the header still there" head_magic
check "BC9: hello ran" has "Hello from user space on Darwin"
dd if=/dev/zero bs=$BS count=1 2>/dev/null | dd_put $((DARWIN_FIRST + STAGEHEAD_FIRST))

echo "== E: the layout block with a bit changed"
python3 -I - $W/disk.img $(( (DARWIN_FIRST + LAYOUT_BLOCK) * BS + 100 )) <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(int(sys.argv[2])); b = f.read(1); f.seek(int(sys.argv[2])); f.write(bytes([b[0] ^ 1]))
PY
boot E "cpus=1 bootcpu=1 ufs=2 ufs_write=1" -smp 2
check "E: not used" has "layout: darwin's block $LAYOUT_BLOCK not used: its CRC is wrong"
check "E: nothing of it for the kernel" eval '! kernel_args_have ufs_var_first' 
check "E: hello ran" has "Hello from user space on Darwin"

# hello_ret.efi and the disk of the EFI runs: a GPT of 4096-byte blocks (as the phone's UFS) whose partition 1 is
# named logfs, with the phone's logfs type GUID, and FAT12 of the phone's logfs geometry (minfo of
# a backup of logfs: 1280 sectors of 4096 bytes, 1 per cluster, 1 reserved, 2 FATs, 512 root entries)
EFI_CC=${CLANG:-$(command -v clang-18 || command -v clang)}; EFI_LD=${LLD_LINK:-$(command -v lld-link-18 || command -v lld-link)}
mkdir -p $W/efi
if $EFI_CC --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
	-mgeneral-regs-only -O1 -Wall -Ixnuloader -c qemu/efi/hello_ret.c -o $W/efi/hello_ret.obj &&
	$EFI_LD /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$W/efi/hello_ret.efi $W/efi/hello_ret.obj >/dev/null &&
	mkfs.fat -F 12 -S 4096 -s 1 -R 1 -f 2 -r 512 -n LOGFS -C $W/efi/fat.img 5120 >/dev/null &&
	printf 'not an image\n' > $W/efi/notpe.efi &&
	printf '# title|path|options\r\nHakiOS test|\\EFI\\test\\hello_ret.efi|hakios-test-options 42\r\n' > $W/efi/entries.cfg &&
	MTOOLS_SKIP_CHECK=1 mmd -i $W/efi/fat.img ::/EFI ::/EFI/test ::/EFI/junk ::/EFI/hakios &&
	MTOOLS_SKIP_CHECK=1 mcopy -i $W/efi/fat.img $W/efi/hello_ret.efi ::/EFI/test/hello_ret.efi &&
	MTOOLS_SKIP_CHECK=1 mcopy -i $W/efi/fat.img $W/efi/notpe.efi ::/EFI/junk/notpe.efi &&
	MTOOLS_SKIP_CHECK=1 mcopy -i $W/efi/fat.img $W/efi/entries.cfg ::/EFI/hakios/entries.cfg &&
	python3 -I - $W/efi/disk.img $W/efi/fat.img <<'PY'
import struct, sys, uuid, zlib
out, fat = sys.argv[1], open(sys.argv[2], 'rb').read()
bs, first, blocks = 4096, 6, 2048              # the partition: 8 MiB, as logfs (sda14: 2048 blocks)
total = first + blocks + 6
part = struct.pack('<16s16sQQQ72s', uuid.UUID('bc0330eb-3410-4951-a617-03898dbe3372').bytes_le,
                   uuid.UUID('f4a2a42c-31fd-d4f7-e446-b56a69586acf').bytes_le, first, first + blocks - 1, 0,
                   'logfs'.encode('utf-16le').ljust(72, b'\0'))
entries = part.ljust(128 * 128, b'\0')
def header(me, other, table):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, me, other, first, total - 6,
                    uuid.UUID('38e8a47f-0681-e35c-49ab-ef37976dcf1e').bytes_le, table, 128, 128, zlib.crc32(entries))
    h = h[:16] + struct.pack('<I', zlib.crc32(h)) + h[20:]
    return h.ljust(bs, b'\0')
mbr = bytearray(bs)
mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(total - 1, 0xffffffff))
mbr[510:512] = b'\x55\xaa'
d = bytearray(total * bs)
d[0:bs] = mbr
d[bs:2 * bs] = header(1, total - 1, 2)
d[2 * bs:2 * bs + len(entries)] = entries
d[first * bs:first * bs + len(fat)] = fat
d[(total - 5) * bs:(total - 5) * bs + len(entries)] = entries
d[(total - 1) * bs:] = header(total - 1, 1, total - 5)
open(out, 'wb').write(d)
PY
then efi_disk=(-drive file=$W/efi/disk.img,format=raw,if=none,id=efi
	-device virtio-blk-pci,drive=efi,addr=0x6,logical_block_size=4096,physical_block_size=4096)
	efi_sum=$(sha256sum < $W/efi/disk.img)
else
	echo "standalone-test: hello_ret.efi or its disk not made (clang/lld-link, mkfs.fat, mtools): the EFI runs fail"
	efi_disk=(); efi_sum=none
fi

echo "== EFI1: boot_menu_efi=1, a logfs with hello_ret.efi"
boot EFI1 "cpus=1 boot_menu_efi=1" "${efi_disk[@]}"
check "EFI1: UEFI's revision said" flat_has "efi: UEFI 2."
check "EFI1: ACPI 2.0 found, its XSDT listed" flat_has "efi: ACPI XSDT: "
check "EFI1: SMBIOS 3 found" flat_has "efi: SMBIOS 3 at "
check "EFI1: variables answer" flat_has "efi: GetVariable(PlatformLang): "
check "EFI1: ... and the answer is a good one" flat_has " -- variables answer"
check "EFI1: the logfs partition seen as a volume" flat_has '(partition "logfs")'
check "EFI1: the loader's own file left out" flat_has 'EFI\BOOT\BOOTAA64.EFI: the loader itself: left out'
check "EFI1: a .efi that is no image left out" flat_has 'notpe.efi: no MZ header: left out'
check "EFI1: entries.cfg read" flat_has 'efi:   \EFI\hakios\entries.cfg: '
check "EFI1: hello_ret listed first, with its options" flat_has 'efi:   1: HakiOS test (logfs) -- \EFI\test\hello_ret.efi'
check "EFI1: ... once (the same file found in \\EFI\\test not again)" flat_has 'efi: 1 EFI applications found'
check "EFI1: nothing started" eval '! flat_has "efi: starting"'
check "EFI1: hello ran" has "Hello from user space on Darwin"

echo "== EFI2: boot_menu_efi=1 boot_efi_run=1"
boot EFI2 "cpus=1 boot_menu_efi=1 boot_efi_run=1" "${efi_disk[@]}"
check "EFI2: started" flat_has "efi: starting HakiOS test (logfs): "
check "EFI2: loaded from the volume listed, with the options" flat_has 'from volume as listed; load options "hakios-test-options 42"'
check "EFI2: the watchdog armed (5 minutes)" flat_has "efi: the firmware's watchdog: 300 s"
check "EFI2: hello_ret ran and got its options" flat_has "hello_ret: load options [hakios-test-options 42]"
check "EFI2: its own file opens from DeviceHandle" flat_has 'hello_ret: its own file opens from DeviceHandle: \EFI\test\hello_ret.efi'
check "EFI2: its exit data said" flat_has 'efi: its exit data: "hello_ret says goodbye"'
check "EFI2: its status said, then the reset" flat_has "efi: StartImage: EFI_STATUS 8000000000000015 (aborted): the phone resets in 5 s"
check "EFI2: no kernel (reset first)" hasnt "Hello from user space on Darwin"
check "EFI2: the FAT disk not written" eval '[ "$(sha256sum < $W/efi/disk.img)" = "$efi_sum" ]'

echo "== EFI3: boot_efi_run=1 without boot_menu_efi=1"
boot EFI3 "cpus=1 boot_efi_run=1" "${efi_disk[@]}"
check "EFI3: nothing looked for" eval '! flat_has "efi: UEFI " && ! flat_has "efi: starting"'
check "EFI3: hello ran" has "Hello from user space on Darwin"

echo "== EFI4: boot_menu_efi=1 boot_efi_run=5, one entry"
boot EFI4 "cpus=1 boot_menu_efi=1 boot_efi_run=5" "${efi_disk[@]}"
check "EFI4: said, the start goes on" flat_has "efi: boot_efi_run=5: there are 1: the start goes on"
check "EFI4: hello ran" has "Hello from user space on Darwin"

# ---- the boot menu (m1-menu) ----
# keys.py SOCKET LOG SCRIPT: QEMU's serial console; all it prints goes into LOG, and SCRIPT ("|" between steps) is
# typed: "wait TEXT" (until the console has said TEXT since the last wait), up, down, enter (at once), "sleep SECS",
# "shot NAME" (with MON=QEMU's monitor socket and SHOTS=DIR: the screen read out of guest memory -- refind_menu.c's
# 1080x2400 GOP, else the loader's "display W x H at ADDR" -- its 1080x2400 top left corner written to DIR/NAME.png)
cat > $W/keys.py <<'PY'
import os, socket, sys, time
path, log, script = sys.argv[1], sys.argv[2], sys.argv[3].split('|')
s = socket.socket(socket.AF_UNIX)
for _ in range(600):
    try:
        s.connect(path); break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit('keys.py: no QEMU serial socket')
out, seen, keys = open(log, 'wb'), b'', {'up': b'\x1b[A', 'down': b'\x1b[B', 'enter': b'\r'}
def more():
    global seen
    d = s.recv(65536)
    if not d:
        return False
    out.write(d); out.flush(); seen += d
    return True
alive = True
for step in script:
    if not alive:
        break
    if step.startswith('wait '):
        want = step[5:].encode()
        while alive and want not in seen:
            alive = more()
        seen = seen[seen.find(want) + len(want):] if alive else seen
    elif step.startswith('sleep '):
        end = time.time() + float(step[6:])
        while alive and time.time() < end:      # (reading on: QEMU's serial must not back up)
            s.settimeout(max(0.05, end - time.time()))
            try:
                alive = more()
            except socket.timeout:
                pass
        s.settimeout(None)
    elif step.startswith('shot '):
        import re, struct, zlib
        mon, shots = os.environ.get('MON'), os.environ.get('SHOTS')
        t = open(log, 'rb').read()
        g = re.search(rb'GOP is (\d+)x(\d+), pitch (\d+), at ([0-9a-f]+)', t)
        if g:
            w, h, a = int(g.group(3)), int(g.group(2)), int(g.group(4), 16)
        else:
            g = re.search(rb'display (\d+) x (\d+) at ([0-9a-f]+)', t)
            if g:
                w, h, a = int(g.group(1)), int(g.group(2)), int(g.group(3), 16)
        if not mon or not shots or not g:
            continue
        raw = os.path.join(shots, step[5:] + '.raw')
        m = socket.socket(socket.AF_UNIX); m.connect(mon); m.settimeout(60)
        m.sendall(b'pmemsave 0x%x %d "%s"\n' % (a, w * h * 4, raw.encode()))
        got = b''
        while got.count(b'(qemu)') < 2:
            got += m.recv(4096)
        m.close()
        fb, cw, ch = open(raw, 'rb').read(), min(w, 1080), min(h, 2400)
        rows = []
        for y in range(ch):
            r, o = fb[y * w * 4:(y * w + cw) * 4], bytearray(cw * 3)
            o[0::3], o[1::3], o[2::3] = r[2::4], r[1::4], r[0::4]
            rows.append(b'\0' + bytes(o))
        png = b'\x89PNG\r\n\x1a\n'
        for t, d in ((b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0)), (b'IDAT', zlib.compress(b''.join(rows), 9)), (b'IEND', b'')):
            png += struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        open(os.path.join(shots, step[5:] + '.png'), 'wb').write(png)
        os.remove(raw)
    else:
        s.sendall(keys[step])
while more():
    pass
PY
python3 -I - $W/disk.img $(( (DARWIN_FIRST + LAYOUT_BLOCK) * BS + 100 )) <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(int(sys.argv[2])); b = f.read(1); f.seek(int(sys.argv[2])); f.write(bytes([b[0] ^ 1]))
PY
dd if=/dev/zero bs=32 count=1 2>/dev/null | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
rest=$(misc_rest)
run=A; check "A: no menu without boot_menu=1" hasnt "boot_menu"

echo "== MA: boot_menu=1 boot_menu_timeout=3, a key before the menu"
KEYS="wait standalone: the image boots by itself|enter" boot MA "cpus=1 boot_menu=1 boot_menu_timeout=3"
check "MA: the entries" eval 'has "boot_menu=1: 3 entries; HakiOS (iOS) in 3 s unless a key" && has "boot_menu: entry 0: HakiOS (iOS)" && has "boot_menu: entry 1: Android" && has "boot_menu: entry 2: Diagnostics"'
check "MA: no Power Off, EFI or Default Boot" eval '! has "entry 3:" && ! has "Power Off" && ! has "EFI: " && ! has "Default: "'
check "MA: the key from before the menu dropped" has "boot_menu: dropped a key from before the menu: scan 0 char d"
check "MA: shown on HakiOS" has "boot_menu: shown, cursor on HakiOS (iOS)"
check "MA: HakiOS by the timer" has "boot_menu: the timer: HakiOS (iOS)"
check "MA: no key taken" hasnt "boot_menu: key scan"
check "MA: hello ran" has "Hello from user space on Darwin"

echo "== MB: boot_menu_timeout=0"
boot MB "cpus=1 boot_menu=1 boot_menu_timeout=0"
check "MB: 0 not taken by the loader" has "boot_menu: boot_menu_timeout=0 is not taken (the phone would wait for a key for good): 8 s"
check "MB: 8 s" has "HakiOS (iOS) in 8 s unless a key"
check "MB: HakiOS by the timer" has "boot_menu: the timer: HakiOS (iOS)"
check "MB: hello ran" has "Hello from user space on Darwin"
check "MB: standalone.sh build refuses it" eval 'ARGS="cpus=1 boot_menu=1 boot_menu_timeout=0" OUT=$W/x.img devices/poco-x3-pro/tools/standalone.sh build > $W/sa.out 2>&1; grep -q "REFUSED: boot_menu_timeout=0" $W/sa.out'
check "MB: ... and 00 too" eval 'ARGS="cpus=1 boot_menu=1 boot_menu_timeout=00" OUT=$W/x.img devices/poco-x3-pro/tools/standalone.sh build > $W/sa.out 2>&1; grep -q "REFUSED: boot_menu_timeout=0" $W/sa.out'

echo "== MC: boot_menu_default=android, down, up, up, Enter"
KEYS="wait boot_menu: shown|down|wait cursor on Diagnostics|up|wait cursor on Android|up|wait cursor on HakiOS|enter" \
	boot MC "cpus=1 boot_menu=1 boot_menu_default=android boot_menu_timeout=60"
check "MC: Android by default" has "boot_menu=1: 3 entries; Android in 60 s unless a key"
check "MC: the cursor starts on Android" has "boot_menu: shown, cursor on Android"
check "MC: Vol- logged (scan 2)" has "boot_menu: key scan 2 char 0"
check "MC: Vol+ logged (scan 1)" has "boot_menu: key scan 1 char 0"
check "MC: Power logged (char d)" has "boot_menu: key scan 0 char d"
check "MC: the timer off at the first key" has "boot_menu: a key: the timer is off (idle 300 s)"
check "MC: HakiOS chosen" has "boot_menu: chosen: HakiOS (iOS)"
check "MC: not by the timer" hasnt "boot_menu: the timer:"
check "MC: hello ran" has "Hello from user space on Darwin"

echo "== MD: boot_menu_default=android boot_menu_timeout=2 bcb_reboot=1, misc's command boot-recovery"
printf 'boot-recovery' | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
boot MD "cpus=1 boot_menu=1 boot_menu_default=android boot_menu_timeout=2 bcb_reboot=1"
check "MD: Android by the timer" has "boot_menu: the timer: Android"
check "MD: chosen" has "boot_menu: chosen: Android"
check "MD: the hook clears the command" has "menu: Android: misc's command is \"boot-recovery\": clearing it"
check "MD: ... read back, a reset" has "menu: Android: misc's command is clear, read back the same; restarting (ABL starts boot)"
check "MD: no hello (a reset first)" hasnt "Hello from user space on Darwin"
check "MD: misc's command cleared, the rest as it was" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'

echo "== ME: boot_menu_poweroff=1"
KEYS="wait boot_menu: shown|up|wait cursor on Power Off|enter|wait Power again within|down|wait power off not confirmed|up|wait cursor on Power Off|enter|wait Power again within|enter" \
	boot ME "cpus=1 boot_menu=1 boot_menu_poweroff=1 boot_menu_timeout=60"
check "ME: Power Off listed last" has "boot_menu: entry 3: Power Off"
check "ME: asked first" has "boot_menu: power off asked: Power again within 5 s powers the phone off"
check "ME: a volume key takes it back" has "boot_menu: power off not confirmed"
check "ME: confirmed the second time" has "boot_menu: power off confirmed: EfiResetShutdown"
check "ME: asked twice" eval '[ "$(grep -a -c "power off asked" $W/serial-ME.log)" = 2 ]'
check "ME: no hello" hasnt "Hello from user space on Darwin"

echo "== MF: boot_menu_efi=1 boot_menu_bcb=1, Diagnostics, Default Boot"
tries_put 1 4 0
KEYS="wait boot_menu: shown|up|wait cursor on Diagnostics|enter|wait boot_menu: diag: screen|down|up|wait cursor on Default|enter|wait boot_menu: chosen: Default|down|wait cursor on Diagnostics|down|wait cursor on HakiOS|enter" \
	boot MF "cpus=1 boot_menu=1 boot_menu_efi=1 boot_menu_bcb=1 boot_menu_timeout=60" "${efi_disk[@]}"
check "MF: the EFI application listed" has "boot_menu: entry 2: EFI: HakiOS test (logfs)"
check "MF: Default Boot listed" has "boot_menu: entry 3: Default: "
check "MF: Diagnostics after it" has "boot_menu: entry 4: Diagnostics"
check "MF: no EFI start without a choice" eval '! flat_has "efi: starting"'
check "MF: diag: misc's command" has "boot_menu: diag: misc command: (empty)"
check "MF: diag: darwin's count" has "boot_menu: diag: tries: 1 since a good start, 4 starts, 0 recoveries"
check "MF: diag: EFI looked for" has "boot_menu: diag: EFI: 1 found"
check "MF: diag: the keys, last first" has "boot_menu: diag: keys, last first: 0/d 1/0"
check "MF: back from the diagnostics with any key, then Default Boot" has "boot_menu: chosen: Default: "
check "MF: HakiOS chosen" has "boot_menu: chosen: HakiOS (iOS)"
check "MF: the count untouched (no boot_tries=)" eval '[ "$(tries_get)" = "1 4 0" ]'
check "MF: misc untouched" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'
check "MF: hello ran" has "Hello from user space on Darwin"

echo "== MH: boot_menu_efi=1, the EFI entry chosen"
KEYS="wait boot_menu: shown|down|down|wait cursor on EFI: HakiOS test|enter" \
	boot MH "cpus=1 boot_menu=1 boot_menu_efi=1 boot_menu_timeout=60" "${efi_disk[@]}"
check "MH: chosen" has "boot_menu: chosen: EFI: HakiOS test (logfs)"
check "MH: started, with its options" flat_has 'from volume as listed; load options "hakios-test-options 42"'
check "MH: hello_ret ran" flat_has "hello_ret: load options [hakios-test-options 42]"
check "MH: its status, then the reset" flat_has "efi: StartImage: EFI_STATUS 8000000000000015 (aborted): the phone resets in 5 s"
check "MH: no kernel" hasnt "Hello from user space on Darwin"
check "MH: the FAT disk not written" eval '[ "$(sha256sum < $W/efi/disk.img)" = "$efi_sum" ]'

echo "== MI: boot_menu=1 boot_menu_efi=1 boot_efi_run=1, HakiOS by the timer: no EFI start from boot_efi_run"
boot MI "cpus=1 boot_menu=1 boot_menu_efi=1 boot_efi_run=1 boot_menu_timeout=2" "${efi_disk[@]}"
check "MI: HakiOS by the timer" has "boot_menu: the timer: HakiOS (iOS)"
check "MI: nothing started" eval '! flat_has "efi: starting"'
check "MI: hello ran" has "Hello from user space on Darwin"

echo "== MG: boot_menu_idle=3"
KEYS="wait boot_menu: shown|down" boot MG "cpus=1 boot_menu=1 boot_menu_timeout=60 boot_menu_idle=3"
check "MG: the timer off" has "boot_menu: a key: the timer is off (idle 3 s)"
check "MG: HakiOS after 3 s without a key" has "boot_menu: no key for 3 s: HakiOS (iOS)"
check "MG: hello ran" has "Hello from user space on Darwin"

# ---- boot_menu=refind (m4-refind) ----
if [ -n "${REFIND:-}" ] && [ -f "$REFIND/refind_aa64.efi" ]; then
	refindfs() { xnuloader/refind/mkrefindfs.sh $W/refindfs-$1.img "$REFIND" "$L" "$1" >/dev/null && echo "refindfs=$W/refindfs-$1.img"; }
	dd if=/dev/zero bs=32 count=1 2>/dev/null | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
	rest=$(misc_rest)
	ramfb=(-device ramfb)
	# (TCG: rEFInd takes a while to scan and to draw 1080x2400)
	up_wait="wait refind: mark|wait menu drawn|sleep 3"

	echo "== RA: boot_menu=refind, down x3 (wrap), up, down, Enter"
	shots=${SHOTS:-}; [ -z "$shots" ] || mkdir -p "$shots"
	MON=$W/mon.sock SHOTS=$shots MONITOR=unix:$W/mon.sock,server=on,wait=off PAYLOAD_EXTRA=$(refindfs 60) \
	KEYS="$up_wait|shot refind-1-hakios|down|sleep 6|shot refind-2-android|down|sleep 6|shot refind-3-diagnostics|down|sleep 6|shot refind-5-wrapped|up|sleep 6|down|sleep 6|enter|wait refind: chosen" \
		boot RA "cpus=1 boot_menu=refind boot_menu_timeout=60" "${ramfb[@]}"
	check "RA: the disk in RAM is a FAT volume" has "refind: the disk in RAM ("
	check "RA: QEMU's GOP is the phone's 1080x2400" has "refind: QEMU: the GOP is 1080x2400"
	check "RA: rEFInd started" has "refind: rEFInd started (.*), timeout 60 s, idle 300 s"
	check "RA: six keys, none dropped" has "refind: 6 keys while rEFInd was up, 0 of them dropped: 2/0 2/0 2/0 1/0 2/0 0/d"
	check "RA: its own volume alone" has "its own volume alone, graphics 0; Vol+/Vol-/Power only"
	check "RA: rEFInd's marks: volumes, graphics, the menu" eval 'has "refind: mark +[0-9]* ms: scanning volumes" && has "refind: mark +[0-9]* ms: graphics" && has "refind: mark +[0-9]* ms: menu drawn"'
	check "RA: rEFInd's entry for this loader is not taken for another application" hasnt "rEFInd starts an EFI application"
	check "RA: HakiOS by hand, through the hand-off" has "refind: chosen HakiOS (by hand)"
	check "RA: no text menu" hasnt "boot_menu: shown"
	check "RA: hello ran" has "Hello from user space on Darwin"
	[ -z "$shots" ] || check "RA: screenshots" eval '[ -s "$shots/refind-1-hakios.png" ] && [ -s "$shots/refind-3-diagnostics.png" ]'

	echo "== RH: refind_scan=all, up (wraps to the ESP's loader), down, Enter"
	MON=$W/mon.sock SHOTS=$shots MONITOR=unix:$W/mon.sock,server=on,wait=off PAYLOAD_EXTRA=$(refindfs 60) \
	KEYS="$up_wait|up|sleep 6|shot refind-4-efi|down|sleep 6|enter|wait refind: chosen" \
		boot RH "cpus=1 boot_menu=refind boot_menu_timeout=60 refind_scan=all" "${ramfb[@]}"
	check "RH: every volume" has "every volume, graphics 0; Vol+/Vol-/Power only"
	check "RH: HakiOS by hand" eval 'has "refind: 3 keys while rEFInd was up, 0 of them dropped: 1/0 2/0 0/d" && has "refind: chosen HakiOS (by hand)"'
	check "RH: hello ran" has "Hello from user space on Darwin"

	echo "== RB: rEFInd's timer"
	PAYLOAD_EXTRA=$(refindfs 3) boot RB "cpus=1 boot_menu=refind boot_menu_timeout=3" "${ramfb[@]}"
	check "RB: HakiOS by rEFInd's timer" has "refind: chosen HakiOS (rEFInd's timer)"
	check "RB: hello ran" has "Hello from user space on Darwin"

	echo "== RC: Android, bcb_reboot=1, misc's command boot-recovery"
	printf 'boot-recovery' | dd of=$W/disk.img bs=1 seek=$((MISC_FIRST * BS)) conv=notrunc 2>/dev/null
	PAYLOAD_EXTRA=$(refindfs 60) KEYS="$up_wait|down|sleep 4|enter" \
		boot RC "cpus=1 boot_menu=refind boot_menu_timeout=60 bcb_reboot=1" "${ramfb[@]}"
	check "RC: Android chosen" has "refind: chosen Android (by hand)"
	check "RC: the hook clears the command" has "menu: Android: misc's command is \"boot-recovery\": clearing it"
	check "RC: no hello (a reset first)" hasnt "Hello from user space on Darwin"
	check "RC: misc's command cleared, the rest as it was" eval '[ -z "$(misc_command)" ] && [ "$(misc_rest)" = "$rest" ]'

	echo "== RD: Diagnostics, then HakiOS from the text menu"
	PAYLOAD_EXTRA=$(refindfs 60) KEYS="$up_wait|down|sleep 4|down|sleep 4|enter|wait boot_menu: shown|down|sleep 1|enter" \
		boot RD "cpus=1 boot_menu=refind boot_menu_timeout=60" "${ramfb[@]}"
	check "RD: Diagnostics chosen" has "refind: chosen Diagnostics (by hand)"
	check "RD: the diagnostics page" has "boot_menu: LoadOptions menu=diag: the diagnostics page first"
	check "RD: HakiOS from the text menu" has "boot_menu: chosen: HakiOS (iOS)"
	check "RD: hello ran" has "Hello from user space on Darwin"

	echo "== RE: boot_menu=refind without rEFInd in the image"
	boot RE "cpus=1 boot_menu=refind boot_menu_timeout=2" "${ramfb[@]}"
	check "RE: said" has "refind: boot_menu=refind but no rEFInd in the image"
	check "RE: the text menu's timer" has "boot_menu: the timer: HakiOS (iOS)"
	check "RE: hello ran" has "Hello from user space on Darwin"

	echo "== RG: boot_menu_idle=5, a key, nothing chosen"
	PAYLOAD_EXTRA=$(refindfs 60) KEYS="$up_wait|down" boot RG "cpus=1 boot_menu=refind boot_menu_timeout=60 boot_menu_idle=5" "${ramfb[@]}"
	check "RG: the loader resets, from rEFInd's loop" has "refind: no key for boot_menu_idle= s, nothing chosen: a reset"
	check "RG: no hello" hasnt "Hello from user space on Darwin"

	echo "== RI: refind_test_hang=1 refind_hang=15"
	PAYLOAD_EXTRA=$(refindfs 60) boot RI "cpus=1 boot_menu=refind boot_menu_timeout=60 refind_test_hang=1 refind_hang=15" "${ramfb[@]}"
	check "RI: rEFInd hangs after its graphics mark" has "refind: mark +[0-9]* ms: test: hanging here"
	check "RI: the loader resets" has "refind: no sign of life from rEFInd for 15 s (last mark: test: hanging here): a reset"
	check "RI: no hello" hasnt "Hello from user space on Darwin"

	echo "== RJ: refind_safe=1 refind_conf=bmp"
	MON=$W/mon.sock SHOTS=$shots MONITOR=unix:$W/mon.sock,server=on,wait=off PAYLOAD_EXTRA=$(refindfs 8) \
	KEYS="wait menu drawn|sleep 2|shot refind-bmp-safe" \
		boot RJ "cpus=1 boot_menu=refind boot_menu_timeout=8 refind_safe=1 refind_conf=bmp" "${ramfb[@]}"
	check "RJ: rEFInd told" has "refind: refind_conf=bmp: rEFInd's LoadOptions \"refind_aa64.efi -c refind-bmp.conf\""
	check "RJ: graphics 7" has "its own volume alone, graphics 7;"
	check "RJ: the half-size BMP banner" eval 'has "refind: mark +[0-9]* ms: load theme.background-half.bmp" && has "refind: mark +[0-9]* ms: banner scaled 1080x2400"'
	check "RJ: no read-back of the screen" eval '! has "refind: mark +[0-9]* ms: blt: read"'
	check "RJ: the menu, then HakiOS by the timer" eval 'has "refind: mark +[0-9]* ms: menu drawn" && has "refind: chosen HakiOS (rEFInd'"'"'s timer)"'
	check "RJ: hello ran" has "Hello from user space on Darwin"

	echo "== RK: refind_conf=text"
	PAYLOAD_EXTRA=$(refindfs 3) boot RK "cpus=1 boot_menu=refind boot_menu_timeout=3 refind_conf=text" "${ramfb[@]}"
	check "RK: rEFInd told" has "refind_aa64.efi -c refind-text.conf"
	check "RK: HakiOS by the timer" has "refind: chosen HakiOS (rEFInd's timer)"
	check "RK: hello ran" has "Hello from user space on Darwin"
else
	echo "== R*: skipped (REFIND= is not a directory of xnuloader/refind/build.sh's)"
fi

[ $fails = 0 ] && echo "standalone-test: PASS (logs/standalone-test-*.log)" || { echo "standalone-test: $fails FAILED (logs/standalone-test-*.log)"; exit 1; }
