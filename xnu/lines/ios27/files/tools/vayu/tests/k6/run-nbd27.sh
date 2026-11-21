#!/bin/bash
# run-nbd27.sh KERNEL SCENARIO: the NBD disk of an xnu-12377 VAYU kernel (bsd/dev/vayu_nbd.c) in QEMU against
# nbd-server.py, the way the phone takes its root over en2 -- here over en0, QEMU's virtio NIC (bsd/dev/vayu_qemu_net.c,
# qemu_net=1). QEMU's user network is made the phone's: the guest 10.11.0.2, the host 10.11.0.1 (this machine's
# loopback, where the server listens). Process 1 is nbd27_test (mockfs: the ramdisk is that one program).
# Runs on a Linux host with QEMU 9.2 (our xnuloader 2.7, xnu=12377), the 8796 line's toolchain and libmini.
# env: HAKIOS (the directory with env/qemu, the loader and the repository; required), LOADER_DIR, REPO
#
#   basic     two exports (nbd0 64 MiB of random data, nbd1 another), nbd_selftest=16: the kernel's CRC-32 of the first
#             16 MiB against the server's, the raw SHA-256 of both disks against the images', EROFS without nbd_rw
#   bs4096    basic with nbd_bs=4096
#   restart   the server killed in the middle of the read and started again: the same sums, "connected again"
#   ident     after a first read the server is restarted with another image of the same size: the kernel must refuse it
#             ("not the export the disk was") and, nbd_giveup=15, the next read fails with EIO
#   nxio      nbd_export=nosuch: the server refuses it, the open fails with ENXIO after nbd_wait=10
#   rw        nbd_rw=1, the server with --cow: 4 KiB written at 8 KiB and 2 MiB, read back; the server restarted on the same
#             overlay: the kernel takes it again (its first 64 KiB with the write in them); the image itself unchanged
#   root      the two-step root: the RAM root is HFS Plus ($HFS/ramroot.cdr: /sbin/launchd = nbd27_test, /dev, /flash), the
#             export another HFS Plus volume ($HFS/nbdroot.cdr: test.bin, marker.txt, /old, /dev; both made by hdiutil on
#             the Mac, test.bin's SHA-256 in $HFS/test.bin.sha256); process 1 mounts it from /dev/nbd0 on /flash and asks
#             kern.hakios_switch_root "/flash old": the export is the root, test.bin the same, a second switch refused
#   off       vayu27=1 without qemu_net/nbd: no "nbd:" or "vayu-qemu-net:" line, no /dev/rnbd0
# env: W (work dir, default ${TMPDIR:-/tmp}/nbd27-$SCENARIO), PORT (default 10870), SECS (default 600),
#      EXTRA (more boot-args)
# Exit 0 on PASS.
set -u
K=$(readlink -f "$1"); SC=${2:-basic}
H=${HAKIOS:?HAKIOS= the directory with env/qemu, the loader and the repository}; Q=$H/env/qemu
L=${LOADER_DIR:-$H/xnuloader}; R=${REPO:-$H/repo}
SRC=$(cd "$(dirname "$0")" && pwd)
W=${W:-${TMPDIR:-/tmp}/nbd27-$SC}; PORT=${PORT:-10870}; SECS=${SECS:-600}; HFS=${HFS:-$H/hfs}
rm -rf "$W" && mkdir -p "$W/esp/EFI/BOOT" || exit 1

