#!/usr/bin/env python3
"""darwin's layout on this phone: the one place its numbers are made.

The port's partition darwin (sda33 on the POCO X3 Pro the port was made on, 24 GiB at the end of LUN 0, beside Android's
userdata, which stays) holds the iOS root volume, both cryptexes, var_store's slots, /private/var's HFS Plus
volume and the place where iOS stages a new boot image. Where each of them is, is said once, here, and goes
from here to everything that needs it:

  - the installer (darwin-install.sh) reads it as shell variables (`darwin-layout.py sh`);
  - darwin itself gets it as one block (`darwin-layout.py block`, written by `darwin-install.sh layout` into
    darwin's block LAYOUT_BLOCK, below ufs_write_first, which nothing of iOS can write);
  - the loader (2.5) reads that block, checks it and hands the kernel ufs_var_first=/ufs_var_blocks= and
    ufs_stage_first=/ufs_stage_blocks=/ufs_stage_head= on its command line (xnuloader/loader.c, darwin_layout);
  - the partition's identity (name, GUIDs, LBAs, unit) is the loader's struct darwin_identity as well:
    `darwin-layout.py check-loader` compares the two.

The identities are this phone's own GPT (its dump of 2026-11-17, darwin-install.sh dump-gpt, not in git:
gpt-sda.head.bin sha256 c649dd9e..., gpt-sde.head.bin 9798fb3a...). The image sizes are the iOS 16.5 (20F66,
iPhone10,3) files the fork vayu-dev wrote: rootfs.apfs 5628755968 bytes (096-04088-077.dmg extracted), the OS
cryptex 096-04666-078.dmg 2726297600 bytes, the App cryptex 10485760 bytes; the kernel's APFS finds container
",c=N" where container N-1 ends (bsd/miscfs/apfs/apfs_vfsops.c), so they lie back to back from block 0.

usage:
  darwin-layout.py [--profile vayu|qemu] sh                    the numbers as shell variables
  darwin-layout.py [--profile P] block OUT                     the layout block (4096 bytes; - for stdout)
  darwin-layout.py [--profile P] show FILE                     a layout block read back: decoded, and whether it
                                                               is this one (exit 0) or not (exit 1)
  darwin-layout.py [--profile P] gpt DISK FILE                 the first 6 blocks of a LUN (FILE, - for stdin):
                                                               CRCs, and the entries of this file's partitions on it
  darwin-layout.py [--profile P] gpt-tail DISK HEAD TAIL       the backup copy (TAIL: the LUN's last 6 blocks) too
  darwin-layout.py [--profile P] stagehead OUT IMAGE           the header flash_stage writes (tests)
  darwin-layout.py --profile qemu disk OUT                     the QEMU test disk (qemu/run-standalone-test.sh)
  darwin-layout.py check-loader [LOADER_C]                     these identities against xnuloader/loader.c's
  darwin-layout.py selftest                                    the checks against made-up wrong blocks
"""
import os
import re
import struct
import sys
import uuid
import zlib

BS = 4096
APFS = '7C3457EF-0000-11AA-AA11-00306543ECAC'
ANDROID_BOOT = '20117F86-E985-4357-B9EE-374BC1D8487D'

