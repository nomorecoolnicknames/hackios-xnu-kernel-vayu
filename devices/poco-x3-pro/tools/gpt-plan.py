#!/usr/bin/env python3
"""Preview (and produce) the GPT change that makes room for darwin, on dumps of the unit's ends, off the phone.

usage:
  gpt-plan.py show  HEAD TAIL BLOCKS                       the dumped GPT as it is: both headers, CRCs, entries
  gpt-plan.py compare HEAD TAIL HEAD2 TAIL2 BLOCKS          the two GPTs field by field (headers, tables, entries), not
                                                           byte by byte: block 0 (the protective MBR) and the disk GUID
                                                           are reported, as sgdisk may rewrite them legitimately
  gpt-plan.py plan  HEAD TAIL BLOCKS ENTRIES N END NEW START LAST [UNIQ] OUTDIR
                                                           the planned GPT: header fields, CRCs, the changed entries,
                                                           and OUTDIR/head.bin, OUTDIR/tail.bin as they would be

HEAD is the unit's first 6 blocks (LBA 0..5), TAIL its last 6 (LBA BLOCKS-6..BLOCKS-1), both read on the phone with
dd bs=4096. BLOCKS is the unit's size in blocks of 4096. The plan:
  - the table resized to ENTRIES entries (64: 2 blocks; primary at LBA 2.., backup right before the backup header),
    first usable LBA kept, last usable LBA = the backup table's first LBA - 1;
  - entry N (1-based, the partition to shorten) ends at END instead, everything else of it kept (type, unique GUID,
    attributes, name);
  - entry NEW is added: darwin, type Apple APFS, START..LAST, unique GUID UNIQ (random when not given).
Every check fails loudly: CRCs of the input, entries overlapping, the new last usable LBA before any entry's end,
the table not fitting before the first usable LBA. Nothing here writes to a device.
"""
import os
import struct
import sys
import uuid
import zlib

BS = 4096
SIG = b'EFI PART'
APFS = uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC')
HDR = struct.Struct('<8sIIIIQQQQ16sQIII')       # 92 bytes


def guid_bytes(u):
    return u.bytes_le


def guid_str(b):
    return str(uuid.UUID(bytes_le=bytes(b))).upper()


def parse_header(block):
    f = HDR.unpack_from(block, 0)
    h = dict(sig=f[0], rev=f[1], size=f[2], crc=f[3], current=f[5], backup=f[6], first=f[7], last=f[8],
             disk=f[9], entries_lba=f[10], count=f[11], esize=f[12], ecrc=f[13])
    raw = bytearray(block[:h['size']])
    struct.pack_into('<I', raw, 16, 0)
    h['crc_ok'] = zlib.crc32(raw) & 0xffffffff == h['crc']
    return h


def build_header(h):
    raw = bytearray(HDR.pack(SIG, h['rev'], 92, 0, 0, h['current'], h['backup'], h['first'], h['last'], h['disk'],
                             h['entries_lba'], h['count'], h['esize'], h['ecrc']))
    struct.pack_into('<I', raw, 16, zlib.crc32(raw) & 0xffffffff)
    return bytes(raw) + b'\0' * (BS - len(raw))


def parse_entries(raw, count, esize):
    out = []
    for i in range(count):
        e = raw[i * esize:(i + 1) * esize]
        type_g, uniq, first, last, attrs = e[:16], e[16:32], *struct.unpack_from('<QQQ', e, 32)
        name = e[56:128].decode('utf-16-le', 'replace').split('\0')[0]
        out.append(dict(type=type_g, uniq=uniq, first=first, last=last, attrs=attrs, name=name,
                        empty=type_g == b'\0' * 16))
    return out


def build_entries(entries, count, esize):
    raw = bytearray(count * esize)
    for i, e in enumerate(entries):
        if e['empty']:
            continue
        name = e['name'].encode('utf-16-le')[:72]
        struct.pack_into('<16s16sQQQ72s', raw, i * esize, bytes(e['type']), bytes(e['uniq']), e['first'], e['last'],
                         e['attrs'], name)
    return bytes(raw)