# process 1
( cd $R && . ./xnu-env.sh && xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
	-Wall -Wextra -Werror -Iuserland/libmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$W/launchd" \
	"$SRC/nbd27_test.c" userland/libmini/mini.c userland/libmini/start.S ) || { echo "run-nbd27: the test does not build"; exit 1; }

# the exports: random data (the first 64 KiB differ between them)
head -c $((64 << 20)) /dev/urandom > "$W/disk0.img"
head -c $((16 << 20)) /dev/urandom > "$W/disk1.img"
cp "$W/disk0.img" "$W/other.img" && head -c 65536 /dev/urandom | dd of="$W/other.img" conv=notrunc status=none
sum0=$(sha256sum "$W/disk0.img" | cut -c1-64)
raw0=$(head -c $((64 << 20)) "$W/disk0.img" | sha256sum | cut -c1-64)
raw1=$(head -c $((8 << 20)) "$W/disk1.img" | sha256sum | cut -c1-64)

server=
start_server() {        # IMAGE [more server args]
	local img=$1; shift
	python3 -I $R/tools/nbd/nbd-server.py --listen 127.0.0.1 --port $PORT --crc 16 "$@" test="$img" two="$W/disk1.img" \
		>> "$W/server.log" 2>&1 &
	server=$!
	for _ in $(seq 100); do [ "$(grep -c 'listening on' "$W/server.log")" -gt "${started:-0}" ] && { started=$((${started:-0} + 1)); return 0; }; sleep 0.1; done
	echo "run-nbd27: the server did not start"; cat "$W/server.log"; return 1
}
stop_server() { [ -n "$server" ] && kill "$server" 2>/dev/null; wait "$server" 2>/dev/null; server=; }
qemu=
trap 'stop_server; [ -n "$qemu" ] && kill -9 $qemu 2>/dev/null' EXIT
: > "$W/server.log"

NET="vayu27=1 qemu_net=1 nbd=1 nbd_if=en0 nbd_ip=10.11.0.2 nbd_host=10.11.0.1 nbd_port=$PORT nbd_export=test"
COW=()
case $SC in
basic)   ARGS="$NET nbd_selftest=16 nbd_test_mb=64 nbd1=1 nbd1_export=two nbd_test_mb1=8" ;;
bs4096)  ARGS="$NET nbd_bs=4096 nbd1=1 nbd1_export=two nbd1_bs=4096 nbd_test_mb=64 nbd_test_mb1=8" ;;
restart) ARGS="$NET nbd_test_mb=64" ;;
ident)   ARGS="$NET nbd_test_mb=4 nbd_test_mb2=4 nbd_test_pause=20 nbd_test_after=eio nbd_giveup=15" ;;
nxio)    ARGS="vayu27=1 qemu_net=1 nbd=1 nbd_if=en0 nbd_ip=10.11.0.2 nbd_host=10.11.0.1 nbd_port=$PORT nbd_export=nosuch nbd_wait=10 nbd_test_expect=nxio" ;;
rw)      ARGS="$NET nbd_rw=1 nbd_test_write=1 nbd_test_mb=4 nbd_test_mb2=4 nbd_test_pause=20 nbd_test_after=same"; mkdir -p "$W/cow"; COW=(--cow "$W/cow") ;;
off)     ARGS="vayu27=1" ;;
root)    ARGS="rd=md0 $NET nbd_test_mb=8 nbd_test_root=1"; cp "$HFS/nbdroot.cdr" "$W/disk0.img"; cp "$HFS/ramroot.cdr" "$W/ramdisk"
         raw0=$(head -c $((8 << 20)) "$W/disk0.img" | sha256sum | cut -c1-64); sum0=$(sha256sum "$W/disk0.img" | cut -c1-64) ;;
*) echo "run-nbd27: no scenario $SC"; exit 1 ;;
esac
ARGS="cpus=1 -v serial=3 debug=0x8 xnu=12377 wdt=-1 $ARGS ${EXTRA:-}"
[ $SC = off ] || start_server "$W/disk0.img" "${COW[@]}" || exit 1

cp $L/loader.efi "$W/esp/EFI/BOOT/BOOTAA64.EFI"; cp $Q/share/qemu/edk2-arm-vars.fd "$W/vars.fd"
[ -f "$W/ramdisk" ] || cp "$W/launchd" "$W/ramdisk"
python3 $L/mkpayload.py -o "$W/payload.bin" --xnu 12377 kernel="$K" ramdisk="$W/ramdisk" --cmdline "$ARGS" >/dev/null || exit 1
log="$W/serial.log"; : > "$log"
$Q/bin/qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 6G -accel tcg,tb-size=384 \
	-drive if=pflash,format=raw,readonly=on,file=$Q/share/qemu/edk2-aarch64-code.fd -drive if=pflash,format=raw,file="$W/vars.fd" \
	-drive file=fat:rw:"$W/esp",format=raw,if=virtio -global virtio-mmio.force-legacy=false \
	-netdev user,id=n0,net=10.11.0.0/24,host=10.11.0.1,dhcpstart=10.11.0.100 -device virtio-net-device,netdev=n0 \
	-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
	-display none -serial file:"$log" -monitor none -no-reboot </dev/null >/dev/null 2>&1 &
qemu=$!
waited=0; acted=0
while [ $waited -lt $SECS ] && kill -0 $qemu 2>/dev/null && ! grep -aq "nbd_test: \(PASS\|FAIL\)\|panic(" "$log"; do
	if [ $acted = 0 ] && [ $SC = restart ] && grep -aq "nbd_test: /dev/rnbd0 open" "$log"; then
		sleep 3; stop_server; echo "run-nbd27: the server killed in the middle of the read"; sleep 3
		start_server "$W/disk0.img" || exit 1; echo "run-nbd27: the server started again"; acted=1
	fi
	if [ $acted = 0 ] && [ $SC = ident ] && grep -aq "nbd_test: pause" "$log"; then
		stop_server; start_server "$W/other.img" || exit 1; echo "run-nbd27: the server restarted with another image of the same size"; acted=1
	fi
	if [ $acted = 0 ] && [ $SC = rw ] && grep -aq "nbd_test: pause" "$log"; then
		stop_server; start_server "$W/disk0.img" "${COW[@]}" || exit 1; echo "run-nbd27: the server restarted on the same overlay"; acted=1
	fi
	sleep 1; waited=$((waited + 1))
