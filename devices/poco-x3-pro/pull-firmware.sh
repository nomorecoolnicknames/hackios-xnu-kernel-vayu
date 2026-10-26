#!/bin/bash
# Pull the phone's own firmware out of its Android into ../../fw-poco-x3-pro/ (never committed: it is the
# vendor's). The phone is on adb, in Android. userland/build.sh (DEVICE=poco-x3-pro) copies that directory
# flat into the image's fw/, where the vayu_* drivers look for it. Missing files are told, not fatal; the
# paths are those of the stock vayu ROM and are the first thing to check if a file is not found.
set -e
cd "$(dirname "$0")/../.."
OUT=fw-poco-x3-pro
mkdir -p $OUT
adb get-state >/dev/null
pull() { # SOURCE...
	for f in "$@"; do adb pull "$f" "$OUT/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on the phone"; done
}
find_pull() { # NAME: wherever the phone keeps it under /vendor
	local p
	p=$(adb shell "find /vendor -name '$1' 2>/dev/null | head -1" | tr -d '\r')
	if [ -n "$p" ]; then adb pull "$p" "$OUT/" >/dev/null 2>&1; else echo "pull-firmware.sh: $1: not found under /vendor"; fi
}
# GPU: GMU, command-processor and the signed zap shader (vayu_adreno.c, "gpu=1 vayu_gpu=1")
pull /vendor/firmware/a630_sqe.fw /vendor/firmware/a640_gmu.bin /vendor/firmware/a640_zap.mdt \
	$(for i in 00 01 02; do echo /vendor/firmware/a640_zap.b$i; done)
# touch: the Novatek NT36675 is flashless, XNU downloads its firmware at every start (vayu_nvt.c)
pull /vendor/firmware/j20s_novatek_ts_fw01.bin /vendor/firmware/j20s_novatek_ts_fw02.bin
# the DSPs started by vayu_pas.c: audio, modem, sensors (slpi_vy)
for n in adsp modem slpi_vy; do
	pull /vendor/firmware_mnt/image/$n.mdt $(for i in $(seq -w 0 40); do echo /vendor/firmware_mnt/image/$n.b$i; done)
done
# Wi-Fi runs on the modem's DSP: the image the modem asks for over TFTP, and the board data (vayu_wifi.c)
find_pull wlanmdsp.mbn
find_pull bdwlan.bin
# Bluetooth controller firmware (vayu_bt.c)
find_pull 'crbtfw*.tlv'
find_pull 'crnv*.bin'
echo "pull-firmware.sh: $OUT: $(ls -1 $OUT | wc -l | tr -d ' ') files"
echo "pull-firmware.sh: the camera tables (fw/camera-*.tab) are made from the sensor modules with devices/poco-x3-pro/tools/camtab.py"
