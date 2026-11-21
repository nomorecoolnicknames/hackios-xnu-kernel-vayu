#!/bin/bash
# The host's half of "phone-mode.sh deploy" (devices/poco-x3-pro/tools/phone-mode.sh) against a stand-in phone
# (fake_phone.py: the command port, sweetd and the effects of flash_stage and of the loader's staged flash, on
# 127.0.0.1 and a scratch directory). The phone's own halves have their tests elsewhere: flash_stage.h in
# tools/flash-stage-test, the loader's staged flash in qemu/run-standalone-test.sh (A, B, C, K, L).
#
#   tools/deploy-test/run.sh
# Prints "deploy-test: PASS" when every check holds.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
W=$(mktemp -d "${TMPDIR:-/tmp}/deploy-test.XXXXXX")
PH=
trap '[ -n "$PH" ] && kill $PH 2>/dev/null; rm -rf "$W"' EXIT
fails=0
check() { local what=$1; shift; if "$@"; then echo "  ok   $what"; else echo "  FAIL $what"; fails=$((fails + 1)); fi; }
TOKEN=0123456789abcdef0123456789abcdef
echo "$TOKEN" > $W/token
CP=$((20000 + RANDOM % 20000)); SP=$((CP + 1))
export IP=127.0.0.1 PORT=$CP SWEETD_PORT=$SP SWEETD_TOKEN=$W/token DARWIN_PROFILE=qemu WAIT=30 DOWN_WAIT=10 SERIAL= PHONE_MODE_LOG=$W/phone-mode.log
eval "$(python3 -I devices/poco-x3-pro/tools/darwin-layout.py --profile qemu sh)"   # BOOT_BYTES of the test disk
PM=devices/poco-x3-pro/tools/phone-mode.sh
sha() { sha256sum "$1" | cut -d' ' -f1; }

phone() {       # phone MODE: a fresh stand-in, boot.img an "old" image
	[ -n "$PH" ] && kill $PH 2>/dev/null; wait $PH 2>/dev/null
	rm -rf $W/phone; mkdir -p $W/phone/root/private/var/tmp
	{ printf 'ANDROID!'; head -c $((BOOT_BYTES - 8)) /dev/zero | tr '\0' o; } > $W/phone/boot.img
	cp $W/phone/boot.img $W/old-boot.img
	{ printf 'ANDROID!'; head -c $((RECOVERY_BYTES - 8)) /dev/zero | tr '\0' r; } > $W/phone/recovery.img
	cp $W/phone/recovery.img $W/old-recovery.img
	python3 -I tools/deploy-test/fake_phone.py $W/phone $CP $SP $TOKEN "$1" & PH=$!
	for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CP) 2>/dev/null && break; sleep 0.1; done
	export BACKUPS=$W/backups-$1
}
deploy() { (cd $W && bash "$OLDPWD/$PM" deploy "$@") > $W/out.txt 2>&1; }
says() { grep -q -- "$1" $W/out.txt; }
boot_is() { cmp -s $W/phone/boot.img "$1"; }
boot_begins() { cmp -s <(head -c "$(stat -c %s "$1")" $W/phone/boot.img) "$1"; }
no_head() { [ ! -e $W/phone/stage.head ]; }
saved_is_old() { local f; f=$(ls -1t $BACKUPS/boot-*.img 2>/dev/null | head -1); [ -n "$f" ] && cmp -s "$f" $W/old-boot.img &&
	[ "$(basename "$f" .img | sed 's/.*-//')" = "$(sha $W/old-boot.img | cut -c1-16)" ]; }

# images as mkxnuimg.py makes them: the command line inside the gzipped kernel (image-args.py reads it)
portimg() {     # portimg OUT ARGS
	python3 -I - "$1" "$2" <<'PY'
import gzip, os, struct, sys
out, args = sys.argv[1], sys.argv[2].encode() + b'\0'
def entry(name, data):
    return struct.pack('<16sII', name, len(data), 0) + data + bytes(-len(data) % 16)
body = entry(b'kernel', os.urandom(300000)) + entry(b'cmdline', args) + bytes(24)
kernel = gzip.compress(b'shim' + struct.pack('<II', 0x50554E58, len(body)) + body, 6, mtime=0) + bytes(64)
h = struct.pack('<8s10I', b'ANDROID!', len(kernel), 0x8000, 0, 0x1000000, 0, 0, 0x100, 4096, 1, 0)
open(out, 'wb').write(h + bytes(4096 - len(h)) + kernel)
PY
}
portimg $W/new.img "usbnet_cmd=1 bcb_reboot=1 flash_stage=1 sweetd=1 usbnet_log=1"
portimg $W/blind.img "flash_stage=1"
portimg $W/new-rec.img "usbnet_cmd=1 bcb_reboot=1 flash_stage=1 flash_stage_target=recovery sweetd=1 usbnet_log=1"
python3 -I -c 'import os,sys; open(sys.argv[1],"wb").write(b"ANDROID!" + os.urandom(300000) + b" usbnet_cmd=1 bcb_reboot=1 flash_stage=1 ")' $W/raw.img

