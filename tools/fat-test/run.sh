#!/bin/bash
# The kernel's read-only FAT reader (bsd/dev/hakios.fat.c: firmware out of the phone's "modem" and "bluetooth"
# partitions) built on this machine under the address and undefined-behaviour sanitizers, and checked against
# mtools: on FAT16 and FAT32 volumes made here (long and short names, upper and lower case, directories in
# directories, a fragmented file, files of 0, 1 and cluster +/- 1 bytes), and on every file of each image named
# in FAT_IMAGES (for instance the copies of the phone's partitions: FAT_IMAGES="modem.img bluetooth.img").
# Needs mkfs.fat (dosfstools) and mtools. Nothing is written but under TMPDIR.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}
[ -f "$K/bsd/dev/hakios.fat.c" ] || K=$ROOT/xnu/files
export MTOOLS_SKIP_CHECK=1
W=$(mktemp -d "${TMPDIR:-/tmp}/fat-test.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/inc/sys"
ln -s "$K/bsd/sys/hakios.fat.h" "$W/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
${CC:-cc} $FLAGS -I"$W/inc" -o "$W/test" "$HERE/test_fat.c" "$K/bsd/dev/hakios.fat.c"
echo "fat-test: with $K/bsd/dev/hakios.fat.c"
"$W/test" paths

fail=0
# every regular file of image $1 read by the kernel's code and by mtools, compared
check_all() {
	local img=$1 n=0 p
	while IFS= read -r p; do
		p=${p#::/}
		[ -n "$p" ] || continue
		case $p in */) continue ;; esac
		mcopy -n -i "$img" "::/$p" "$W/want" 2>/dev/null || continue     # (a directory)
		if ! "$W/test" "$img" "$p" "$W/got"; then
			fail=1
		elif ! cmp -s "$W/want" "$W/got"; then
			echo "FAIL: $img: $p differs"
			fail=1
		fi
		# the same name in other case must be found too
		if ! "$W/test" "$img" "$(echo "$p" | tr 'a-z' 'A-Z')" "$W/got" || ! cmp -s "$W/want" "$W/got"; then
			echo "FAIL: $img: $p not found in upper case"
			fail=1
		fi
		n=$((n + 1))
	done < <(mdir -/ -b -i "$img" ::/)
	echo "fat-test: $img: $n files compared"
	[ "$n" -gt 0 ] || fail=1
}

make_volume() {      # $1 image, $2.. mkfs.fat options
	local img=$1
	shift
	rm -f "$img"
	mkfs.fat -C "$img" "$@" >/dev/null
	mmd -i "$img" ::/image ::/image/modem_pr ::/image/modem_pr/mcfg ::/image/modem_pr/mcfg/configs
	head -c 9260 /dev/urandom > "$W/modem.mdt"
	head -c 1 /dev/urandom > "$W/one"
	: > "$W/empty"
	mcopy -i "$img" "$W/modem.mdt" ::/image/modem.mdt
	mcopy -i "$img" "$W/one" "::/image/A_Long_Name_With_Underscores.bin"
	mcopy -i "$img" "$W/empty" ::/image/empty.bin
	mcopy -i "$img" "$W/one" ::/image/UPPER.TXT
	mcopy -i "$img" "$W/one" ::/image/MixedCase_And_A_Name_Longer_Than_Thirty_Nine_Characters.mbn
	for s in 4095 4096 4097 16383 16384 16385; do
		head -c $s /dev/urandom > "$W/s$s"
		mcopy -i "$img" "$W/s$s" "::/image/modem_pr/mcfg/configs/size_$s.bin"
	done
	# a fragmented file: small files in between, every other one deleted, then a large one in the holes
	for i in $(seq 1 40); do
		head -c 20000 /dev/urandom > "$W/f$i"
		mcopy -i "$img" "$W/f$i" "::/image/gap$i.bin"
	done
	for i in $(seq 1 2 40); do mdel -i "$img" "::/image/gap$i.bin"; done
	head -c 300000 /dev/urandom > "$W/big"
	mcopy -i "$img" "$W/big" ::/image/modem.b02
}

make_volume "$W/fat16.img" -F 16 -S 4096 -s 4 81920          # as vayu's modem partition: 4096-byte sectors, 16 KB clusters
check_all "$W/fat16.img"
make_volume "$W/fat32.img" -F 32 -S 512 -s 1 70000
check_all "$W/fat32.img"
# what must not be found
for p in image/nothing.bin image image/modem_pr image/modem.mdt/x "../image/modem.mdt" image/../image/modem.mdt; do
	"$W/test" "$W/fat16.img" "$p" - || fail=1
done

for img in $FAT_IMAGES; do
	check_all "$img"
done
if [ $fail -ne 0 ]; then
	echo "fat-test: FAIL"
	exit 1
fi
echo "fat-test: PASS"