PROFILES = {
    'vayu': {
        'darwin': dict(disk='sda', entry=33, name='darwin', type=APFS, uniq='1C213492-90D4-4921-869E-98EC2FA0497C',
                       first=55871232, last=62162938, unit=0, c='darwin_vayu'),
        'boot': dict(disk='sde', entry=49, name='boot', type=ANDROID_BOOT, uniq='6829CC63-8DF3-89CE-5473-55CBA9339A82',
                     first=79872, last=112639, unit=4, c='boot_vayu'),
        # what must be as it is beside them (checked, never written): Android's data and the recovery
        'others': [dict(disk='sda', entry=32, name='userdata', type='1B81E7E6-F50D-419B-A739-2AEEF8DA3335',
                        uniq='F195412A-B809-DE82-929E-CFC7D8B7BDFA', first=2851328, last=55871231),
                   dict(disk='sda', entry=28, name='recovery', type='9D72D4E4-9958-42DA-AC26-BEA7A90B0434',
                        uniq='F9606103-C42F-EFE1-595E-BC49331D22CE', first=2359808, last=2392575)],
        'lun_blocks': {'sda': 62162944, 'sde': 262144},
        'write_first': 2097152,
        'layout_block': 2097151,
        'extents': [('root', 0, 1374208), ('oscryptex', 1374208, 665600), ('appcryptex', 2039808, 2560),
                    ('layout', 2097151, 1), ('varstore', 2097152, 1048576), ('var', 3145728, 2097152),
                    ('stage', 6258938, 32768), ('stagehead', 6291706, 1)],
    },
    # the test disk of qemu/run-standalone-test.sh: the same shape, small; its GUIDs are nobody's but this test's
    'qemu': {
        'darwin': dict(disk='vda', entry=2, name='darwin', type=APFS, uniq='48414B49-4F53-4441-5257-494E00000001',
                       first=2304, last=14303, unit=0, c='darwin_qemu'),
        'boot': dict(disk='vda', entry=1, name='boot', type=ANDROID_BOOT, uniq='48414B49-4F53-424F-4F54-000000000002',
                     first=256, last=2303, unit=0, c='boot_qemu'),
        'others': [],
        'lun_blocks': {'vda': 16384},
        'write_first': 1024,
        'layout_block': 1023,
        'extents': [('root', 0, 256), ('oscryptex', 256, 256), ('appcryptex', 512, 256), ('layout', 1023, 1),
                    ('varstore', 1024, 1024), ('var', 2048, 4096), ('stage', 9951, 2048), ('stagehead', 11999, 1)],
    },
}
# (the stage and its header at darwin's very end: the free space stays in one piece right after var, for the
# HFS Plus volume to grow into later)
# written while iOS runs: at write_first or past it (the loader checks the same)
WRITTEN_BY_IOS = ('varstore', 'var', 'stage', 'stagehead')
# written by the installer from the recovery, and the images for them: their containers must fill them exactly
INSTALLED = ('root', 'oscryptex', 'appcryptex')


class Bad(Exception):
    pass


def blocks_of(part):
    return part['last'] - part['first'] + 1


def check_profile(p):
    """the layout's own rules, the loader's (layout_check) and a few more of the installer's"""
    n = blocks_of(p['darwin'])
    ext = p['extents']
    names = [e[0] for e in ext]
    if len(set(names)) != len(names) or len(ext) > 32:
        raise Bad('extent names repeat, or more than 32')
    for name, first, count in ext:
        if len(name.encode()) > 15 or count <= 0 or first < 0 or first + count > n:
            raise Bad(f'{name}: empty, its name too long, or not inside darwin ({n} blocks)')
        if name in WRITTEN_BY_IOS and first < p['write_first']:
            raise Bad(f'{name}: iOS writes it, and it begins before write_first {p["write_first"]}')
        if name not in WRITTEN_BY_IOS and first + count > p['write_first']:
            raise Bad(f'{name}: only the installer writes it, and it reaches write_first {p["write_first"]}')
    for i, (a, af, ac) in enumerate(ext):
        for b, bf, bc in ext[:i]:
            if af < bf + bc and bf < af + ac:
                raise Bad(f'{a} and {b} overlap')
    d = dict((e[0], e) for e in ext)
    if d.get('layout') != ('layout', p['layout_block'], 1):
        raise Bad('the layout extent is not the layout block')
    if 'stagehead' in d and d['stagehead'][2] != 1:
        raise Bad('the stage header is not one block')
    if 'stage' in d and d['stage'][2] > blocks_of(p['boot']):
        raise Bad('the stage is larger than boot')
    for a, b in (('root', 'oscryptex'), ('oscryptex', 'appcryptex')):
        if a in d and b in d and d[a][1] + d[a][2] != d[b][1]:
            raise Bad(f'{b} does not begin where {a} ends (the kernel finds a container where the one before ends)')
    if 'root' in d and d['root'][1] != 0:
        raise Bad('the root container does not begin at block 0')


def make_block(p):
    check_profile(p)
    d = p['darwin']
    b = bytearray(BS)
    b[0:8] = b'HKLAYOUT'
    struct.pack_into('<IIQ16sQQ', b, 8, 1, len(p['extents']), blocks_of(d), uuid.UUID(d['uniq']).bytes_le,
                     p['write_first'], p['layout_block'])
    for i, (name, first, count) in enumerate(p['extents']):
        struct.pack_into('<16sQQ', b, 64 + 32 * i, name.encode(), first, count)
    struct.pack_into('<I', b, 4092, zlib.crc32(bytes(b[:4092])) & 0xffffffff)
    return bytes(b)


