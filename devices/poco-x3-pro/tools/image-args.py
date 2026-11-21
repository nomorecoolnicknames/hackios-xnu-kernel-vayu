#!/usr/bin/env python3
"""The kernel's command line of a standalone boot image, as the loader will read it.

mkxnuimg.py puts the payload inside the Android boot image's kernel, gzipped (kernel = gzip(BootShim + UEFI FD +
"XNUP" container) + DTB), so the arguments are not in the image's bytes as text: `grep -a flash_stage=1 IMAGE`
finds nothing in a real one. This takes the kernel out (header version 0..2: page size at 36, kernel size at 8,
the kernel from the first page on), ungzips it, finds the "XNUP" container (xnuloader/mkpayload.py: u32 magic,
u32 size; entries of char name[16], u32 size, u32 reserved, data padded to 16; an empty name ends them) and prints
its "cmdline" entry.

  image-args.py IMAGE            the command line (exit 0), or why there is none (exit 1, on stderr)
  image-args.py IMAGE WORD...    exit 0 if every WORD is one of its words, 1 if not
"""
import struct
import sys
import zlib


def payload_entries(blob, at):
    """the entries of an XNUP container at `at`, or None if it is not one that holds together"""
    if blob[at:at + 4] != b'XNUP':
        return None
    size, = struct.unpack_from('<I', blob, at + 4)
    end, p, out = at + 8 + size, at + 8, {}
    if end > len(blob):
        return None
    while p + 24 <= end:
        name, n, _ = struct.unpack_from('<16sII', blob, p)
        if name == bytes(16):
            return out
        if name[15] != 0 or p + 24 + n > end:
            return None
        out[name.split(b'\0')[0].decode('ascii', 'replace')] = blob[p + 24:p + 24 + n]
        p += 24 + n + (-n % 16)
    return None


def cmdline(path):
    d = open(path, 'rb').read()
    if d[:8] != b'ANDROID!':
        raise ValueError('not an Android boot image')
    ksize, = struct.unpack_from('<I', d, 8)
    page, = struct.unpack_from('<I', d, 36)
    if page not in (2048, 4096, 16384) or page + ksize > len(d):
        raise ValueError('its header says no kernel that fits')
    kernel = d[page:page + ksize]
    if kernel[:2] != b'\x1f\x8b':
        raise ValueError('its kernel is not gzipped (not an image of mkxnuimg.py)')
    blob = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(kernel)
    at = blob.find(b'XNUP')
    while at >= 0:
        e = payload_entries(blob, at)
        if e is not None:
            if 'cmdline' not in e:
                raise ValueError('its payload has no command line')
            return e['cmdline'].split(b'\0')[0].decode('ascii', 'replace')
        at = blob.find(b'XNUP', at + 1)
    raise ValueError('no XNUP payload in its kernel')


def main(argv):
    if not argv:
        sys.exit(__doc__)
    try:
        args = cmdline(argv[0])
    except (OSError, ValueError, zlib.error) as e:
        print(f'image-args: {argv[0]}: {e}', file=sys.stderr)
        return 1
    if len(argv) == 1:
        print(args)
        return 0
    words = args.split()
    return 0 if all(w in words for w in argv[1:]) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
