#!/bin/bash
# darwin-install.sh and standalone.sh against a phone made of files: a fake adb and a fake fastboot that run the
# scripts' commands on files standing for the LUNs, the partitions, sysfs and /proc/mounts. What it shows: every
# check refuses what it should (another GPT, another sysfs view, a mounted darwin, a size, a missing magic, no
# DARWIN_WRITE/FLASH_BOOT, a layout block that is not darwin-layout.py's), the writes land where the layout says,
# read back, and a boot write that does not read back puts the saved boot back. The writes go as phone-write.sh
# makes them: a push, the staged copy checked, dd from it; the fake adb breaks each step once (a wrong copy, a
# device that writes wrong, no room, another writer, a shell that drops while its dd goes on) and the scripts must
# refuse, roll back or wait -- never start a second writer. No phone is touched.
#
#   devices/poco-x3-pro/tools/install-test.sh [GPT_DIR]
#
# Two phones: the QEMU test disk's numbers (darwin-layout.py --profile qemu: small, every write done in full),
# and -- with GPT_DIR holding this phone's gpt-sda.head.bin and gpt-sde.head.bin (darwin-install.sh dump-gpt
# writes them) -- vayu's own numbers on sparse files the size of sda and sde (checks, the layout
# block and boot only, unless ROOT_IMG, OS_IMG, APP_IMG (and VAR_IMG) name the real images: then the whole
# installation is done on the files, as it would be on the phone -- that wants some 30 GB of disk).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GPT_DIR=${1:-}
T=$(mktemp -d "${TMPDIR:-/tmp}/install-test.XXXXXX"); trap 'rm -rf "$T"' EXIT
export FAKE=$T/phone FAKE_SERIAL=0123456789abcdef TOP_LOGS=$T
export SERIAL=$FAKE_SERIAL STAGE_DIR=/hakios-stage      # the phone's staging place: $FAKE/stage
fails=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fails=$((fails + 1)); }
expect() {      # expect yes|no WHAT COMMAND...: the command succeeds (yes) or is refused (no)
	local want=$1 what=$2; shift 2
	if "$@" > $T/out 2>&1; then got=yes; else got=no; fi
	[ $got = $want ] && ok "$what" || { bad "$what"; sed 's/^/       | /' $T/out | tail -5; }
}

mkdir -p $T/bin
cat > $T/bin/adb <<'EOF'
#!/bin/bash
# The phone's side of the writes (phone-write.sh) runs here as it would there. Switches, each used once:
#   $FAKE/corrupt     NAME: the next dd that writes a node ending in NAME writes every byte +1 (the device's fault)
#   $FAKE/badpush     the next push delivers a copy with one byte changed (the transport's fault)
#   $FAKE/drop        SECONDS: the next writing shell returns at once, without a word (the transport broke), while
#                     its script goes on in the background SECONDS later -- as a dd may go on after adb has gone
[ "$1" = -s ] && { [ "$2" = "$FAKE_SERIAL" ] || { echo "error: device '$2' not found" >&2; exit 1; }; shift 2; }
rewrite() { sed -e "s|/hakios-stage|$FAKE/stage|g" -e "s|/dev/block/|$FAKE/dev/block/|g" -e "s|/sys/class/block/|$FAKE/sys/class/block/|g" \
	-e "s|/proc/sys/vm/drop_caches|$FAKE/drop_caches|g" -e "s|/proc/mounts|$FAKE/mounts|g"; }
pre='blockdev() { case $1 in --getsize64) stat -L -c %s "$2" ;; --getss) echo 4096 ;; esac; }; id() { echo 0; };
dd() { local a i= o=(); if [ -f $FAKE/corrupt ] && [[ " $* " == *" of="*"$(cat $FAKE/corrupt) "*fsync* ]]; then
	rm -f $FAKE/corrupt; for a; do case $a in if=*) i=${a#if=} ;; *) o+=("$a") ;; esac; done
	tr "\000-\377" "\001-\377\000" < "$i" | command dd "${o[@]}"; else command dd "$@"; fi; };'
case $1 in
get-state) cat $FAKE/state ;;
shell)
	if [ -f $FAKE/drop ] && [[ "$2" == *conv=notrunc,fsync* ]]; then
		d=$(cat $FAKE/drop); rm -f $FAKE/drop
		( sleep $d; bash -c "$pre $(printf '%s' "$2" | rewrite)" ) </dev/null >/dev/null 2>&1 &
		echo "error: closed" >&2; exit 255
	fi
	bash -c "$pre $(printf '%s' "$2" | rewrite)" | sed "s|$FAKE||g" ;;