def read_block(b):
    """a layout block as the loader reads it (xnuloader/loader.c layout_check): its fields, or Bad"""
    if len(b) != BS:
        raise Bad(f'{len(b)} bytes, not {BS}')
    if b[:8] != b'HKLAYOUT':
        raise Bad('no layout block (no HKLAYOUT)')
    version, count, dblocks, duniq, wf, lb = struct.unpack_from('<IIQ16sQQ', b, 8)
    if version != 1 or not 0 < count <= 32:
        raise Bad(f'version {version}, {count} extents')
    if zlib.crc32(b[:4092]) & 0xffffffff != struct.unpack_from('<I', b, 4092)[0]:
        raise Bad('CRC wrong')
    ext = []
    for i in range(count):
        name, first, n = struct.unpack_from('<16sQQ', b, 64 + 32 * i)
        if name[15] != 0:
            raise Bad(f'extent {i}: its name is not terminated')
        ext.append((name.split(b'\0')[0].decode('ascii', 'replace'), first, n))
    return dict(darwin_blocks=dblocks, darwin_uniq=str(uuid.UUID(bytes_le=duniq)).upper(), write_first=wf,
                layout_block=lb, extents=ext)


def as_profile(fields, base):
    """a profile with what a block says, the identities of `base`: check_profile() then judges it"""
    p = dict(base)
    p['darwin'] = dict(base['darwin'])
    if fields['darwin_blocks'] != blocks_of(base['darwin']) or fields['darwin_uniq'] != base['darwin']['uniq']:
        raise Bad('made for another partition (size or GUID)')
    p['write_first'], p['layout_block'], p['extents'] = fields['write_first'], fields['layout_block'], fields['extents']
    if p['write_first'] != base['write_first'] or p['layout_block'] != base['layout_block']:
        raise Bad('its write_first or its own place is not this phone\'s')
    return p


def guid_text(raw):
    return str(uuid.UUID(bytes_le=bytes(raw))).upper()


def parse_gpt(head, lun_blocks, backup=None):
    """the primary GPT of a LUN from its first 6 blocks (and the backup from its last 6): entries by number"""
    if len(head) < 2 * BS:
        raise Bad('fewer than two blocks')
    h = head[BS:2 * BS]
    if h[:8] != b'EFI PART':
        raise Bad('no GPT header at block 1')
    size, crc = struct.unpack_from('<II', h, 12)
    hh = bytearray(h[:size])
    hh[16:20] = bytes(4)
    if zlib.crc32(bytes(hh)) & 0xffffffff != crc:
        raise Bad('the header CRC is wrong')
    cur, bak, fu, lu = struct.unpack_from('<QQQQ', h, 24)
    elba, n, es, tcrc = struct.unpack_from('<QIII', h, 72)
    if cur != 1 or bak != lun_blocks - 1 or es != 128 or elba != 2:
        raise Bad(f'header: current {cur}, backup {bak} (the LUN has {lun_blocks} blocks), entries at {elba}, size {es}')
    tbytes = n * es
    if len(head) < elba * BS + tbytes:
        raise Bad('the table is not all in the dump')
    t = head[elba * BS:elba * BS + tbytes]
    if zlib.crc32(t) & 0xffffffff != tcrc:
        raise Bad('the table CRC is wrong')
    entries = {}
    for i in range(n):
        e = t[i * es:(i + 1) * es]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        entries[i + 1] = dict(type=guid_text(e[:16]), uniq=guid_text(e[16:32]), first=first, last=last,
                              name=e[56:128].decode('utf-16-le').split('\0')[0])
    if backup is not None:
        bh = backup[5 * BS:6 * BS]
        if bh[:8] != b'EFI PART':
            raise Bad('no backup header in the LUN\'s last block')
        bsize, bcrc = struct.unpack_from('<II', bh, 12)
        bb = bytearray(bh[:bsize])
        bb[16:20] = bytes(4)
        bcur, bbak = struct.unpack_from('<QQ', bh, 24)
        belba, bn, bes, btcrc = struct.unpack_from('<QIII', bh, 72)
        if zlib.crc32(bytes(bb)) & 0xffffffff != bcrc or bcur != lun_blocks - 1 or bbak != 1 or bn != n or bes != es:
            raise Bad('the backup header is wrong')
        off = (belba - (lun_blocks - 6)) * BS
        if off < 0 or backup[off:off + tbytes] != t or btcrc != tcrc:
            raise Bad('the backup table is not the primary one')
    return entries, fu, lu


