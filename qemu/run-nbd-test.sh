#!/bin/bash
# The kernel's NBD disk (bsd/dev/vayu_nbd.c, nbd=1) in QEMU, against the host's tools/nbd/nbd-server.py, the way the
# phone is to take an image from the host at the other end of the USB cable: QEMU's user network is made the phone's (the guest
# 10.11.0.2 on en0 = virtio_net=1, the host 10.11.0.1, which QEMU turns into this machine's loopback, where the
# server listens). userland/src/nbd_test.c is process 1 of a tiny APFS root; it reads the first MB MiB of
# /dev/rnbd0 and mounts the export from /dev/nbd0, and this script compares the sums it says with the image's.
#
#   SCENARIO=basic    (default) an APFS container (tools/apfs/mkapfs.py) with test.bin, read only: the raw sum of
#                     the first MB MiB, the speed, and test.bin's sum through the APFS mount
#   SCENARIO=restart  the same, and the server is killed in the middle of the read and started again RESTART_GAP s
#                     later: the reads must go on over a new connection with the same sums
#   SCENARIO=rw       an HFS Plus volume, the server with --cow: boot 1 (nbd_rw=1) writes a file and unmounts, boot 2
#                     finds it and writes its own; the image itself must not change (its sum before and after), and
#                     a boot without nbd_rw must be refused the writable mount
#
# usage: qemu/run-nbd-test.sh
# env:   K       the kernel (default xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837)
#        LOADER  loader.efi (default xnuloader/loader.efi, else prebuilt/loader.efi)
#        QEMU, QEMU_EFI, QEMU_VARS   qemu-system-aarch64 and its UEFI (default: in $PATH, Arch's edk2 paths)
#        MKFS    mkfs.hfsplus (SCENARIO=rw)   MB  MiB to read raw (default 100)   PORT  the server's (default 10809)
#        SELF_MB the kernel's own read at the start (nbd_selftest=, default 16), its CRC-32 against the server's --crc
#        FILE_MB test.bin's MiB (default 16)   W  work directory (default qemu/nbd-test)   SECS  a boot's budget (default 900)
#        EXTRA   more boot-args
# Exit 0 when every boot says PASS and the sums agree.
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
SCENARIO=${SCENARIO:-basic}
case $SCENARIO in basic|restart|rw) ;; *) echo "run-nbd-test: SCENARIO is basic, restart or rw"; exit 1 ;; esac
W=$(mkdir -p "${W:-qemu/nbd-test}" && cd "${W:-qemu/nbd-test}" && pwd)
K=${K:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}
LOADER=${LOADER:-xnuloader/loader.efi}; [ -f "$LOADER" ] || LOADER=prebuilt/loader.efi
QEMU=${QEMU:-qemu-system-aarch64}
QEMU_EFI=${QEMU_EFI:-/usr/share/edk2/aarch64/QEMU_EFI.fd}
QEMU_VARS=${QEMU_VARS:-/usr/share/edk2/aarch64/QEMU_VARS.fd}
MKFS=${MKFS:-mkfs.hfsplus}
MB=${MB:-100}; FILE_MB=${FILE_MB:-16}; PORT=${PORT:-10809}; SECS=${SECS:-900}; RESTART_GAP=${RESTART_GAP:-3}; SELF_MB=${SELF_MB:-16}
for f in "$K" "$LOADER" "$QEMU_EFI" "$QEMU_VARS"; do [ -f "$f" ] || { echo "run-nbd-test: no $f"; exit 1; }; done

# the program, as userland/build.sh builds init (static, libmini), and its root
. ./xnu-env.sh
rm -rf "$W/root" "$W/esp" && mkdir -p "$W/root/dev" "$W/root/mnt" "$W/root/sbin" "$W/esp/EFI/BOOT" || exit 1
xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
	-Iuserland/libmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$W/root/sbin/launchd" \
	userland/src/nbd_test.c userland/libmini/mini.c userland/libmini/start.S || exit 1
python3 tools/apfs/mkapfs.py -o "$W/root.apfs" Root="$W/root" >/dev/null || exit 1
cp "$LOADER" "$W/esp/EFI/BOOT/BOOTAA64.EFI"

