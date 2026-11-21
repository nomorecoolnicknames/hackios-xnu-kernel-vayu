#!/bin/bash
# qemu-vayu.sh KERNEL [SECS] [CMDLINE]: boot an xnu-12377 VAYU kernel in QEMU 9.2 (virt, GICv3, cortex-a76, 1 CPU) with Scion's
# xnuloader (scion-kernel 1950dc78, platform qemu-virt: ADT, virtual-timer-intid 27, PL011) and legacy's hello as the ramdisk.
# Serial log: qemu-vayu.log next to this script.
T=$(cd "$(dirname "$0")" && pwd); K=$(readlink -f "$1"); SECS=${2:-90}; CMD=${3:-"cpus=1 -v serial=3 debug=0x8"}
# env: QEMU_PREFIX (where QEMU 9.2 is installed, default /usr), USERLAND (a directory with the hello ramdisk)
Q=${QEMU_PREFIX:-/usr}; L=$T/sk-loader/xnuloader; U=${USERLAND:?USERLAND= a directory with hello, the ramdisk}
W=$(mktemp -d /tmp/qemu-vayu.XXXXXX); mkdir -p $W/esp/EFI/BOOT
cp $L/loader.efi $W/esp/EFI/BOOT/BOOTAA64.EFI; cp $Q/share/qemu/edk2-arm-vars.fd $W/vars.fd
python3 $L/mkpayload.py --no-version -o $W/payload.bin kernel=$K ramdisk=$U/hello --cmdline "$CMD" >/dev/null || exit 1
timeout $SECS $Q/bin/qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 6G -accel tcg,tb-size=384 \
  -drive if=pflash,format=raw,readonly=on,file=$Q/share/qemu/edk2-aarch64-code.fd -drive if=pflash,format=raw,file=$W/vars.fd \
  -drive file=fat:rw:$W/esp,format=raw,if=virtio -device loader,file=$W/payload.bin,addr=0xF0000000,force-raw=on \
  ${QEMU_EXTRA:-} -display none -serial file:$W/serial.log ${QEMU_MON:--monitor none} -no-reboot </dev/null
cp $W/serial.log $T/qemu-vayu.log; rm -rf $W
strings $T/qemu-vayu.log | grep -a -E "XNULOADER|^platform|Darwin Kernel Version|VAYU|BSD root|Hello from user space|panic|PE_init|arm_init" | head -30 | cut -c1-200
echo "qemu-vayu: $(wc -l < $T/qemu-vayu.log) lines in $T/qemu-vayu.log"