def check_gpt(p, disk, head, backup=None):
    """this file's partitions on DISK are where and what they are, and nothing else lies over darwin or boot"""
    if disk not in p['lun_blocks']:
        raise Bad(f'{disk}: no partition of this file is on it')
    entries, fu, lu = parse_gpt(head, p['lun_blocks'][disk], backup)
    mine = [x for x in [p['darwin'], p['boot']] + p['others'] if x['disk'] == disk]
    for want in mine:
        got = entries.get(want['entry'])
        have = dict((k, want[k]) for k in ('name', 'type', 'uniq', 'first', 'last'))
        if got != have:
            raise Bad(f'{disk} entry {want["entry"]} is {got}, not {have}')
        if want['first'] < fu or want['last'] > lu:
            raise Bad(f'{want["name"]} is not inside the usable blocks {fu}..{lu}')
        for k, e in entries.items():
            if k != want['entry'] and e['first'] <= want['last'] and e['last'] >= want['first']:
                raise Bad(f'entry {k} ({e["name"]}) lies over {want["name"]}')
        named = [k for k, e in entries.items() if e['name'] == want['name']]
        if named != [want['entry']]:
            raise Bad(f'more than one partition named {want["name"]}: {named}')
    return [(w['name'], w['entry'], w['first'], w['last']) for w in mine]


def c_guid(text):
    a, b, c, d = re.findall(r'0x([0-9a-f]+)\s*,\s*0x([0-9a-f]+)\s*,\s*0x([0-9a-f]+)\s*,\s*\{([^}]*)\}', text)[0]
    raw = struct.pack('<IHH', int(a, 16), int(b, 16), int(c, 16)) + bytes(int(x, 16) for x in d.split(','))
    return guid_text(raw)


def check_loader(path):
    src = open(path).read()
    bad = []
    for prof_name, p in PROFILES.items():
        for role in ('darwin', 'boot'):
            want = p[role]
            m = re.search(r'static const struct darwin_identity ' + want['c'] + r' = \{(.*?)\n\};', src, re.S)
            if not m:
                bad.append(f'{want["c"]}: not in {path}')
                continue
            body = m.group(1)

            def field(name, default=0):
                f = re.search(r'\.' + name + r'\s*=\s*(\d+)', body)
                return int(f.group(1)) if f else default
            got = dict(first=field('first_lba'), last=field('last_lba'), unit=field('unit'),
                       type=c_guid(re.search(r'\.type\s*=\s*\{(.*?\})\s*\}', body, re.S).group(1)),
                       uniq=c_guid(re.search(r'\.uniq\s*=\s*\{(.*?\})\s*\}', body, re.S).group(1)),
                       write_first=field('write_first'), layout_block=field('layout_block'))
            exp = dict(first=want['first'], last=want['last'], unit=want['unit'], type=want['type'], uniq=want['uniq'],
                       write_first=p['write_first'] if role == 'darwin' else 0,
                       layout_block=p['layout_block'] if role == 'darwin' else 0)
            if got != exp:
                bad.append(f'{want["c"]}: loader.c has {got}, this file {exp}')
    return bad