def load(head_path, tail_path, blocks):
    head, tail = open(head_path, 'rb').read(), open(tail_path, 'rb').read()
    if len(head) != 6 * BS or len(tail) != 6 * BS:
        sys.exit('gpt-plan: HEAD and TAIL must be 6 blocks of 4096 each')
    ph = parse_header(head[BS:2 * BS])
    bh = parse_header(tail[5 * BS:6 * BS])
    for name, h in (('primary', ph), ('backup', bh)):
        if h['sig'] != SIG or not h['crc_ok']:
            sys.exit(f'gpt-plan: the {name} header is not a valid GPT header')
    if ph['current'] != 1 or ph['backup'] != blocks - 1 or bh['current'] != blocks - 1:
        sys.exit(f'gpt-plan: header LBAs do not fit a unit of {blocks} blocks')
    nblk = (ph['count'] * ph['esize'] + BS - 1) // BS
    if ph['entries_lba'] + nblk > 6:
        sys.exit('gpt-plan: the primary table is not inside the first 6 blocks')
    ptab = head[ph['entries_lba'] * BS:(ph['entries_lba'] + nblk) * BS][:ph['count'] * ph['esize']]
    toff = bh['entries_lba'] - (blocks - 6)
    if toff < 0:
        sys.exit('gpt-plan: the backup table is not inside the last 6 blocks')
    btab = tail[toff * BS:toff * BS + bh['count'] * bh['esize']]
    for name, h, tab in (('primary', ph, ptab), ('backup', bh, btab)):
        if zlib.crc32(tab) & 0xffffffff != h['ecrc']:
            sys.exit(f'gpt-plan: the {name} table CRC is wrong')
    if ptab != btab:
        sys.exit('gpt-plan: the primary and backup tables differ')
    return head, tail, ph, bh, parse_entries(ptab, ph['count'], ph['esize'])


def report(title, ph, bh, entries, blocks, mark=()):
    print(f'== {title}')
    for name, h in (('primary', ph), ('backup', bh)):
        print(f'{name} header: LBA {h["current"]}, other {h["backup"]}, usable {h["first"]}..{h["last"]}, '
              f'table at {h["entries_lba"]}, {h["count"]} x {h["esize"]}, header crc {h["crc"]:08x}, '
              f'table crc {h["ecrc"]:08x}, disk {guid_str(h["disk"])}')
    for i, e in enumerate(entries, 1):
        if e['empty']:
            continue
        flag = ' <==' if i in mark else ''
        print(f'{i:3d} {e["name"]:<16} {e["first"]:>10}..{e["last"]:<10} type {guid_str(e["type"])} '
              f'uniq {guid_str(e["uniq"])} attrs {e["attrs"]:x}{flag}')
    print(f'unit: {blocks} blocks; used entries {sum(not e["empty"] for e in entries)} of {len(entries)}')


