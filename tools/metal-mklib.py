#!/usr/bin/env python3
"""metal-mklib.py OUT.metallib NAME=TYPE:FILE.bc[+CONST:TYPE:INDEX:REQUIRED...] ... : a Metal library made on the PC from
AIR bitcode (vayu task 9).

There is no Apple compiler here. The shaders are the hand-written AIR of hakios-metal/tools/kernels/*.ll (llvm-as
turns each into a module), and this puts them into the MTLB container iOS 16.5's Metal reads itself in
-newLibraryWithData: / -newLibraryWithURL: -- the layout of tools/merge_metallib.py (upstream HakiOS), version 1.2
as the image's own files have it:

  0x00  "MTLB", u16 1, u16 2, then the platform/type bytes of AppleMetalGLRenderer.bundle/default.metallib
  0x10  total size; +0x18 the function list's offset and the size of its entries; +0x28/+0x38/+0x48 offset and
        size of the public, private and bitcode sections
  0x58  u32 function count, then each entry: u32 size (counting itself), tags NAME TYPE HASH OFFT VERS MDSZ RFLT
        ENDT (tag = 4 bytes, u16 length, value)
        the header extension: RLST (the reflection list's absolute offset and size), UUID, ENDT
        the public, private and bitcode sections, then the reflection list

Each function's private metadata is the empty tag list "u32 8, ENDT" -- what the image's own functions carry
(AppleMetalGLRenderer's clearVertex, for one) -- and so is its public metadata unless it declares function constants:
then one CNST tag, as AV1SW.metallib's BlitLumaKernel_Uint has it: a u16 count, then per constant its name (NUL
ended), its MTLDataType (a byte), its index (u16) and a byte that is 1 for a constant the function reads without
asking is_function_constant_defined (one the pipeline must supply) and 0 for a guarded one. Metal reads these for
-functionConstantsDictionary and checks -newFunctionWithName:constantValues: against them; without them a
function's constants are silently not passed (task 9's first phone run: tri_fc_fs drew "nothing supplied"). Reflection entries are
the same empty list. TYPE: vertex 0, fragment 1, kernel 2. HASH: SHA-256 of the module. VERS: AIR 2.5, Metal 3.0,
what the modules' own !air.version / !air.language_version say. Each module goes in LLVM's 0x0B17C0DE wrapper, the
form every module of the image's libraries is in.

Hand-written, MIT, no byte of Apple's: only the shape of the container, read off the image's files.

From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023.
"""
import hashlib
import struct
import sys
import uuid

TYPES = {'vertex': 0, 'fragment': 1, 'kernel': 2}
# MTLDataType of the constants this packer is asked to declare
DATA_TYPES = {'float': 3, 'half': 16, 'int': 29, 'uint': 33, 'short': 37, 'ushort': 41, 'char': 45, 'uchar': 49, 'bool': 53}
# the image's AppleMetalGLRenderer.bundle/default.metallib: "MTLB" 01 00 02 00, then 07 00 00 82 10 00 05 00
HEAD = b'MTLB' + bytes.fromhex('0100020007000082') + bytes.fromhex('10000500')
EMPTY = struct.pack('<I', 8) + b'ENDT'


def tag(name, value):
    return name + struct.pack('<H', len(value)) + value


def wrap(bc):
    """LLVM's bitcode wrapper: magic, version 0, offset 0x14, size, cpu type -1 (as the image's modules have it)."""
    if bc[:4] == b'\xde\xc0\x17\x0b':
        return bc
    assert bc[:4] == b'BC\xc0\xde', 'not LLVM bitcode'
    return struct.pack('<IIIIi', 0x0B17C0DE, 0, 0x14, len(bc), -1) + bc


def build(funcs, out):
    pub = prv = bc = refl = b''
    entries = []
    for name, kind, module, constants in funcs:
        tags = [
            tag(b'NAME', name.encode() + b'\0'),
            tag(b'TYPE', bytes([TYPES[kind]])),
            tag(b'HASH', hashlib.sha256(module).digest()),
            tag(b'OFFT', struct.pack('<QQQ', len(pub), len(prv), len(bc))),
            tag(b'VERS', struct.pack('<HHHH', 2, 5, 3, 0)),
            tag(b'MDSZ', struct.pack('<Q', len(module))),
            tag(b'RFLT', struct.pack('<Q', 4 + len(refl))),
        ]
        body = b''.join(tags) + b'ENDT'
        entries.append(struct.pack('<I', 4 + len(body)) + body)
        if constants:
            cnst = struct.pack('<H', len(constants))
            for cname, ctype, index, required in constants:
                cnst += cname.encode() + b'\0' + bytes([DATA_TYPES[ctype]]) + struct.pack('<H', index) + bytes([required])
            public = tag(b'CNST', cnst) + b'ENDT'
            pub += struct.pack('<I', 4 + len(public)) + public
        else:
            pub += EMPTY
        prv += EMPTY
        bc += module
        refl += EMPTY
    funcs_off = 0x58
    listing = struct.pack('<I', len(funcs)) + b''.join(entries)
    ext_len = (6 + 16) + (6 + 16) + 4
    pub_off = funcs_off + len(listing) + ext_len
    prv_off = pub_off + len(pub)
    bc_off = prv_off + len(prv)
    refl_off = bc_off + len(bc)
    reflection = struct.pack('<I', len(funcs)) + refl
    total = refl_off + len(reflection)
    ext = (tag(b'RLST', struct.pack('<QQ', refl_off, len(reflection))) +
           tag(b'UUID', uuid.uuid5(uuid.NAMESPACE_URL, 'ios-vayu:' + ','.join(f[0] for f in funcs)).bytes) + b'ENDT')
    head = HEAD + struct.pack('<QQQQQQQQQ', total, funcs_off, sum(map(len, entries)), pub_off, len(pub),
                              prv_off, len(prv), bc_off, len(bc))
    data = head + listing + ext + pub + prv + bc + reflection
    assert len(head) == 0x58 and len(data) == total
    with open(out, 'wb') as f:
        f.write(data)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    funcs = []
    for spec in argv[2:]:
        name, rest = spec.split('=', 1)
        kind, path = rest.split(':', 1)
        path, *cspecs = path.split('+')
        constants = []
        for c in cspecs:
            cname, ctype, index, required = c.split(':')
            constants.append((cname, ctype, int(index), int(required)))
        funcs.append((name, kind, wrap(open(path, 'rb').read()), constants))
    build(funcs, argv[1])
    print('%s: %d functions' % (argv[1], len(funcs)))


if __name__ == '__main__':
    main(sys.argv)