def make_disk(p, out):
    """a GPT disk of 4096-byte blocks with boot and darwin as the profile says, darwin's layout block in it"""
    n = p['lun_blocks']['vda']
    entries = bytearray(128 * 128)
    for part in (p['boot'], p['darwin']):
        struct.pack_into('<16s16sQQQ72s', entries, (part['entry'] - 1) * 128, uuid.UUID(part['type']).bytes_le,
                         uuid.UUID(part['uniq']).bytes_le, part['first'], part['last'], 0,
                         part['name'].encode('utf-16-le'))
    ecrc = zlib.crc32(bytes(entries)) & 0xffffffff
    disk_guid = uuid.UUID('48414B49-4F53-4449-534B-000000000000').bytes_le

    def header(cur, bak, elba):
        h = bytearray(92)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x10000, 92, 0, 0, cur, bak, 6, n - 6, disk_guid,
                         elba, 128, 128, ecrc)
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)) & 0xffffffff)
        return bytes(h) + bytes(BS - 92)
    mbr = bytearray(BS)              # the protective MBR: one partition of type 0xEE over the whole disk
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1, min(n - 1, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    with open(out, 'wb') as f:
        f.truncate(n * BS)
        f.seek(0)
        f.write(mbr)
        f.write(header(1, n - 1, 2))
        f.write(entries)
        f.seek((n - 5) * BS)
        f.write(entries)
        f.write(header(n - 1, 1, n - 5))
        f.seek((p['darwin']['first'] + p['layout_block']) * BS)
        f.write(make_block(p))


def make_stagehead(p, image):
    data = open(image, 'rb').read()
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
    stage = dict((e[0], e) for e in p['extents'])['stage']
    return struct.pack('<8sQQQ', b'XNUFLASH', stage[1], len(data), h) + bytes(BS - 32)


def shell(p):
    d, b = p['darwin'], p['boot']
    lines = [f'DARWIN_DISK={d["disk"]} DARWIN_PART={d["entry"]} DARWIN_NODE=/dev/block/{d["disk"]}{d["entry"]}',
             f'DARWIN_FIRST={d["first"]} DARWIN_LAST={d["last"]} DARWIN_BLOCKS={blocks_of(d)}',
             f'DARWIN_TYPE={d["type"]} DARWIN_UNIQ={d["uniq"]}',
             f'DARWIN_LUN_BLOCKS={p["lun_blocks"][d["disk"]]}',
             f'BOOT_DISK={b["disk"]} BOOT_PART={b["entry"]} BOOT_NODE=/dev/block/{b["disk"]}{b["entry"]}',
             f'BOOT_FIRST={b["first"]} BOOT_LAST={b["last"]} BOOT_BLOCKS={blocks_of(b)} BOOT_BYTES={blocks_of(b) * BS}',
             f'BOOT_LUN_BLOCKS={p["lun_blocks"][b["disk"]]}',
             f'WRITE_FIRST={p["write_first"]} LAYOUT_BLOCK={p["layout_block"]} BS={BS}']
    for name, first, count in p['extents']:
        lines.append(f'{name.upper()}_FIRST={first} {name.upper()}_BLOCKS={count}')
    return '\n'.join(lines)


def selftest():
    fails = 0

    def expect(what, fn, bad):
        nonlocal fails
        try:
            fn()
            ok = not bad
        except Bad:
            ok = bad
        print(f'  {"ok  " if ok else "FAIL"} {what}')
        fails += not ok
    for name, p in PROFILES.items():
        blk = make_block(p)
        expect(f'{name}: the block reads back as made', lambda: as_profile(read_block(blk), p) and
               check_profile(as_profile(read_block(blk), p)), False)
        for what, change in (('an extent over another', lambda e: e[:5] + [('var', e[5][1] - 1, e[5][2])] + e[6:]),
                             ('var before write_first', lambda e: e[:5] + [('var', p['write_first'] - 2, 1)] + e[6:]),
                             ('root past write_first', lambda e: [('root', 0, p['write_first'] + 1)] + e[1:]),
                             ('a gap between the root and the OS cryptex',
                              lambda e: [('root', 0, e[0][2] - 1)] + e[1:]),
                             ('stage past darwin', lambda e: e[:6] + [('stage', blocks_of(p['darwin']) - 1, 2)] + e[7:])):
            q = dict(p)
            q['extents'] = change(list(p['extents']))
            expect(f'{name}: {what} is refused', lambda: check_profile(q), True)
        bad = bytearray(blk)
        bad[100] ^= 1
        expect(f'{name}: a block with one bit changed is refused', lambda: read_block(bytes(bad)), True)
        other = dict(p)
        other['darwin'] = dict(p['darwin'], uniq='00000000-0000-0000-0000-000000000001')
        expect(f'{name}: a block made for another partition is refused', lambda: as_profile(read_block(blk), other), True)
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        make_disk(PROFILES['qemu'], t + '/disk')
        raw = open(t + '/disk', 'rb').read()
        q = PROFILES['qemu']
        expect('qemu disk: its GPT is right', lambda: check_gpt(q, 'vda', raw[:6 * BS], raw[-6 * BS:]), False)
        lb = (q['darwin']['first'] + q['layout_block']) * BS
        expect('qemu disk: its layout block is the profile\'s', lambda: as_profile(read_block(raw[lb:lb + BS]), q), False)
        broken = bytearray(raw[:6 * BS])
        broken[2 * BS + 128 + 40] ^= 1                  # darwin's last LBA, CRC not fixed
        expect('qemu disk: a table changed under its CRC is refused', lambda: check_gpt(q, 'vda', bytes(broken)), True)
    here = os.path.dirname(os.path.abspath(__file__))
    loader = os.path.join(here, '..', '..', '..', 'xnuloader', 'loader.c')
    if os.path.isfile(loader):
        bad = check_loader(loader)
        for b in bad:
            print('  ' + b)
        print(f'  {"ok  " if not bad else "FAIL"} the loader\'s identities are this file\'s')
        fails += bool(bad)
    print(f'selftest: {"PASS" if not fails else f"{fails} FAILED"}')
    return 1 if fails else 0


def main(argv):
    prof = 'vayu'
    if len(argv) >= 2 and argv[0] == '--profile':
        prof, argv = argv[1], argv[2:]
    if prof not in PROFILES or not argv:
        sys.exit(__doc__)
    p = PROFILES[prof]
    cmd, args = argv[0], argv[1:]
    try:
        if cmd == 'sh' and not args:
            check_profile(p)
            print(shell(p))
        elif cmd == 'block' and len(args) == 1 and args[0] == '-':
            sys.stdout.buffer.write(make_block(p))
        elif cmd == 'block' and len(args) == 1:
            open(args[0], 'wb').write(make_block(p))
            print(f'layout: {args[0]}: the layout block of {prof} ({len(p["extents"])} extents), crc '
                  f'{struct.unpack_from("<I", make_block(p), 4092)[0]:08x}')
        elif cmd == 'show' and len(args) == 1:
            raw = sys.stdin.buffer.read() if args[0] == '-' else open(args[0], 'rb').read()
            if raw == bytes(len(raw)):
                print('layout: an empty block (no layout written there yet)')
                return 1
            f = read_block(raw)
            for name, first, count in f['extents']:
                print(f'  {name:12s} {first:9d} + {count:9d} blocks ({count * BS / 2**30:7.3f} GiB)')
            check_profile(as_profile(f, p))
            same = raw == make_block(p)
            print(f'layout: a valid block for {prof}\'s darwin; {"the SAME as" if same else "NOT the same as"} '
                  f'darwin-layout.py\'s')
            return 0 if same else 1
        elif cmd in ('gpt', 'gpt-tail') and len(args) == (2 if cmd == 'gpt' else 3):
            raw = sys.stdin.buffer.read() if args[1] == '-' else open(args[1], 'rb').read()
            tail = open(args[2], 'rb').read() if cmd == 'gpt-tail' else None
            for name, entry, first, last in check_gpt(p, args[0], raw, tail):
                print(f'gpt: {args[0]}{entry} is {name}, LBA {first}..{last}, as this phone has it')
            print(f'gpt: {args[0]}: CRCs right{", backup copy the same" if tail else ""}; nothing else lies over them')
        elif cmd == 'stagehead' and len(args) == 2:
            open(args[0], 'wb').write(make_stagehead(p, args[1]))
        elif cmd == 'disk' and len(args) == 1 and prof == 'qemu':
            make_disk(p, args[0])
            print(f'layout: {args[0]}: the QEMU test disk ({p["lun_blocks"]["vda"]} blocks of {BS})')
        elif cmd == 'check-loader' and len(args) <= 1:
            path = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..',
                                                     'xnuloader', 'loader.c')
            bad = check_loader(path)
            for b in bad:
                print('check-loader: ' + b)
            print('check-loader: ' + ('the loader\'s identities are this file\'s' if not bad else 'DIFFERENT'))
            return 1 if bad else 0
        elif cmd == 'selftest' and not args:
            return selftest()
        else:
            sys.exit(__doc__)
    except Bad as e:
        print(f'darwin-layout: REFUSED: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