done
sleep 2; kill -9 $qemu 2>/dev/null; wait $qemu 2>/dev/null; qemu=
echo "== $SC ($waited s): $ARGS"
strings "$log" | grep -a "nbd\|panic(\|vayu-qemu-net\|VAYU: after devfs\|Darwin Kernel Version\|BSD root\|hakios:\|hfs" | grep -av "^Boot args" | cut -c1-230 | head -60

fail=0
said() { strings "$log" | grep -a "nbd_test: $1 raw" | head -1 | sed 's/.*sha256 \([0-9a-f]\{64\}\).*/\1/'; }
want() { strings "$log" | grep -aq "$1" && echo "run-nbd27: ok: $2" || { echo "run-nbd27: FAILED: $2"; fail=1; }; }
case $SC in
off)
	strings "$log" | grep -aq "Hello\|nbd_test: process" && echo "run-nbd27: ok: process 1 ran" || { echo "run-nbd27: FAILED: no process 1"; fail=1; }
	strings "$log" | grep -a "^nbd: \|vayu-qemu-net:\|vayu-usbnet:\|vayu-ctl:" && { echo "run-nbd27: FAILED: a driver said something"; fail=1; } ||
		echo "run-nbd27: ok: no NBD, NIC, USB or ctl line"
	want "nbd_test: no nbd=1" "no nbd=1, nothing read" ;;
*)
	want "nbd_test: PASS" "nbd27_test PASS" ;;
esac
case $SC in
basic|bs4096|restart)
	[ "$(said nbd0)" = "$raw0" ] && echo "run-nbd27: ok: nbd0's 64 MiB: the image's sum" || { echo "run-nbd27: FAILED: nbd0 '$(said nbd0)', not $raw0"; fail=1; } ;;
esac
case $SC in
basic|bs4096)
	[ "$(said nbd1)" = "$raw1" ] && echo "run-nbd27: ok: nbd1's 8 MiB: the image's sum" || { echo "run-nbd27: FAILED: nbd1 '$(said nbd1)', not $raw1"; fail=1; } ;;
esac
if [ $SC = basic ]; then
	crc_k=$(strings "$log" | grep -a -m1 "nbd: nbd0: selftest:" | sed 's/.*crc32 \([0-9a-f]\{8\}\).*/\1/')
	crc_h=$(grep -m1 "'test'.*crc32" "$W/server.log" | sed 's/.*crc32 \([0-9a-f]\{8\}\).*/\1/')
	[ -n "$crc_k" ] && [ "$crc_k" = "$crc_h" ] && echo "run-nbd27: ok: the kernel's selftest crc32 $crc_k = the server's" ||
		{ echo "run-nbd27: FAILED: selftest crc32 '$crc_k', the server's '$crc_h'"; fail=1; }
fi
if [ $SC = root ]; then
	[ "$(said nbd0)" = "$raw0" ] && echo "run-nbd27: ok: nbd0's 8 MiB: the image's sum" || { echo "run-nbd27: FAILED: nbd0 '$(said nbd0)', not $raw0"; fail=1; }
	want "hakios: /flash is the root now, the one before it on /old: 0" "the kernel switched the root"
	f=$(strings "$log" | grep -a -m1 "nbd_test: file /test.bin" | sed 's/.*sha256 \([0-9a-f]\{64\}\).*/\1/')
	[ "$f" = "$(cat "$HFS/test.bin.sha256")" ] && echo "run-nbd27: ok: /test.bin on the new root: the file's sum" ||
		{ echo "run-nbd27: FAILED: /test.bin '$f', not $(cat "$HFS/test.bin.sha256")"; fail=1; }
fi
[ $SC = bs4096 ] && want "nbd_test: nbd0 blocks of 4096" "blocks of 4096"
[ $SC = restart ] && want "nbd: nbd0: connected again" "the kernel connected again"
[ $SC = ident ] && want "not the export the disk was, refused" "the other export refused" && want "nbd: nbd0: no connection for 15 s" "the read given up after nbd_giveup"
[ $SC = nxio ] && want "the server refuses export \"nosuch\"" "the server's refusal said" && want "no server answered in 10 s" "the open gave up after nbd_wait"
if [ $SC = rw ]; then
	want "nbd: nbd0: connected again" "the kernel took the restarted server (the same first 64 KiB, its write in them)"
	[ "$(sha256sum "$W/disk0.img" | cut -c1-64)" = "$sum0" ] && echo "run-nbd27: ok: the image itself is as it was" || { echo "run-nbd27: FAILED: THE IMAGE CHANGED"; fail=1; }
fi
echo "-- the server"; cut -c1-200 "$W/server.log" | tail -6
[ $fail = 0 ] && echo "run-nbd27: $SC PASS" || { echo "run-nbd27: $SC FAIL (log $log)"; exit 1; }