# the export
rm -rf "$W/export" "$W/cow" && mkdir -p "$W/export" || exit 1
if [ "$SCENARIO" = rw ]; then
	truncate -s 256M "$W/disk.img" && "$MKFS" -s -J -b 4096 -U 0 -G 0 -M 755 -v NBD "$W/disk.img" >/dev/null || exit 1
	FS_ARGS="nbd_test_fs=hfs"
else
	# fill.bin first and as big as the raw read, so the image's first MB MiB are mostly random data; test.bin after
	# it, what is read through the APFS mount (FILE_MB MiB: TCG's network is slow)
	head -c $((MB << 20)) /dev/urandom > "$W/export/fill.bin"
	head -c $((FILE_MB << 20)) /dev/urandom > "$W/export/test.bin"
	python3 tools/apfs/mkapfs.py -o "$W/disk.img" Test="$W/export" >/dev/null || exit 1
	FS_ARGS=""
fi
img_sum=$(sha256sum "$W/disk.img" | cut -c1-64)
raw_want=$(head -c $((MB << 20)) "$W/disk.img" | sha256sum | cut -c1-64)
file_want=$( [ -f "$W/export/test.bin" ] && sha256sum "$W/export/test.bin" | cut -c1-64)
echo "run-nbd-test: $SCENARIO: export $(($(stat -c %s "$W/disk.img") >> 20)) MiB, its first $MB MiB $raw_want${file_want:+, test.bin $file_want}"

server=
start_server() {
	local mode=()
	[ "$SCENARIO" = rw ] && mode=(--cow "$W/cow")
	python3 -I tools/nbd/nbd-server.py --listen 127.0.0.1 --port "$PORT" --crc "$SELF_MB" "${mode[@]}" test="$W/disk.img" >> "$W/server.log" 2>&1 &
	server=$!
	for _ in $(seq 50); do grep -q "listening on" "$W/server.log" 2>/dev/null && return 0; sleep 0.1; done
	echo "run-nbd-test: the server did not start"; cat "$W/server.log"; return 1
}
stop_server() { [ -n "$server" ] && kill "$server" 2>/dev/null; wait "$server" 2>/dev/null; server=; }
trap 'stop_server' EXIT
: > "$W/server.log"
start_server || exit 1

# one boot: its extra args; prints the log's lines of interest; 0 if it said PASS
boot() {
	local n=$1 extra=$2 log="$W/serial-$1.log" qemu waited=0 killed=0
	: > "$log"
	xnuloader/mkpayload.py -o "$W/payload.bin" kernel="$K" ramdisk="$W/root.apfs" \
		--cmdline "cpus=1 rd=md0 virtio_net=1 nbd=1 nbd_host=10.11.0.1 nbd_port=$PORT nbd_export=test nbd_if=en0 nbd_ip=10.11.0.2 nbd_test_mb=$MB $FS_ARGS $extra${EXTRA:+ $EXTRA}" >/dev/null || return 1
	cp "$QEMU_VARS" "$W/vars.fd"
	"$QEMU" -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 4G -accel tcg,tb-size=384 \
		-drive if=pflash,format=raw,readonly=on,file="$QEMU_EFI" -drive if=pflash,format=raw,file="$W/vars.fd" \
		-drive file=fat:rw:"$W/esp",format=raw,if=virtio -global virtio-mmio.force-legacy=false \
		-netdev user,id=n0,net=10.11.0.0/24,host=10.11.0.1,dhcpstart=10.11.0.100 -device virtio-net-device,netdev=n0 \
		-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
		-display none -serial file:"$log" -monitor none -no-reboot </dev/null >/dev/null 2>&1 &
	qemu=$!
	while [ $waited -lt "$SECS" ] && kill -0 $qemu 2>/dev/null && ! grep -aq "nbd_test: \(PASS\|FAIL\)" "$log"; do
		if [ "$SCENARIO" = restart ] && [ $killed = 0 ] && grep -aq "nbd_test: /dev/rnbd0 open" "$log"; then
			sleep 2; stop_server; echo "run-nbd-test: the server killed in the middle of the read"
			sleep "$RESTART_GAP"; start_server || return 1; echo "run-nbd-test: the server started again"
			killed=1
		fi
		sleep 1; waited=$((waited + 1))
	done
	sleep 2; kill -9 $qemu 2>/dev/null; wait $qemu 2>/dev/null
	echo "== boot $n ($extra; $waited s)"
	strings "$log" | grep -a "nbd\|panic(\|virtio-net\|^net: \|apfs: device\|hfs: " | grep -v "^nbd_test: process" | cut -c1-220
	grep -aq "nbd_test: PASS" "$log"
}

