#!/usr/bin/env python3
"""Pack IMX582 register lists into fw/camera-imx582.tab ("VYCTAB01") for vayu_camera.c step 4.

The lists themselves come from the vendor sensor module (com.qti.sensormodule.vayu_*imx582.bin,
TheMuppets proprietary_vendor_xiaomi_vayu @56e4960) and are NOT kept in git: they are made on the
host (a V1.3.1 decoder writes one text file per list, lines "{0xADDR, 0xVALUE, delay},") and this
tool packs them for the root image's fw/.

  usage: camtab.py -o camera-imx582.tab init=imx582_init.txt res3=imx582_res3.txt \
             on=imx582_stream_on.txt off=imx582_stream_off.txt

Format (little-endian):
  char magic[8] "VYCTAB01"; u32 blocks
  per block: char name[8]; u32 count; count x { u16 address; u16 value; u8 address_bytes;
             u8 value_bytes; u16 delay_us }
Address bytes are 2 (IMX582's register width, sensorDriverData +0x30 = 2); value bytes are the
width the decoder printed (2 hex digits = 1 byte, 4 = 2 bytes).
"""
import argparse, re, struct

LINE = re.compile(r'^\{0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*(\d+)\},')

ap = argparse.ArgumentParser()
ap.add_argument('-o', '--output', required=True)
ap.add_argument('lists', nargs='+', help='name=file (name at most 8 bytes)')
args = ap.parse_args()

body = b''
for item in args.lists:
    name, path = item.split('=', 1)
    assert 0 < len(name) <= 8, name
    rows = []
    for line in open(path):
        m = LINE.match(line.strip())
        if not m:
            continue
        addr, value, delay = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
        vbytes = (len(m.group(2)) + 1) // 2
        assert addr <= 0xffff and vbytes in (1, 2) and delay <= 0xffff, line
        rows.append(struct.pack('<HHBBH', addr, value, 2, vbytes, delay))
    assert rows, 'no entries in ' + path
    body += struct.pack('<8sI', name.encode(), len(rows)) + b''.join(rows)
    print('%s: %d entries from %s' % (name, len(rows), path))

blocks = len(args.lists)
open(args.output, 'wb').write(b'VYCTAB01' + struct.pack('<I', blocks) + body)
print('%s: %d blocks' % (args.output, blocks))
