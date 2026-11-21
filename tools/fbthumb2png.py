#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""fbthumb2png.py LOG [OUTDIR]: the screen thumbnails of a kernel log (boot-arg vayu_fbthumb=N,
bsd/dev/vayu_fbthumb.c in the xnu-12377 tree) as PNG files, OUTDIR/fbthumb-<seq>.png (default: next to LOG).

The log may be anything that holds the kernel's lines: the phone's kernel.txt, a QEMU serial log, with other text
before or around "FBTHUMB" on the line. Each thumbnail is checked against its crc32; a thumbnail with lines missing
is written with them magenta and named fbthumb-<seq>-partial.png; "same as" lines are listed, not written again.
Python 3 standard library only. Prints one line per thumbnail; exit status 1 when none was found.
"""
import base64
import os
import re
import struct
import sys
import zlib

BEGIN = re.compile(r"FBTHUMB (\d+) (\d+)x(\d+) begin fmt=(rgb565|rgb888) div=(\d+) src=(\d+)x(\d+) lines=(\d+) "
                   r"bytes=(\d+) t=(\d+)")
LINE = re.compile(r"FBTHUMB (\d+) (\d+)x(\d+) (\d+)/(\d+): ([A-Za-z0-9+/=]+)")
END = re.compile(r"FBTHUMB (\d+) (\d+)x(\d+) end crc32=([0-9a-f]{8}) bytes=(\d+)")
SAME = re.compile(r"FBTHUMB (\d+) (\d+)x(\d+) same as (\d+) crc32=([0-9a-f]{8}) t=(\d+)")


def png(path, w, h, rgb):
    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def to_rgb(fmt, data, w, h):
    if fmt == "rgb888":
        return data[:w * h * 3]
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = data[2 * i] | data[2 * i + 1] << 8
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        out[3 * i:3 * i + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    log = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(log))
    os.makedirs(out, exist_ok=True)
    frames, found = {}, 0
    with open(log, "rb") as f:
        for raw in f:
            s = raw.decode("latin-1")
            if "FBTHUMB" not in s:
                continue
            m = LINE.search(s)
            if m:
                fr = frames.get(int(m.group(1)))
                if fr is not None:
                    fr["lines"][int(m.group(4))] = m.group(6)
                continue
            m = BEGIN.search(s)
            if m:
                seq = int(m.group(1))
                frames[seq] = {"w": int(m.group(2)), "h": int(m.group(3)), "fmt": m.group(4), "div": int(m.group(5)),
                               "src": "%sx%s" % (m.group(6), m.group(7)), "n": int(m.group(8)),
                               "bytes": int(m.group(9)), "t": int(m.group(10)), "lines": {}, "crc": None}
                continue
            m = END.search(s)
            if m and int(m.group(1)) in frames:
                frames[int(m.group(1))]["crc"] = int(m.group(4), 16)
                continue
            m = SAME.search(s)
            if m:
                print("fbthumb %s: %sx%s at %s s, the same picture as %s" % (m.group(1), m.group(2), m.group(3),
                                                                            m.group(6), m.group(4)))
    for seq in sorted(frames):
        fr = frames[seq]
        w, h, bpp = fr["w"], fr["h"], 3 if fr["fmt"] == "rgb888" else 2
        data, missing = bytearray(), 0
        for l in range(1, fr["n"] + 1):
            chunk = fr["lines"].get(l)
            want = min(144, fr["bytes"] - (l - 1) * 144)
            try:
                b = base64.b64decode(chunk, validate=True) if chunk else b""
            except Exception:
                b = b""
            if len(b) != want:
                missing += 1
                b = (b"\xff\x00\xff" if bpp == 3 else b"\x1f\xf8") * (want // bpp)
            data += b
        crc = zlib.crc32(bytes(data)) & 0xffffffff
        ok = missing == 0 and fr["crc"] == crc
        name = "fbthumb-%04d%s.png" % (seq, "" if ok else "-partial")
        rgb = to_rgb(fr["fmt"], bytes(data), w, h)
        png(os.path.join(out, name), w, h, rgb)
        lit = sum(1 for i in range(0, len(rgb), 3) if rgb[i] | rgb[i + 1] | rgb[i + 2])
        found += 1
        print("fbthumb %d: %dx%d %s (1/%d of %s) at %d s -> %s; %s; %d of %d pixels not black" % (
            seq, w, h, fr["fmt"], fr["div"], fr["src"], fr["t"], os.path.join(out, name),
            "crc32 ok" if ok else "%d line(s) missing, crc32 %s" % (
                missing, "of the log unknown" if fr["crc"] is None else "%08x != %08x" % (crc, fr["crc"])),
            lit, w * h))
    sys.exit(0 if found else 1)


if __name__ == "__main__":
    main()