exec-out) bash -c "$pre $(printf '%s' "$2" | rewrite)" ;;
push)
	dst=$(printf '%s' "$3" | rewrite)
	[ -d "$(dirname "$dst")" ] || { echo "adb: error: failed to copy '$2' to '$3': remote No such file or directory" >&2; exit 1; }
	cp "$2" "$dst" || exit 1
	if [ -f $FAKE/badpush ]; then rm -f $FAKE/badpush; printf '\x5a' | dd of="$dst" bs=1 seek=100 conv=notrunc 2>/dev/null; fi
	echo "$2: 1 file pushed." ;;
exec-in) echo "$*" >> $FAKE/exec-in-used; cat > /dev/null; exit 1 ;;     # not a write path any more
*) exit 1 ;;
esac
EOF
cat > $T/bin/fastboot <<'EOF'
#!/bin/bash
[ "$1" = -s ] && { [ "$2" = "$FAKE_SERIAL" ] || exit 1; shift 2; }
[ "$(cat $FAKE/state)" = fastboot ] || { [ "$1" = devices ] && exit 0; echo "< waiting for any device >"; exit 1; }
case $1 in
devices) echo "$FAKE_SERIAL	fastboot" ;;
getvar) echo "$2: vayu" >&2 ;;
flash) case $2 in boot|recovery) dd if="$3" of=$FAKE/dev/block/$(cat $FAKE/${2}node) conv=notrunc 2>/dev/null && echo OKAY ;; esac ;;
boot) echo "booting $2"; echo OKAY ;;
esac
EOF
chmod +x $T/bin/*
export ADB=$T/bin/adb FASTBOOT=$T/bin/fastboot

# a phone of PROFILE: the LUN files (their GPT copied in), the partition files, sysfs as the kernel shows it
phone() {       # phone PROFILE HEAD_OF_DARWIN_LUN HEAD_OF_BOOT_LUN [TAILS...]
	local prof=$1 dh=$2 bh=$3
	rm -rf $FAKE; mkdir -p $FAKE/dev/block/by-name $FAKE/sys/class/block
	eval "$(python3 -I $HERE/darwin-layout.py --profile $prof sh)"
	: > $FAKE/mounts; : > $FAKE/drop_caches; echo recovery > $FAKE/state; mkdir -p $FAKE/stage
	truncate -s $((DARWIN_LUN_BLOCKS * BS)) $FAKE/dev/block/$DARWIN_DISK
	dd if=$dh of=$FAKE/dev/block/$DARWIN_DISK conv=notrunc 2>/dev/null
	[ -e $FAKE/dev/block/$BOOT_DISK ] || { truncate -s $((BOOT_LUN_BLOCKS * BS)) $FAKE/dev/block/$BOOT_DISK;
		dd if=$bh of=$FAKE/dev/block/$BOOT_DISK conv=notrunc 2>/dev/null; }
	truncate -s $((DARWIN_BLOCKS * BS)) $FAKE/dev/block/$DARWIN_DISK$DARWIN_PART
	truncate -s $BOOT_BYTES $FAKE/dev/block/$BOOT_DISK$BOOT_PART
	truncate -s $RECOVERY_BYTES $FAKE/dev/block/$RECOVERY_DISK$RECOVERY_PART
	truncate -s $MISC_BYTES $FAKE/dev/block/$MISC_DISK$MISC_PART
	ln -s $FAKE/dev/block/$MISC_DISK$MISC_PART $FAKE/dev/block/by-name/misc
	ln -s $FAKE/dev/block/$DARWIN_DISK$DARWIN_PART $FAKE/dev/block/by-name/darwin
	ln -s $FAKE/dev/block/$BOOT_DISK$BOOT_PART $FAKE/dev/block/by-name/boot
	ln -s $FAKE/dev/block/$RECOVERY_DISK$RECOVERY_PART $FAKE/dev/block/by-name/recovery
	echo $BOOT_DISK$BOOT_PART > $FAKE/bootnode
	echo $RECOVERY_DISK$RECOVERY_PART > $FAKE/recoverynode
	for p in "$DARWIN_DISK $DARWIN_PART $DARWIN_FIRST $DARWIN_BLOCKS darwin" "$BOOT_DISK $BOOT_PART $BOOT_FIRST $BOOT_BLOCKS boot" \
	    "$RECOVERY_DISK $RECOVERY_PART $RECOVERY_FIRST $RECOVERY_BLOCKS recovery" "$MISC_DISK $MISC_PART $MISC_FIRST $MISC_BLOCKS misc"; do
		set -- $p
		mkdir -p $FAKE/sys/class/block/$1$2
		echo $2 > $FAKE/sys/class/block/$1$2/partition
		echo $(($3 * 8)) > $FAKE/sys/class/block/$1$2/start
		echo $(($4 * 8)) > $FAKE/sys/class/block/$1$2/size
		printf 'MAJOR=259\nPARTN=%s\nPARTNAME=%s\n' $2 $5 > $FAKE/sys/class/block/$1$2/uevent
	done
	printf 'ANDROID!' > $T/android-boot.img; head -c 3000000 /dev/urandom >> $T/android-boot.img
	dd if=$T/android-boot.img of=$FAKE/dev/block/$BOOT_DISK$BOOT_PART conv=notrunc 2>/dev/null
	printf 'ANDROID!' > $T/pbrp.img; head -c 2000000 /dev/urandom >> $T/pbrp.img
	dd if=$T/pbrp.img of=$FAKE/dev/block/$RECOVERY_DISK$RECOVERY_PART conv=notrunc 2>/dev/null
}
# an image of this port's as mkxnuimg.py makes it (header 1, kernel = gzip(shim + XNUP) + DTB), its command line ARGS
portimg() {     # portimg OUT ARGS
	python3 -I - "$1" "$2" <<'PY'
import gzip, os, struct, sys
out, args = sys.argv[1], sys.argv[2].encode() + b'\0'
def entry(name, data):
    return struct.pack('<16sII', name, len(data), 0) + data + bytes(-len(data) % 16)
body = entry(b'kernel', os.urandom(70000)) + entry(b'cmdline', args) + entry(b'standalone', b'1\0') + bytes(24)
kernel = gzip.compress(b'shim XNUP not this one' + os.urandom(5000) + struct.pack('<II', 0x50554E58, len(body)) + body,
                       6, mtime=0) + b'\xd0\x0d\xfe\xed' + bytes(100)
h = struct.pack('<8s10I', b'ANDROID!', len(kernel), 0x8000, 0, 0x1000000, 0, 0, 0x100, 4096, 1, 0)
open(out, 'wb').write(h + bytes(4096 - len(h)) + kernel + bytes(-len(kernel) % 4096))
PY
}
apfs() {        # apfs OUT BLOCKS: a stand-in APFS container (its superblock says BLOCKS blocks of 4096)
	python3 -I -c '
import struct, sys
n = int(sys.argv[2]); f = open(sys.argv[1], "wb"); f.truncate(n * 4096)
b = bytearray(4096); b[32:36] = b"NXSB"; struct.pack_into("<IQ", b, 36, 4096, n); b[100:120] = b"stand-in container  "
f.seek(0); f.write(b); f.seek(n * 4096 - 4096); f.write(b"end of the container".ljust(4096, b"."))' "$1" "$2"
}
I="$HERE/darwin-install.sh"
S="$HERE/standalone.sh"
cd $T

echo "== the QEMU test disk's numbers (every write in full)"
export DARWIN_PROFILE=qemu
python3 -I $HERE/darwin-layout.py --profile qemu disk $T/vda.img >/dev/null
phone qemu $T/vda.img $T/vda.img
eval "$(python3 -I $HERE/darwin-layout.py --profile qemu sh)"
expect yes "check: the disk as set up" $I check
expect no  "another serial is not this phone" env SERIAL=0000 $I check
expect no  "no SERIAL: no phone is assumed" env -u SERIAL $I check
expect no  "no SERIAL: standalone.sh does not assume one either" env -u SERIAL BACKUPS=$T/bk $S save
echo 1 > $FAKE/sys/class/block/vda2/partition; expect no "sysfs says another entry number" $I check; echo 2 > $FAKE/sys/class/block/vda2/partition
echo "$FAKE/dev/block/vda2 /mnt ext4 rw 0 0" > $FAKE/mounts; expect no "darwin mounted: refused" $I check; : > $FAKE/mounts
cp $FAKE/dev/block/vda $T/vda.keep; printf '\x01' | dd of=$FAKE/dev/block/vda bs=1 seek=$((2 * BS + 128 + 40)) conv=notrunc 2>/dev/null
expect no "a GPT changed under its CRC: refused" $I check; cp $T/vda.keep $FAKE/dev/block/vda
echo fastboot > $FAKE/state; expect no "not in the recovery: refused" $I check; echo recovery > $FAKE/state
apfs $T/root.apfs $ROOT_BLOCKS; apfs $T/os.apfs $OSCRYPTEX_BLOCKS; apfs $T/short.apfs $((ROOT_BLOCKS / 2))
apfs $T/liar.apfs $ROOT_BLOCKS; python3 -I -c 'import struct,sys; f=open(sys.argv[1],"r+b"); f.seek(40); f.write(struct.pack("<Q", 2*int(sys.argv[2])))' $T/liar.apfs $ROOT_BLOCKS
expect no  "write before the layout block is there: refused" env DARWIN_WRITE=yes $I write root $T/root.apfs
expect no  "layout without DARWIN_WRITE=yes: refused" $I layout
expect yes "layout: written" env DARWIN_WRITE=yes $I layout
expect yes "layout again: there already" env DARWIN_WRITE=yes $I layout
cmp -s <(dd if=$FAKE/dev/block/vda2 bs=$BS skip=$LAYOUT_BLOCK count=1 2>/dev/null) <(python3 -I $HERE/darwin-layout.py --profile qemu block -) &&
	ok "layout: the block is darwin-layout.py's, at LAYOUT_BLOCK" || bad "layout: the block is not where it should be"
expect no  "write without DARWIN_WRITE=yes: refused" $I write root $T/root.apfs
expect no  "write a region that does not exist (var through write): refused" env DARWIN_WRITE=yes $I write var $T/root.apfs
expect no  "write a container smaller than its region: refused" env DARWIN_WRITE=yes $I write root $T/short.apfs
head -c $((ROOT_BLOCKS * BS)) /dev/zero > $T/zeros.img
expect no  "write something that is no APFS container: refused" env DARWIN_WRITE=yes $I write root $T/zeros.img
expect no  "write a container whose superblock says another size: refused" env DARWIN_WRITE=yes $I write root $T/liar.apfs
expect yes "write root: written, read back" env DARWIN_WRITE=yes $I write root $T/root.apfs
expect yes "write oscryptex: written, read back" env DARWIN_WRITE=yes $I write oscryptex $T/os.apfs
cmp -s <(dd if=$FAKE/dev/block/vda2 bs=$BS skip=$OSCRYPTEX_FIRST count=$OSCRYPTEX_BLOCKS 2>/dev/null) $T/os.apfs &&
	ok "oscryptex is at its first block" || bad "oscryptex is not where it should be"
echo "-- the writes themselves: staged, chunked, checked (phone-write.sh)"
apfs $T/app.apfs $APPCRYPTEX_BLOCKS
cp $T/app.apfs $T/app2.apfs; printf 'second' | dd of=$T/app2.apfs bs=1 seek=$((APPCRYPTEX_BLOCKS * BS - 10000)) conv=notrunc 2>/dev/null
region() { cmp -s <(dd if=$FAKE/dev/block/vda2 bs=$BS skip=$APPCRYPTEX_FIRST count=$APPCRYPTEX_BLOCKS 2>/dev/null) "$1"; }
expect yes "write appcryptex in chunks of 100 blocks (100+100+56)" env DARWIN_WRITE=yes PW_CHUNK_BLOCKS=100 $I write appcryptex $T/app.apfs
grep -q '100%' $T/out && region $T/app.apfs && ok "the chunks: every one in its place" || bad "the chunks"
touch $FAKE/badpush
expect no  "a push that delivers another copy: refused before dd" env DARWIN_WRITE=yes PW_CHUNK_BLOCKS=100 $I write appcryptex $T/app2.apfs
grep -q "staged copy's sha256" $T/out && region $T/app.apfs && ok "... the region untouched" || bad "... the region"
rm -f $FAKE/badpush
expect no  "no room in the staging place: refused" env DARWIN_WRITE=yes PW_RESERVE_MB=100000000 $I write appcryptex $T/app2.apfs
grep -q "KiB free" $T/out && region $T/app.apfs && ok "... nothing written" || bad "... no room"
expect no  "no staging place: refused" env DARWIN_WRITE=yes STAGE_DIR=/nowhere $I write appcryptex $T/app2.apfs
(exec -a dd python3 -c 'import time; time.sleep(60)' of=$FAKE/dev/block/vda2) & writer=$!; sleep 0.3
expect no  "a dd writing darwin already: no second writer" env DARWIN_WRITE=yes $I write appcryptex $T/app2.apfs
kill $writer; wait $writer 2>/dev/null
grep -q "a dd is writing" $T/out && region $T/app.apfs && ok "... refused in the writing shell, nothing written" || bad "... the other writer"
echo 1 > $FAKE/drop
expect yes "the shell drops, the write goes on: waited for, read back" env DARWIN_WRITE=yes $I write appcryptex $T/app2.apfs
grep -q "UNKNOWN" $T/out && grep -q "the writer is gone" $T/out && region $T/app2.apfs && ok "... the region is the image" || bad "... dropped shell"
echo 4 > $FAKE/drop
expect no  "the shell drops, the writer does not end in PW_WAIT: nothing more" env DARWIN_WRITE=yes PW_WAIT=2 PW_CHUNK_BLOCKS=100 $I write appcryptex $T/app.apfs
grep -q "may still be running" $T/out && ! grep -q '%:' $T/out && ok "... stopped at the first chunk, no second writer" || bad "... unsettled"
sleep 3; [ -z "$(ls $FAKE/stage)" ] && ok "the staging place is empty again" || bad "the staging place: $(ls $FAKE/stage)"
expect yes "write appcryptex once more" env DARWIN_WRITE=yes $I write appcryptex $T/app.apfs
python3 -I - $T/var.img $((VAR_BLOCKS * BS)) <<'PY'
import sys
f = open(sys.argv[1], 'wb'); f.truncate(int(sys.argv[2]))
f.seek(1024); f.write(b'HX' + b'\0' * 510 + b'volume header')
f.seek(int(sys.argv[2]) // 2); f.write(b'journal' * 2000)
f.seek(int(sys.argv[2]) - 8192); f.write(b'alternate header'.ljust(8192, b'\0'))
PY
expect yes "var: written piece by piece, read back" env DARWIN_WRITE=yes $I var $T/var.img
cmp -s <(dd if=$FAKE/dev/block/vda2 bs=$BS skip=$VAR_FIRST count=$VAR_BLOCKS 2>/dev/null) $T/var.img &&
	ok "var: the region is the volume" || bad "var: the region is not the volume"
expect no  "var again, a volume there: refused" env DARWIN_WRITE=yes $I var $T/var.img
expect yes "var again with VAR_REINIT=yes" env DARWIN_WRITE=yes VAR_REINIT=yes $I var $T/var.img
[ "$(dd if=$FAKE/dev/block/vda2 bs=$BS skip=$VARSTORE_FIRST count=$VARSTORE_BLOCKS 2>/dev/null | tr -d '\0' | wc -c)" = 0 ] &&
	ok "var_store's slots untouched (zeros)" || bad "var_store's slots were written"
expect yes "status" $I status
grep -q "root ($ROOT_FIRST + $ROOT_BLOCKS blocks): an APFS container of $((ROOT_BLOCKS * BS)) bytes, filling it" $T/out &&
	grep -q "an HFS Plus volume" $T/out && grep -q "the SAME as" $T/out && ok "status says what is there" ||
	{ bad "status says what is there"; sed 's/^/       | /' $T/out; }
expect yes "dump-gpt: both LUNs' ends, checked" $I dump-gpt $T/dump

echo "== boot (standalone.sh), the QEMU numbers"
printf 'ANDROID!' > $T/new.img; head -c 1234567 /dev/urandom >> $T/new.img
printf 'NOTBOOT!' > $T/notboot.img; head -c 1000 /dev/urandom >> $T/notboot.img
{ printf 'ANDROID!'; head -c $((BOOT_BYTES + 4096)) /dev/zero; } > $T/big.img
mkdir -p $T/logs
expect no  "flash without FLASH_BOOT=yes: refused" env BACKUPS=$T/bk $S flash $T/new.img
expect no  "flash something that is not a boot image: refused" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/notboot.img
expect no  "flash an image larger than boot: refused" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/big.img
expect yes "flash: saved first, written, read back" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/new.img
saved=$(ls $T/bk/boot-*.img | head -1)
cmp -s <(head -c $(stat -c %s $T/android-boot.img) $saved) $T/android-boot.img && ok "the saved copy is the old boot" || bad "the saved copy"
expect yes "verify: boot begins with the new image" env BACKUPS=$T/bk $S verify $T/new.img
n=$(stat -c %s $T/new.img)
[ $((n % 4096)) != 0 ] && cmp -s <(tail -c +$((n + 1)) $FAKE/dev/block/vda1) <(tail -c +$((n + 1)) $saved) &&
	ok "an image of $n bytes (not whole blocks): the bytes after it are boot's as they were" || bad "the bytes after the image"
echo $BOOT_NODE | sed 's|.*/||' > $FAKE/corrupt
expect no  "flash whose write goes wrong: refused, the saved boot put back" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/notboot.img
cp $T/new.img $T/new2.img; printf 'x' >> $T/new2.img
echo $BOOT_NODE | sed 's|.*/||' > $FAKE/corrupt
expect no  "flash whose write goes wrong (a boot image): refused" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/new2.img
grep -q "boot is as it was" $T/out && ok "the boot of before is back, read back" || bad "the boot of before was not put back"
expect yes "verify: still the image flashed before" env BACKUPS=$T/bk $S verify $T/new.img
touch $FAKE/badpush
expect no  "flash whose push goes wrong: refused, the saved boot put back" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/new2.img
grep -q "staged copy's sha256" $T/out && grep -q "boot is as it was" $T/out && ok "... refused before dd, boot as it was" || bad "... bad push"
echo 1 > $FAKE/drop
expect yes "flash whose shell drops: the writer waited for, read back" env BACKUPS=$T/bk FLASH_BOOT=yes $S flash $T/new2.img
grep -q "UNKNOWN" $T/out && ok "... said so" || bad "... said so"
echo 4 > $FAKE/drop
expect no  "flash whose writer does not end in PW_WAIT: refused, NO rollback" env BACKUPS=$T/bk FLASH_BOOT=yes PW_WAIT=2 $S flash $T/new.img
grep -q "Do NOT restart" $T/out && ! grep -q "putting .* back" $T/out && ok "... no second writer on boot" || bad "... a rollback over the writer"
sleep 3
expect yes "verify: the writer that went on has put the image in" env BACKUPS=$T/bk $S verify $T/new.img
expect yes "rollback to the first saved copy" env BACKUPS=$T/bk FLASH_BOOT=yes $S rollback $saved
expect yes "verify: boot is the old one again" env BACKUPS=$T/bk $S verify $T/android-boot.img
cp $saved $T/bk/boot-20260101-000000-0000000000000000.img
expect no  "rollback to a copy that is not what its name says: refused" env BACKUPS=$T/bk FLASH_BOOT=yes $S rollback $T/bk/boot-20260101-000000-0000000000000000.img
rm $T/bk/boot-20260101-000000-0000000000000000.img
echo fastboot > $FAKE/state
expect no  "rollback-fastboot without FLASH_BOOT=yes: refused" env BACKUPS=$T/bk $S rollback-fastboot $T/new.img
expect yes "rollback-fastboot: fastboot flash boot" env BACKUPS=$T/bk FLASH_BOOT=yes $S rollback-fastboot $T/new.img
expect yes "try: fastboot boot (an image as it is)" env BACKUPS=$T/bk $S try $T/new.img
echo recovery > $FAKE/state
expect yes "verify: boot is what fastboot flashed" env BACKUPS=$T/bk $S verify $T/new.img

