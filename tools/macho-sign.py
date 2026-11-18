#!/usr/bin/env python3
"""macho-sign.py MACHO [ENTITLEMENTS.plist] [--id IDENTIFIER] [--no-der]: an ad hoc code signature, with entitlements, in place.

What ldid -S does, for a thin arm64 Mach-O that the linker already gave a signature (link with -Wl,-adhoc_codesign: that
makes the room, LC_CODE_SIGNATURE at the end of __LINKEDIT): the signature is replaced by a SuperBlob of a CodeDirectory
(SHA-256 of every 4 KiB page up to the signature, special slots for the requirements and both entitlements blobs), an
empty requirements set, the entitlements as XML and as DER. LC_CODE_SIGNATURE and __LINKEDIT grow to hold it.

vayu's kernel (xnu/files/bsd/kern/kern_minimac.c, libkern/amfi/miniamfi_entitlements.cpp) takes an ad hoc signed
program for a platform binary and reads its entitlements from this blob; nothing vouches for them. No tool in this
tree did this before (no ldid in WSL; xnu-deps' codesign is a stub).

From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023.
"""
import hashlib
import plistlib
import struct
import sys

LC_SEGMENT_64, LC_CODE_SIGNATURE = 0x19, 0x1d
PAGE = 4096


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def der(tag, body):
    return bytes([tag]) + der_len(len(body)) + body


def der_tag(tagnum, constructed, cls, body):
    """an identifier for tag numbers past 30 (the entitlements' [APPLICATION 16] and [CONTEXT 16] are below)"""
    first = cls | (0x20 if constructed else 0)
    if tagnum < 31:
        return bytes([first | tagnum]) + der_len(len(body)) + body
    raise ValueError('tag')


def der_value(v):
    if isinstance(v, bool):
        return der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        n = v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big', signed=True)
        return der(0x02, n)
    if isinstance(v, str):
        return der(0x0c, v.encode())
    if isinstance(v, list):
        return der(0x30, b''.join(der_value(x) for x in v))
    if isinstance(v, dict):
        return der_tag(16, True, 0x80, b''.join(der(0x30, der(0x0c, k.encode()) + der_value(v[k])) for k in sorted(v)))
    raise ValueError(f'entitlement value {v!r}')


def der_entitlements(d):
    # [APPLICATION 16] { INTEGER 1, [CONTEXT 16] { SEQUENCE { key, value } ... } }
    return der_tag(16, True, 0x40, der(0x02, b'\x01') + der_value(d))


def blob(magic, body):
    return struct.pack('>II', magic, 8 + len(body)) + body


def main():
    args = sys.argv[1:]
    ident = None
    with_der = '--no-der' not in args
    args = [a for a in args if a != '--no-der']
    if '--id' in args:
        i = args.index('--id')
        ident = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    ents = plistlib.load(open(args[1], 'rb')) if len(args) > 1 else {}
    data = bytearray(open(path, 'rb').read())
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf:
        sys.exit(f'{path}: not a thin 64-bit Mach-O')
    ident = ident or path.replace('\\', '/').rsplit('/', 1)[-1]
    off = 32
    cs_off = linkedit_off = None
    text = (0, 0)
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b'\0')
            if name == b'__LINKEDIT':
                linkedit_off = off
            elif name == b'__TEXT':
                fileoff, filesize = struct.unpack_from('<QQ', data, off + 40)
                text = (fileoff, filesize)
        elif cmd == LC_CODE_SIGNATURE:
            cs_off = off
        off += size
    if cs_off is None or linkedit_off is None:
        sys.exit(f'{path}: no LC_CODE_SIGNATURE (link with -Wl,-adhoc_codesign)')
    dataoff, _old = struct.unpack_from('<II', data, cs_off + 8)
    code_limit = dataoff

    xml = plistlib.dumps(ents, fmt=plistlib.FMT_XML)
    ent_blob = blob(0xfade7171, xml) if ents else b''
    der_blob = blob(0xfade7172, der_entitlements(ents)) if ents and with_der else b''
    req_blob = struct.pack('>III', 0xfade0c01, 12, 0)
    n_code = (code_limit + PAGE - 1) // PAGE
    n_special = 7
    ident_b = ident.encode() + b'\0'
    cd_header = 88   # version 0x20400: through execSegFlags
    hash_off = cd_header + len(ident_b) + n_special * 32
    cd_len = hash_off + n_code * 32
    nblobs = 2 + (1 if ents else 0) + (1 if der_blob else 0)
    total = 12 + 8 * nblobs + cd_len + len(req_blob) + len(ent_blob) + len(der_blob)
    sig_size = (total + 15) & ~15

    # the header first (page 0 is hashed): the signature's size and __LINKEDIT's
    struct.pack_into('<II', data, cs_off + 8, dataoff, sig_size)
    vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, linkedit_off + 24)
    filesize = dataoff + sig_size - fileoff
    vmsize = (filesize + 0x3fff) & ~0x3fff
    struct.pack_into('<QQQQ', data, linkedit_off + 24, vmaddr, vmsize, fileoff, filesize)
    del data[dataoff:]

    special = [b'\0' * 32] * n_special          # slot -1 .. -7
    special[2 - 1] = hashlib.sha256(req_blob).digest()
    if ents:
        special[5 - 1] = hashlib.sha256(ent_blob).digest()
        if der_blob:
            special[7 - 1] = hashlib.sha256(der_blob).digest()
    hashes = b''.join(hashlib.sha256(bytes(data[i:min(i + PAGE, code_limit)])).digest() for i in range(0, code_limit, PAGE))
    exec_flags = 1 if ftype == 2 else 0          # CS_EXECSEG_MAIN_BINARY for an executable
    cd = struct.pack('>IIIIIIIIIBBBBIIIIQQQQ', 0xfade0c02, cd_len, 0x20400, 0x2, hash_off, cd_header, n_special, n_code,
                     code_limit, 32, 2, 0, 12, 0, 0, 0, 0, 0, text[0], text[1], exec_flags)
    assert len(cd) == cd_header, len(cd)
    cd += ident_b + b''.join(reversed(special)) + hashes
    parts = [(0, cd), (2, req_blob)] + ([(5, ent_blob)] if ents else []) + ([(7, der_blob)] if der_blob else [])
    index, body = b'', b''
    at = 12 + 8 * len(parts)
    for kind, b in parts:
        index += struct.pack('>II', kind, at + len(body))
        body += b
    sb = struct.pack('>III', 0xfade0cc0, 12 + len(index) + len(body), len(parts)) + index + body
    data += sb + b'\0' * (sig_size - len(sb))
    open(path, 'wb').write(data)
    print(f'{path}: signed ad hoc as {ident}, {n_code} pages, {len(ents)} entitlements ({sig_size} bytes)')


if __name__ == '__main__':
    main()
