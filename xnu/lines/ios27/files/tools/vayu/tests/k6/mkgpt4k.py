#!/usr/bin/env python3
# mkgpt4k.py OUT SIZE_MB NAME=FILE_OR_MB ...: a GPT disk of 4096-byte sectors for QEMU (virtio-blk with
# logical_block_size=4096, as the phone's UFS units are), the partitions one after another from LBA 256, each NAME
# either holding FILE (padded to whole sectors) or MB MiB of zeros. For the loader's UFS block service in QEMU
# (tools/vayu/tests/k6/run-ufs27.sh): its table names them, and the kernel's ufs_selftest checks the GPT's CRCs.
import struct, sys, uuid, zlib

SECTOR = 4096
out, size = sys.argv[1], int(sys.argv[2]) << 20
total = size // SECTOR
entries, esize = 128, 128
parts, lba = [], 256
for spec in sys.argv[3:]:
    name, what = spec.split('=', 1)
    data = open(what, 'rb').read() if not what.isdigit() else b'\0' * (int(what) << 20)
    n = (len(data) + SECTOR - 1) // SECTOR
    parts.append((name, lba, lba + n - 1, data))
    lba += n + 16
assert lba < total - 64
disk = bytearray(size)
table = bytearray(entries * esize)
basic = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
for i, (name, first, last, data) in enumerate(parts):
    table[i * esize:(i + 1) * esize] = struct.pack('<16s16sQQQ72s', basic, uuid.uuid4().bytes_le, first, last, 0,
                                                   name.encode('utf-16-le').ljust(72, b'\0'))
    disk[first * SECTOR:first * SECTOR + len(data)] = data
tcrc = zlib.crc32(table) & 0xffffffff
tsectors = len(table) // SECTOR
def header(me, other, table_lba):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, me, other, 2 + tsectors, total - 2 - tsectors,
                    uuid.uuid4().bytes_le, table_lba, entries, esize, tcrc)
    return h[:16] + struct.pack('<I', zlib.crc32(h) & 0xffffffff) + h[20:]
disk[SECTOR:SECTOR + 92] = header(1, total - 1, 2)
disk[2 * SECTOR:2 * SECTOR + len(table)] = table
disk[(total - 1 - tsectors) * SECTOR:(total - 1) * SECTOR] = table
disk[(total - 1) * SECTOR:(total - 1) * SECTOR + 92] = header(total - 1, 1, total - 1 - tsectors)
mbr = struct.pack('<B3sB3sII', 0, b'\0\x02\0', 0xee, b'\xff\xff\xff', 1, min(total - 1, 0xffffffff))
disk[446:446 + 16] = mbr
disk[510:512] = b'\x55\xaa'
open(out, 'wb').write(disk)
for name, first, last, data in parts:
    print('%s: LBA %d..%d (%d bytes of data)' % (name, first, last, len(data)))