echo "== the recovery (standalone.sh --part recovery: the multiboot, HakiOS in the recovery), the QEMU numbers"
R="$S --part recovery"
portimg $T/rec-ok.img "ufs=2 flash_stage=1 flash_stage_target=recovery usbnet_cmd=1 bcb_reboot=1"
portimg $T/rec-boot.img "ufs=2 flash_stage=1 usbnet_cmd=1 bcb_reboot=1"
portimg $T/rec-plain.img "ufs=2 usbnet_cmd=1"
expect yes "image-args.py: the command line out of the gzipped kernel" eval 'python3 -I $HERE/image-args.py $T/rec-ok.img flash_stage_target=recovery flash_stage=1'
expect no  "image-args.py: a word that is not there" python3 -I $HERE/image-args.py $T/rec-boot.img flash_stage_target=recovery
expect no  "image-args.py: no image of this port's (no XNUP)" python3 -I $HERE/image-args.py $T/new.img
boot_before=$(sha256sum < $FAKE/dev/block/$BOOT_DISK$BOOT_PART)
expect no  "--part recovery flash with FLASH_BOOT=yes alone: refused" env BACKUPS=$T/bkr FLASH_BOOT=yes $R flash $T/rec-ok.img
expect no  "--part nowhere: refused" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $S --part nowhere flash $T/rec-ok.img
expect no  "flash_stage=1 without flash_stage_target=recovery into the recovery: refused" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R flash $T/rec-boot.img
grep -q "would stage into boot" $T/out && ok "... and says why" || bad "... why"
expect no  "an image with flash_stage_target=recovery into boot: refused" env BACKUPS=$T/bkr FLASH_BOOT=yes $S flash $T/rec-ok.img
expect no  "--part recovery build: refused (no such thing)" $R build
expect yes "--part recovery flash: saved first, written, read back" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R flash $T/rec-ok.img
rsaved=$(ls $T/bkr/recovery-*.img 2>/dev/null | head -1)
[ -n "$rsaved" ] && cmp -s <(head -c $(stat -c %s $T/pbrp.img) $rsaved) $T/pbrp.img && ok "the saved copy is the old recovery (recovery-*.img)" || bad "the saved copy of the recovery"
cmp -s <(head -c $(stat -c %s $T/rec-ok.img) $FAKE/dev/block/$RECOVERY_DISK$RECOVERY_PART) $T/rec-ok.img && ok "the recovery holds the image" || bad "the recovery does not hold the image"
[ "$(sha256sum < $FAKE/dev/block/$BOOT_DISK$BOOT_PART)" = "$boot_before" ] && ok "boot untouched" || bad "boot was written"
expect yes "--part recovery verify" env BACKUPS=$T/bkr $R verify $T/rec-ok.img
expect no  "--part recovery verify of another image: refused" env BACKUPS=$T/bkr $R verify $T/rec-plain.img
expect yes "--part recovery flash: an image of the port without flash_stage=1" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R flash $T/rec-plain.img
echo $RECOVERY_NODE | sed 's|.*/||' > $FAKE/corrupt
expect no  "--part recovery flash whose write goes wrong: refused, the saved recovery put back" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R flash $T/rec-ok.img
grep -q "recovery is as it was" $T/out && ok "... the recovery of before is back" || bad "... not put back"
expect yes "--part recovery rollback to PBRP (an image not of the port's: taken as it is)" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R rollback $rsaved
expect yes "--part recovery verify: PBRP again" env BACKUPS=$T/bkr $R verify $T/pbrp.img
[ "$(sha256sum < $FAKE/dev/block/$BOOT_DISK$BOOT_PART)" = "$boot_before" ] && ok "boot untouched through all of it" || bad "boot was written"
echo fastboot > $FAKE/state
expect no  "--part recovery rollback-fastboot without FLASH_RECOVERY=yes: refused" env BACKUPS=$T/bkr FLASH_BOOT=yes $R rollback-fastboot $T/rec-ok.img
expect yes "--part recovery rollback-fastboot: fastboot flash recovery" env BACKUPS=$T/bkr FLASH_RECOVERY=yes $R rollback-fastboot $T/rec-ok.img
grep -q "fastboot flash recovery" $T/out && ok "... said so" || bad "... said so"
echo recovery > $FAKE/state
expect yes "--part recovery verify: what fastboot flashed" env BACKUPS=$T/bkr $R verify $T/rec-ok.img
[ "$(sha256sum < $FAKE/dev/block/$BOOT_DISK$BOOT_PART)" = "$boot_before" ] && ok "boot untouched by the recovery's fastboot" || bad "boot was written"

