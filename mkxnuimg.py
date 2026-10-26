#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see devices/poco-x5-pro/uefi/README.md)
or, when there is none, from devices/poco-x5-pro/prebuilt/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
# another phone: DEVICE=<name> picks devices/<name>/, whose boot.conf (KEY=VALUE lines) may set
# UEFI_TARGET (the Mu-Silicium platform), FD_BASE and FD_SIZE; the defaults are the POCO X5 Pro's
DEVICE = os.environ.get('DEVICE', 'poco-x5-pro')
DEVDIR = os.path.join(HERE, 'devices', DEVICE)
PREBUILT = os.path.join(DEVDIR, 'prebuilt')
conf = {'UEFI_TARGET': 'redwood', 'FD_BASE': '0x9FD00000', 'FD_SIZE': '0x200000'}
if os.path.isfile(os.path.join(DEVDIR, 'boot.conf')):
    for line in open(os.path.join(DEVDIR, 'boot.conf')):
        line = line.split('#')[0].strip()
        if '=' in line:
            k, v = line.split('=', 1)
            conf[k.strip()] = v.strip()
UEFI_TARGET = conf['UEFI_TARGET']
FD_BASE, FD_SIZE = int(conf['FD_BASE'], 0), int(conf['FD_SIZE'], 0)
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.path.join(MU, f'Build/{UEFI_TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{UEFI_TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
    # the prebuilt BootShim.bin is assembled for the POCO X5 Pro's FD_BASE/FD_SIZE; with another
    # phone's UEFI it would copy the FD to the wrong place and the phone would fall through to Android
    if (FD_BASE, FD_SIZE) != (0x9FD00000, 0x200000) and not os.path.isfile(os.path.join(PREBUILT, 'BootShim.bin')):
        sys.exit(f'mkxnuimg: no Mu-Silicium/BootShim and no {PREBUILT}/BootShim.bin for FD_BASE={FD_BASE:#x} '
                 f'FD_SIZE={FD_SIZE:#x}: build the UEFI first (devices/{DEVICE}/uefi/)')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
# mtime=0: gzip otherwise stores the current time, so the boot image would
# change on every build even when every input is identical.
open(kernel, 'wb').write(gzip.compress(blob, 6, mtime=0) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
