#!/usr/bin/env python3
"""Patch the prebuilt XNU kernel for vayu's virtual-timer PPI.

The generic GICv3 driver hard-codes the EL1 virtual timer at PPI 27 (INTID 27),
which is correct for redwood and QEMU.  On SM8150 (vayu) the four arch-timer
PPIs are 1, 2, 3, 0, so the virtual timer is PPI 3 = INTID 19.  Without this
patch no timer interrupt arrives: thread_calls (heartbeat, loader_return_after)
never run and the kernel stops at the first deadline wait, after the last
MiniIOMobileFramebuffer line.

This script is for the **prebuilt** kernel only, until the XNU toolchain can
rebuild `xnu/files/osfmk/arm64/gic_v3_irq.c`; that source file now takes the
same value from the `vtimer_intid` boot argument.

It changes five AArch64 instructions in `__TEXT_EXEC,__text`:
  * 1 << 27 -> 1 << 19 for the redistributor enable mask (both the single-CPU
    and the SMP path);
  * GICR_IPRIORITYR(27) -> GICR_IPRIORITYR(19);
  * the kprintf value 27 -> 19;
  * the handler's `cmp w20, #27` -> `cmp w20, #19`.

usage: tools/patch-vayu-vtimer.py [--input PATH] [--output PATH]
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xFEEDFACF

# (virtual address, expected bytes, replacement bytes, what it is)
PATCHES = [
    (0xFFFFFFF00747493C, bytes.fromhex("0800a172"), bytes.fromhex("0801a072"),
     "SMP path: movk w8, #2048, lsl #16 -> #8 (1 << 19)"),
    (0xFFFFFFF007474944, bytes.fromhex("0800a152"), bytes.fromhex("0801a052"),
     "single-CPU path: mov w8, #134217728 -> #524288 (1 << 19)"),
    (0xFFFFFFF007474968, bytes.fromhex("2a6d0e39"), bytes.fromhex("2a4d0e39"),
     "GICR_IPRIORITYR(27) -> GICR_IPRIORITYR(19)"),
    (0xFFFFFFF0074749D4, bytes.fromhex("69038052"), bytes.fromhex("69028052"),
     "kprintf: virtual timer PPI 27 -> 19"),
    (0xFFFFFFF007474B04, bytes.fromhex("9f6e0071"), bytes.fromhex("9f4e0071"),
     "handler: cmp w20, #27 -> #19"),
]


def fail(message: str) -> None:
    print(f"patch-vayu-vtimer: {message}", file=sys.stderr)
    raise SystemExit(1)


def text_section(data: bytes):
    if len(data) < 32 or struct.unpack_from("<I", data, 0)[0] != MH_MAGIC_64:
        fail("not a little-endian 64-bit Mach-O")
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    for _ in range(ncmds):
        if off + 8 > len(data):
            fail("truncated load commands")
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmdsize < 8 or off + cmdsize > len(data):
            fail("bad load-command size")
        if cmd == LC_SEGMENT_64 and cmdsize >= 72:
            segname = data[off + 8:off + 24].rstrip(b"\0")
            vmaddr, _vmsize, fileoff, _filesize = struct.unpack_from("<QQQQ", data, off + 24)
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            if segname == b"__TEXT_EXEC":
                sect = off + 72
                for _ in range(nsects):
                    if sect + 80 > off + cmdsize:
                        fail("truncated section table")
                    sectname = data[sect:sect + 16].rstrip(b"\0")
                    addr, size, offset = struct.unpack_from("<QQI", data, sect + 32)
                    if sectname == b"__text":
                        return addr, size, offset
                    sect += 80
        off += cmdsize
    fail("no __TEXT_EXEC,__text section")


def main() -> None:
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", type=Path, default=here / "prebuilt/mach.development.bcm2837")
    ap.add_argument("--output", type=Path,
                    default=here / "out/mach.development.bcm2837.vayu-vtimer")
    args = ap.parse_args()

    data = bytearray(args.input.read_bytes())
    text_vmaddr, text_size, text_fileoff = text_section(data)
    print(f"input  {args.input} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})")
    print(f"text   vmaddr {text_vmaddr:#x}, fileoff {text_fileoff:#x}, size {text_size:#x}")

    for vmaddr, old, new, what in PATCHES:
        delta = vmaddr - text_vmaddr
        if delta < 0 or delta + 4 > text_size:
            fail(f"{vmaddr:#x} is outside __TEXT_EXEC,__text")
        off = text_fileoff + delta
        got = bytes(data[off:off + 4])
        if got != old:
            fail(f"{vmaddr:#x}: expected {old.hex()}, found {got.hex()}")
        data[off:off + 4] = new
        print(f"patch  {vmaddr:#x} -> {off:#x}: {what}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"output {args.output} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})")
    print("note   prebuilt-only patch; rebuild gic_v3_irq.c for a durable kernel")


if __name__ == "__main__":
    main()
