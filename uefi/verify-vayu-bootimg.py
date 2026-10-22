#!/usr/bin/env python3
"""Verify a vayu Android boot image without flashing it.

Checks the boot image header, decompresses the kernel, and confirms that it is
BootShim + SILICIUM_UEFI.fd + XNUP payload, with the vayu DTB appended after
the gzip stream. Also checks the DTB contains cont_splash_region@9c000000.

usage:
  uefi/verify-vayu-bootimg.py out/vayu-boot.img \
      --fd Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
      --dtb uefi/files/Resources/DTBs/vayu.dtb
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

MAGIC = b"ANDROID!"
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def die(msg: str) -> None:
    print(f"verify: FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--fd", type=Path, required=True)
    ap.add_argument("--dtb", type=Path, required=True)
    ap.add_argument("--platform", default="", help="require the XNUP platform entry to be this device")
    ap.add_argument("--fb-node", default="cont_splash_region@9c000000")
    args = ap.parse_args()

    image = args.image.read_bytes()
    fd = args.fd.read_bytes()
    dtb_file = args.dtb.read_bytes()

    if image[:8] != MAGIC:
        die("not an Android boot image (missing ANDROID! magic)")
    if len(image) < 4096:
        die("image shorter than one page")

    kernel_size = struct.unpack_from("<I", image, 8)[0]
    page_size = struct.unpack_from("<I", image, 36)[0]
    header_version = struct.unpack_from("<I", image, 40)[0]
    if page_size == 0:
        die("page size is zero")
    kernel_off = page_size
    kernel = image[kernel_off:kernel_off + kernel_size]
    if len(kernel) != kernel_size:
        die(f"kernel truncated: {len(kernel)} of {kernel_size} bytes")

    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    blob = d.decompress(kernel)
    if not d.eof:
        die("kernel gzip stream is incomplete")
    dtb = d.unused_data
    if not blob:
        die("kernel decompressed to zero bytes")

    idx = blob.find(fd)
    if idx < 0:
        die("the supplied SILICIUM_UEFI.fd is not inside the decompressed kernel")
    shim = blob[:idx]
    after_fd = blob[idx + len(fd):]
    if after_fd[:4] != b"XNUP":
        die(f"payload magic is {after_fd[:4]!r}, expected b'XNUP'")
    # XNUP: 8-byte header (magic + data size), then data, padded to 16 bytes.
    payload_data = struct.unpack_from("<I", after_fd, 4)[0]
    payload_total = (8 + payload_data + 15) & ~15
    if len(after_fd) < payload_total:
        die(f"XNUP payload truncated: {len(after_fd)} of {payload_total} bytes")
    payload = after_fd[:payload_total]

    entries = {}
    pos = 8
    entry_end = 8 + payload_data
    while pos + 24 <= entry_end:
        name, size, _ = struct.unpack_from("<16sII", payload, pos)
        if name[0] == 0:
            break
        entries[name.rstrip(b"\0").decode()] = payload[pos + 24:pos + 24 + size]
        pos += 24 + ((size + 15) & ~15)

    if not dtb.startswith(FDT_MAGIC):
        die(f"appended DTB has magic {dtb[:4]!r}, expected FDT magic")
    if dtb != dtb_file:
        die("appended DTB differs from --dtb")
    with tempfile.NamedTemporaryFile(suffix=".dtb") as f:
        f.write(dtb)
        f.flush()
        try:
            dts = subprocess.check_output(
                ["dtc", "-I", "dtb", "-O", "dts", "-q", f.name],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.CalledProcessError) as e:
            die(f"dtc could not parse the appended DTB: {e}")
    node_ok = args.fb_node in dts

    print(f"image       {args.image} ({len(image)} bytes, sha256 {sha(image)})")
    print(f"header      v{header_version}, page {page_size}, kernel {kernel_size}")
    print(f"decompressed {len(blob)} bytes")
    print(f"shim        {len(shim)} bytes")
    print(f"uefi fd     {len(fd)} bytes (sha256 {sha(fd)})")
    print(f"payload     {len(payload)} bytes, XNUP (data {payload_data})")
    print("entries     " + ", ".join(f"{name}={len(data)}" for name, data in entries.items()))
    print(f"dtb         {len(dtb)} bytes (sha256 {sha(dtb)})")
    print(f"dtb node    {args.fb_node}: {'yes' if node_ok else 'NO'}")
    if not node_ok:
        die(f"DTB does not contain {args.fb_node}")
    if args.platform:
        got = entries.get("platform", b"").rstrip(b"\0").decode()
        if got != args.platform:
            die(f"payload platform is {got!r}, expected {args.platform!r}")
        print(f"platform    {got}")
    print("verify: OK")


if __name__ == "__main__":
    main()
