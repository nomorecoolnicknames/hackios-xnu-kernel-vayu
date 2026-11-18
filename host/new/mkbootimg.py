#!/usr/bin/env python3
"""
Pack an Android boot image, header version 1, the way mkbootimg.py does it.

This is the small, self-contained half of the recipe in host/mkxnuimg.py:
mkbootimg.py only writes the header, the id (sha1 of kernel+ramdisk+second) and
the page-padded blobs.  The kernel itself is already gzip(BootShim + UEFI FD +
XNUP payload) + DTB, as mkxnuimg.py builds it.

usage: mkbootimg.py --kernel K --ramdisk R --header_version 1 -o boot.img
"""
import argparse, hashlib, os, struct

BOOT_MAGIC = b'ANDROID!'
BOOT_NAME_SIZE = 16
BOOT_ARGS_SIZE = 512
BOOT_EXTRA_ARGS_SIZE = 1024
BOOT_IMAGE_HEADER_V1_SIZE = 1648
BOOT_IMAGE_HEADER_V2_SIZE = 1660
BOOT_IMAGE_HEADER_V3_SIZE = 1580
BOOT_IMAGE_HEADER_V4_SIZE = 1584


def filesize(path):
    return os.path.getsize(path) if path else 0


def read(path):
    return open(path, 'rb').read() if path else b''


def pad(data, pagesize):
    return data + b'\0' * (-len(data) % pagesize)


def parse_int(text):
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk', required=True)
    ap.add_argument('--second')
    ap.add_argument('--recovery_dtbo')
    ap.add_argument('--dtb')
    ap.add_argument('--output', '-o', required=True)
    ap.add_argument('--base', type=parse_int, default=0x10000000)
    ap.add_argument('--kernel_offset', type=parse_int, default=0x00008000)
    ap.add_argument('--ramdisk_offset', type=parse_int, default=0x01000000)
    ap.add_argument('--second_offset', type=parse_int, default=0x00f00000)
    ap.add_argument('--tags_offset', type=parse_int, default=0x00000100)
    ap.add_argument('--dtb_offset', type=parse_int, default=0x01f00000)
    ap.add_argument('--pagesize', type=parse_int, default=2048)
    ap.add_argument('--header_version', type=parse_int, default=1)
    ap.add_argument('--os_version', type=parse_int, default=0)
    ap.add_argument('--os_patch_level', type=parse_int, default=0)
    ap.add_argument('--board', default='')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--extra_cmdline', default='')
    args = ap.parse_args()

    kernel = read(args.kernel)
    ramdisk = read(args.ramdisk)
    second = read(args.second)
    recovery_dtbo = read(args.recovery_dtbo)
    dtb = read(args.dtb)

    ramdisk_load_address = ((args.base + args.ramdisk_offset)
                            if len(ramdisk) > 0 else 0)
    second_load_address = ((args.base + args.second_offset)
                           if len(second) > 0 else 0)
    tags_load_address = args.base + args.tags_offset

    out = bytearray()
    out += struct.pack('<8s10I', BOOT_MAGIC, len(kernel),
                       args.base + args.kernel_offset, len(ramdisk),
                       ramdisk_load_address, len(second), second_load_address,
                       tags_load_address, args.pagesize, args.header_version,
                       (args.os_version << 11) | args.os_patch_level)
    out += struct.pack(f'{BOOT_NAME_SIZE}s', args.board.encode())
    out += struct.pack(f'{BOOT_ARGS_SIZE}s', args.cmdline.encode())

    # update_sha() in mkbootimg.py: the bytes and then their size as u32, for
    # every field, including an absent one (four zero bytes).
    sha = hashlib.sha1()
    for data in (kernel, ramdisk, second):
        sha.update(data)
        sha.update(struct.pack('I', len(data)))
    if args.header_version > 0:
        sha.update(recovery_dtbo)
        sha.update(struct.pack('I', len(recovery_dtbo)))
    if args.header_version > 1:
        sha.update(dtb)
        sha.update(struct.pack('I', len(dtb)))
    out += struct.pack('32s', sha.digest())
    out += struct.pack(f'{BOOT_EXTRA_ARGS_SIZE}s', args.extra_cmdline.encode())

    if args.header_version > 0:
        out += struct.pack('I', len(recovery_dtbo))
        out += struct.pack('Q', 0)
    if args.header_version == 1:
        out += struct.pack('I', BOOT_IMAGE_HEADER_V1_SIZE)
    elif args.header_version == 2:
        out += struct.pack('I', BOOT_IMAGE_HEADER_V2_SIZE)
    elif args.header_version == 3:
        out += struct.pack('I', BOOT_IMAGE_HEADER_V3_SIZE)
    elif args.header_version == 4:
        out += struct.pack('I', BOOT_IMAGE_HEADER_V4_SIZE)

    image = pad(bytes(out), args.pagesize)
    image += pad(kernel, args.pagesize)
    image += pad(ramdisk, args.pagesize)
    image += pad(second, args.pagesize)
    if 0 < args.header_version < 3:
        image += pad(recovery_dtbo, args.pagesize)
    if args.header_version == 2:
        image += pad(dtb, args.pagesize)

    with open(args.output, 'wb') as f:
        f.write(image)
    print(f'{args.output}: {len(image)} bytes')


if __name__ == '__main__':
    main()