echo "== 1: a deploy that goes"
phone ok
FLASH_BOOT=yes deploy $W/new.img
check "1: said so" says "deploy: boot holds"
check "1: boot begins with the image" boot_begins $W/new.img
check "1: the rest of boot as it was" eval 'cmp -s <(tail -c +$(( $(stat -c %s $W/new.img) + 1 )) $W/phone/boot.img) <(tail -c +$(( $(stat -c %s $W/new.img) + 1 )) $W/old-boot.img)'
check "1: boot as it was saved, named by its sum" saved_is_old
check "1: no header left" no_head
check "1: the image's copy gone from the phone" eval '[ ! -e $W/phone/root/private/var/tmp/hakios-deploy.img ]'
check "1: the phone restarted once" eval '[ "$(grep -c "^reboot" $W/phone/phone.log)" = 1 ]'

echo "== 2: without FLASH_BOOT=yes"
phone ok
deploy $W/new.img
check "2: refused" says "FLASH_BOOT=yes is not set"
check "2: boot untouched" boot_is $W/old-boot.img
check "2: nothing asked of the phone" eval '[ ! -s $W/phone/phone.log ]'

echo "== 3: an image without the way back out (usbnet_cmd=1, bcb_reboot=1)"
phone ok
FLASH_BOOT=yes deploy $W/blind.img
check "3: refused" says "does not say usbnet_cmd=1"
check "3: boot untouched" boot_is $W/old-boot.img

echo "== 4: the stage fails"
phone stagefail
FLASH_BOOT=yes deploy $W/new.img
check "4: refused" says "not staged: nothing will be flashed"
check "4: cancelled" eval 'grep -q "flash_stage cancel" $W/phone/phone.log'
check "4: no restart" eval '! grep -q "^reboot" $W/phone/phone.log'
check "4: the image's copy gone" eval '[ ! -e $W/phone/root/private/var/tmp/hakios-deploy.img ]'
check "4: boot untouched" boot_is $W/old-boot.img

echo "== 5: the loader does not write it"
phone noflash
FLASH_BOOT=yes deploy $W/new.img
check "5: said so" says "boot is as it was"
check "5: boot untouched" boot_is $W/old-boot.img
check "5: no header left" no_head

echo "== 6: the phone never goes down"
phone nodown
FLASH_BOOT=yes deploy $W/new.img
check "6: the stage cancelled when it is seen still up" eval 'no_head'
check "6: boot untouched" boot_is $W/old-boot.img
check "6: not called a success" eval '! says "deploy: boot holds"'

echo "== 7: an image that says flash_stage_target=recovery, and the phone's running one says the same (the multiboot)"
phone ok-recovery
FLASH_RECOVERY=yes deploy $W/new-rec.img
check "7: said so" says "deploy: recovery holds"
check "7: the recovery begins with the image" eval 'cmp -s <(head -c "$(stat -c %s $W/new-rec.img)" $W/phone/recovery.img) $W/new-rec.img'
check "7: boot (Android's) untouched" boot_is $W/old-boot.img
check "7: the recovery as it was saved, as recovery-*.img" eval 'f=$(ls $BACKUPS/recovery-*.img 2>/dev/null | head -1); [ -n "$f" ] && cmp -s "$f" $W/old-recovery.img'
check "7: no header left" no_head

echo "== 8: the phone runs from the recovery, the image is one for boot"
phone ok-recovery
FLASH_BOOT=yes FLASH_RECOVERY=yes deploy $W/new.img
check "8: refused" says "stages into recovery, and .* says boot"
check "8: boot untouched" boot_is $W/old-boot.img
check "8: the recovery untouched" eval 'cmp -s $W/phone/recovery.img $W/old-recovery.img'
check "8: nothing staged" eval '! grep -q "flash_stage stage" $W/phone/phone.log'

echo "== 9: the phone runs from boot, the image is one for the recovery"
phone ok
FLASH_BOOT=yes FLASH_RECOVERY=yes deploy $W/new-rec.img
check "9: refused" says "stages into boot, and .* says recovery"
check "9: boot untouched" boot_is $W/old-boot.img

echo "== 10: an image for the recovery with FLASH_BOOT=yes alone"
phone ok-recovery
FLASH_BOOT=yes deploy $W/new-rec.img
check "10: refused" says "FLASH_RECOVERY=yes is not set"
check "10: nothing asked of the phone" eval '[ ! -s $W/phone/phone.log ]'

echo "== 11: an image whose command line cannot be read (not one of mkxnuimg.py's)"
phone ok
FLASH_BOOT=yes deploy $W/raw.img
check "11: refused" says "command line could not be read"
check "11: boot untouched" boot_is $W/old-boot.img

[ $fails = 0 ] && echo "deploy-test: PASS" || { echo "deploy-test: $fails FAILED"; exit 1; }
