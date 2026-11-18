#!/bin/bash
# Rebuild a boot image in the format of host/boot-test4.img, replacing only the
# loader.efi in its XNUP payload.  The recipe is exactly mkxnuimg.py's:
#
#   kernel = gzip(BootShim.bin + SILICIUM_UEFI.fd + XNUP[loader.efi,usb,platform]) + DTB
#   boot   = mkbootimg.py --kernel kernel --ramdisk dummy --header_version 1
#
# The UEFI pieces are taken from the base image, because this phone's prebuilt
# directory has only device-tree.dtb (no BootShim.bin / SILICIUM_UEFI.fd): the
# base image already carries the right ones for FD_BASE=0x9FC00000, FD_SIZE=0x300000.
#
# usage: mkboot-usbctl.sh [BASE_IMG [LOADER_EFI [OUT_IMG]]]
#   BASE_IMG    default host/boot-test4.img
#   LOADER_EFI  default artifacts/loader-2.3-usbctl.efi
#   OUT_IMG     default host/new/boot-usbctl.img (or stdout name when OUT=-)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASE=${1:-$ROOT/host/boot-test4.img}
LOADER=${2:-$ROOT/artifacts/loader-2.3-usbctl.efi}
OUT=${3:-$ROOT/host/new/boot-usbctl.img}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 -I - "$BASE" "$TMP" <<'PY'
import struct, sys, zlib
base, tmp = sys.argv[1], sys.argv[2]
d = open(base, 'rb').read()
magic, ks, ka, rs, ra, ss, sa, ta, ps, hv, ov = struct.unpack_from('<8s10I', d, 0)
assert magic == b'ANDROID!' and hv == 1, (magic, hv)
kernel = d[ps:ps + ks]
z = zlib.decompressobj(16 + zlib.MAX_WBITS)
blob = z.decompress(kernel)
dtb = z.unused_data
assert dtb[:4] == b'\xd0\x0d\xfe\xed'
fv = blob.find(b'_FVH') - 0x28
assert fv > 0 and blob[fv + 0x28:fv + 0x2c] == b'_FVH'
fvlen, = struct.unpack_from('<I', blob, fv + 0x20)
shim, fd, xnup = blob[:fv], blob[fv:fv + fvlen], blob[fv + fvlen:]
assert xnup[:4] == b'XNUP'
size, = struct.unpack_from('<I', xnup, 4)
p = 8
ent = {}
while True:
    name, n, _ = struct.unpack_from('<16sII', xnup, p)
    if not name.strip(b'\0'):
        break
    ent[name.rstrip(b'\0').decode()] = xnup[p + 24:p + 24 + n]
    p += 24 + n + (-n % 16)
assert 'loader.efi' in ent, ent.keys()
roff = ps + ((ks + ps - 1) // ps) * ps
ramdisk = d[roff:roff + rs]
open(tmp + '/shim.bin', 'wb').write(shim)
open(tmp + '/fd.bin', 'wb').write(fd)
open(tmp + '/dtb.bin', 'wb').write(dtb)
open(tmp + '/ramdisk.bin', 'wb').write(ramdisk)
open(tmp + '/usb.txt', 'w').write('1' if ent.get('usb', b'')[:1] == b'1' else '0')
open(tmp + '/platform.txt', 'w').write(ent.get('platform', b'vayu').decode())
print(f'base: shim {len(shim)}, fd {len(fd)}, dtb {len(dtb)}, ramdisk {len(ramdisk)}, '
      f'payload {len(xnup[:p + 24])}, usb {open(tmp + "/usb.txt").read()}, '
      f'platform {open(tmp + "/platform.txt").read()}')
PY

USBP=$(cat "$TMP/usb.txt")
PLATFORM=$(cat "$TMP/platform.txt")
PAYLOAD_ARGS=(--platform "$PLATFORM")
if [ "$USBP" = 1 ]; then
    PAYLOAD_ARGS+=(--usb)
fi

python3 -I "$ROOT/host/mkpayload.py" -o "$TMP/payload.bin" \
    "${PAYLOAD_ARGS[@]}" "loader.efi=$LOADER"

python3 -I - "$TMP" <<'PY'
import gzip, sys
tmp = sys.argv[1]
shim = open(tmp + '/shim.bin', 'rb').read()
fd = open(tmp + '/fd.bin', 'rb').read()
payload = open(tmp + '/payload.bin', 'rb').read()
blob = shim + fd + payload
blob += b'\0' * (-len(blob) % 16)
dtb = open(tmp + '/dtb.bin', 'rb').read()
open(tmp + '/kernel.bin', 'wb').write(gzip.compress(blob, 6, mtime=0) + dtb)
print(f'rebuilt: shim {len(shim)}, fd {len(fd)}, payload {len(payload)}, '
      f'blob {len(blob)}, kernel {len(blob)} + dtb {len(dtb)}')
PY

python3 -I "$HERE/mkbootimg.py" --kernel "$TMP/kernel.bin" \
    --ramdisk "$TMP/ramdisk.bin" --header_version 1 -o "$OUT"

sha256sum "$OUT"