said() { strings "$W/serial-$1.log" | grep -a "$2" | head -1 | sed "s/.*$3 \([0-9a-f]\{64\}\).*/\1/"; }
fail=0
if [ "$SCENARIO" = rw ]; then
	boot 1 "nbd_rw=1 nbd_test_write=1" || fail=1
	boot 2 "nbd_rw=1 nbd_test_check=1 nbd_test_write=2" || fail=1
	# a boot without nbd_rw: the writable mount must be refused
	boot 3 "nbd_test_write=3" && { echo "run-nbd-test: boot 3 mounted writable without nbd_rw=1"; fail=1; }
	grep -aq "HFS Plus from /dev/nbd0 on /mnt, writable" "$W/serial-3.log" && grep -aq "nbd_test: FAILED: HFS Plus from /dev/nbd0 on /mnt, writable" "$W/serial-3.log" ||
		{ echo "run-nbd-test: boot 3 did not have its writable mount refused"; fail=1; }
	[ "$(sha256sum "$W/disk.img" | cut -c1-64)" = "$img_sum" ] && echo "run-nbd-test: the image itself is as it was" ||
		{ echo "run-nbd-test: THE IMAGE CHANGED"; fail=1; }
	written=$(python3 -c "import sys; print(sum(bin(b).count('1') for b in open(sys.argv[1],'rb').read()))" "$W/cow/test.cow.map" 2>/dev/null)
	echo "run-nbd-test: ${written:-no} blocks of 4 KiB in the overlay"
	[ "${written:-0}" -gt 0 ] || fail=1
else
	boot 1 "nbd_selftest=$SELF_MB" || fail=1
	# the kernel's own read of the first SELF_MB MiB (nbd_selftest) against the server's sum of them
	crc_k=$(strings "$W/serial-1.log" | grep -a -m1 "nbd: selftest:" | sed 's/.*crc32 \([0-9a-f]\{8\}\).*/\1/')
	crc_h=$(grep -m1 "crc32" "$W/server.log" | sed 's/.*crc32 \([0-9a-f]\{8\}\).*/\1/')
	[ -n "$crc_k" ] && [ "$crc_k" = "$crc_h" ] && echo "run-nbd-test: the kernel's selftest of $SELF_MB MiB: crc32 $crc_k, the server's too" ||
		{ echo "run-nbd-test: selftest crc32 '$crc_k', the server's '$crc_h'"; fail=1; }
	raw=$(said 1 "nbd_test: raw" sha256); file=$(said 1 "nbd_test: file test.bin" sha256)
	[ "$raw" = "$raw_want" ] && echo "run-nbd-test: the raw $MB MiB: the same sum" || { echo "run-nbd-test: raw sum $raw, not $raw_want"; fail=1; }
	[ "$file" = "$file_want" ] && echo "run-nbd-test: test.bin through APFS: the same sum" || { echo "run-nbd-test: test.bin sum $file, not $file_want"; fail=1; }
	if [ "$SCENARIO" = restart ]; then
		grep -aq "nbd: connected again" "$W/serial-1.log" && echo "run-nbd-test: the kernel connected again" ||
			{ echo "run-nbd-test: the kernel did not say it connected again"; fail=1; }
	fi
fi
echo "-- the server"; cut -c1-200 "$W/server.log" | grep -v "connected$" | tail -8
[ $fail = 0 ] && echo "run-nbd-test: $SCENARIO PASS" || { echo "run-nbd-test: $SCENARIO FAIL"; exit 1; }