echo "== misc from the recovery (phone-mode.sh bcb, bcb-set, android), the QEMU numbers"
P="env IP=127.0.0.1 PORT=1 WAIT=1 PHONE_MODE_LOG=$T/phone-mode.log $HERE/phone-mode.sh"
MN=$FAKE/dev/block/$MISC_DISK$MISC_PART
python3 -I -c 'import sys; b = bytearray(8192); r = b"recovery\n--prompt_and_wipe_data\n"; b[64:64 + len(r)] = r; b[5000] = 7; sys.stdout.buffer.write(b)' |
	dd of=$MN conv=notrunc 2>/dev/null
mrest() { tail -c +33 $MN | sha256sum; }
mcmd() { head -c 32 $MN | tr -d '\0'; }
rest0=$(mrest)
expect yes "bcb: read" $P bcb
grep -q 'recovery "recovery|--prompt_and_wipe_data|"' $T/out && grep -q "WARNING: the recovery field asks for userdata to be wiped" $T/out &&
	ok "bcb: the recovery field shown, the wipe said loudly" || { bad "bcb: the recovery field"; sed 's/^/       | /' $T/out; }
expect no  "bcb-set without MISC_WRITE=yes: refused" $P bcb-set
[ -z "$(mcmd)" ] && ok "... misc untouched" || bad "... misc written"
expect yes "bcb-set: boot-recovery, read back" env MISC_WRITE=yes $P bcb-set
[ "$(mcmd)" = boot-recovery ] && [ "$(mrest)" = "$rest0" ] && ok "... the command alone" || bad "... misc: '$(mcmd)'"
expect yes "bcb-set again: nothing written" env MISC_WRITE=yes $P bcb-set
grep -q "already: nothing written" $T/out && ok "... said so" || bad "... said so"
expect no  "android from the recovery: misc cleared, then no Android in WAIT (the fake has none)" env MISC_WRITE=yes $P android
[ -z "$(mcmd)" ] && [ "$(mrest)" = "$rest0" ] && ok "... misc's command cleared, the rest as it was" || bad "... misc: '$(mcmd)'"
printf 'boot-fastboot' | dd of=$MN conv=notrunc 2>/dev/null
expect no  "bcb-set over another command: refused" env MISC_WRITE=yes $P bcb-set
[ "$(mcmd)" = boot-fastboot ] && ok "... left alone" || bad "... misc: '$(mcmd)'"
dd if=/dev/zero of=$MN bs=32 count=1 conv=notrunc 2>/dev/null