def check(entries, first_usable, last_usable):
    used = sorted((e['first'], e['last'], e['name']) for e in entries if not e['empty'])
    for a, b, n in used:
        if a < first_usable or b > last_usable or a > b:
            sys.exit(f'gpt-plan: {n} {a}..{b} is outside usable {first_usable}..{last_usable}')
    for (a1, b1, n1), (a2, b2, n2) in zip(used, used[1:]):
        if a2 <= b1:
            sys.exit(f'gpt-plan: {n1} and {n2} overlap')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'show' and len(args) == 3:
        head, tail, ph, bh, entries = load(args[0], args[1], int(args[2]))
        report('as dumped', ph, bh, entries, int(args[2]))
        return
    if cmd == 'compare' and len(args) == 5:
        blocks = int(args[4])
        a, b = load(args[0], args[1], blocks), load(args[2], args[3], blocks)
        bad = 0
        for side, ha, hb in (('primary', a[2], b[2]), ('backup', a[3], b[3])):
            for k in ('current', 'backup', 'first', 'last', 'entries_lba', 'count', 'esize', 'ecrc'):
                if ha[k] != hb[k]:
                    print(f'DIFFERENT: {side} header {k}: {ha[k]} vs {hb[k]}')
                    bad += 1
            if ha['disk'] != hb['disk']:
                print(f'note: {side} disk GUID {guid_str(ha["disk"])} vs {guid_str(hb["disk"])} (not counted)')
        for i, (ea, eb) in enumerate(zip(a[4], b[4]), 1):
            for k in ('type', 'uniq', 'first', 'last', 'attrs', 'name'):
                if ea[k] != eb[k]:
                    print(f'DIFFERENT: entry {i} {k}: {ea[k]!r} vs {eb[k]!r}')
                    bad += 1
        if a[0][:BS] != b[0][:BS]:
            print('note: block 0 (protective MBR) differs (not counted): sgdisk may rewrite it')
        print('SAME GPT' if bad == 0 else f'{bad} DIFFERENCES: stop')
        sys.exit(0 if bad == 0 else 1)
    if cmd != 'plan' or len(args) not in (10, 11):
        sys.exit(__doc__)
    head_p, tail_p, blocks, count, n, end, new, start, last = args[:9]
    blocks, count, n, end, new, start, last = (int(x, 0) for x in (blocks, count, n, end, new, start, last))
    uniq = uuid.UUID(args[9]) if len(args) == 11 else uuid.uuid4()
    outdir = args[-1]
    head, tail, ph, bh, entries = load(head_p, tail_p, blocks)
    report('before', ph, bh, entries, blocks)
    esize = ph['esize']
    nblk = (count * esize + BS - 1) // BS
    if count < len(entries) or count * esize % BS:
        sys.exit('gpt-plan: the new table must hold the old entries and fill whole blocks')
    if 2 + nblk > ph['first']:
        sys.exit(f'gpt-plan: a table of {count} entries ({nblk} blocks from LBA 2) does not end before '
                 f'the first usable LBA {ph["first"]}')
    entries = entries + [dict(type=b'\0' * 16, uniq=b'\0' * 16, first=0, last=0, attrs=0, name='', empty=True)] * \
        (count - len(entries))
    if entries[n - 1]['empty'] or not entries[new - 1]['empty']:
        sys.exit('gpt-plan: entry N must be used and entry NEW free')
    entries[n - 1] = dict(entries[n - 1], last=end)
    entries[new - 1] = dict(type=guid_bytes(APFS), uniq=guid_bytes(uniq), first=start, last=last, attrs=0,
                            name='darwin', empty=False)
    # the backup table where it is, if the bigger one still ends before the backup header, and the usable range as
    # it is (vayu's unit 0: table at 62162939, usable ..62162938 = userdata's end, 3 free blocks behind the table);
    # else right before the backup header, with the last usable LBA moved to just before it
    if bh['entries_lba'] + nblk <= blocks - 1 and bh['entries_lba'] > ph['last']:
        btab_lba = bh['entries_lba']
        last_usable = ph['last']
    else:
        btab_lba = blocks - 1 - nblk
        last_usable = btab_lba - 1
    check(entries, ph['first'], last_usable)
    tab = build_entries(entries, count, esize)
    ecrc = zlib.crc32(tab) & 0xffffffff
    nph = dict(ph, entries_lba=2, count=count, ecrc=ecrc, last=last_usable)
    nbh = dict(bh, entries_lba=btab_lba, count=count, ecrc=ecrc, last=last_usable)
    hp, hb = build_header(nph), build_header(nbh)
    nph, nbh = parse_header(hp), parse_header(hb)
    report('planned', nph, nbh, entries, blocks, mark=(n, new))
    new_head = bytearray(head)
    new_head[BS:2 * BS] = hp
    new_head[2 * BS:(2 + nblk) * BS] = tab
    new_tail = bytearray(tail)
    toff = btab_lba - (blocks - 6)
    new_tail[toff * BS:(toff + nblk) * BS] = tab
    new_tail[5 * BS:6 * BS] = hb
    if True:
        os.makedirs(outdir, exist_ok=True)
        open(os.path.join(outdir, 'head.bin'), 'wb').write(new_head)
        open(os.path.join(outdir, 'tail.bin'), 'wb').write(new_tail)
        print(f'written: {outdir}/head.bin, {outdir}/tail.bin (nothing on any device)')


if __name__ == '__main__':
    main()