if [ -n "$GPT_DIR" ] && [ -f "$GPT_DIR/gpt-sda.head.bin" ] && [ -f "$GPT_DIR/gpt-sde.head.bin" ]; then
	echo "== vayu's own numbers (sparse sda and sde with this phone's GPT; no 5.6 GB write)"
	export DARWIN_PROFILE=vayu
	eval "$(python3 -I $HERE/darwin-layout.py sh)"
	phone vayu $GPT_DIR/gpt-sda.head.bin $GPT_DIR/gpt-sde.head.bin
	expect yes "check: sda33 as this phone has it" $I check
	echo $((DARWIN_FIRST * 8 + 8)) > $FAKE/sys/class/block/sda33/start; expect no "sysfs says darwin begins elsewhere: refused" $I check
	echo $((DARWIN_FIRST * 8)) > $FAKE/sys/class/block/sda33/start
	expect yes "layout: written into block $LAYOUT_BLOCK" env DARWIN_WRITE=yes $I layout
	cmp -s <(dd if=$FAKE/dev/block/sda33 bs=$BS skip=2097151 count=1 2>/dev/null) <(python3 -I $HERE/darwin-layout.py block -) &&
		ok "layout: block 2097151 holds it" || bad "layout: block 2097151"
	apfs $T/small.apfs 256
	expect no  "write root with a container of another size: refused" env DARWIN_WRITE=yes $I write root $T/small.apfs
	expect yes "status" $I status
	grep -q "no APFS container" $T/out && grep -q "nothing staged" $T/out && ok "status: nothing installed yet" || bad "status"
	if [ -n "${ROOT_IMG:-}" ]; then
		# the real images (env ROOT_IMG, OS_IMG, APP_IMG, VAR_IMG): the whole installation, as on the phone
		expect yes "write root: $(basename $ROOT_IMG)" env DARWIN_WRITE=yes $I write root $ROOT_IMG
		expect yes "write oscryptex: $(basename $OS_IMG)" env DARWIN_WRITE=yes $I write oscryptex $OS_IMG
		expect yes "write appcryptex: $(basename $APP_IMG)" env DARWIN_WRITE=yes $I write appcryptex $APP_IMG
		[ -z "${VAR_IMG:-}" ] || expect yes "var: $(basename $VAR_IMG)" env DARWIN_WRITE=yes $I var $VAR_IMG
		expect yes "status after the installation" $I status
		[ "$(grep -c 'filling it' $T/out)" = 3 ] && { [ -z "${VAR_IMG:-}" ] || grep -q 'an HFS Plus volume' $T/out; } &&
			ok "status: three containers filling their regions${VAR_IMG:+, the volume at var}" ||
			{ bad "status after the installation"; sed 's/^/       | /' $T/out; }
		cmp -s <(dd if=$FAKE/dev/block/sda33 bs=1048576 skip=$((OSCRYPTEX_FIRST / 256)) count=$((OSCRYPTEX_BLOCKS / 256)) 2>/dev/null) $OS_IMG &&
			ok "the OS cryptex is where root ends (block $OSCRYPTEX_FIRST)" || bad "the OS cryptex is not where root ends"
	fi
	expect yes "flash boot (sde49): saved, written, read back" env BACKUPS=$T/bkv FLASH_BOOT=yes $S flash $T/new.img
	expect yes "rollback boot" env BACKUPS=$T/bkv FLASH_BOOT=yes $S rollback
	expect yes "verify: Android's boot again" env BACKUPS=$T/bkv $S verify $T/android-boot.img
	portimg $T/rec-ok.img "ufs=2 flash_stage=1 flash_stage_target=recovery"
	expect yes "flash the recovery (sda28): saved, written, read back" env BACKUPS=$T/bkvr FLASH_RECOVERY=yes $S --part recovery flash $T/rec-ok.img
	expect yes "rollback the recovery" env BACKUPS=$T/bkvr FLASH_RECOVERY=yes $S --part recovery rollback
	expect yes "verify: the recovery as it was" env BACKUPS=$T/bkvr $S --part recovery verify $T/pbrp.img
else
	echo "== vayu's own numbers: skipped (no GPT_DIR with gpt-sda.head.bin and gpt-sde.head.bin)"
fi

[ ! -e $T/phone/exec-in-used ] && ok "no write went through adb exec-in" || bad "adb exec-in was used: $(cat $T/phone/exec-in-used)"
[ $fails = 0 ] && echo "install-test: PASS" || { echo "install-test: $fails FAILED"; exit 1; }
