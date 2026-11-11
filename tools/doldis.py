#!/usr/bin/env python3
"""Disassemble an address range from a Wii main.dol (Gekko/Broadway).

The DOL header lists up to 7 text and 11 data sections, each with its own file
offset and address, so a plain "file offset = addr - base" mapping is only valid
inside the first text section. This resolves the section properly.

Usage: doldis.py <main.dol> <start_hex> <end_hex> [--hex]
"""
import struct
import sys

from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN


def sections(data):
    # DOL header: offsets at 0x00/0x1C, addresses at 0x48/0x64, sizes at
    # 0x90/0xAC. They are three parallel tables, not interleaved triples.
    out = []
    for i in range(7):
        off = struct.unpack_from(">I", data, 0x00 + i * 4)[0]
        addr = struct.unpack_from(">I", data, 0x48 + i * 4)[0]
        size = struct.unpack_from(">I", data, 0x90 + i * 4)[0]
        if size:
            out.append((addr, addr + size, off, "text%d" % i))
    for i in range(11):
        off = struct.unpack_from(">I", data, 0x1C + i * 4)[0]
        addr = struct.unpack_from(">I", data, 0x64 + i * 4)[0]
        size = struct.unpack_from(">I", data, 0xAC + i * 4)[0]
        if size:
            out.append((addr, addr + size, off, "data%d" % i))
    return out


def read(data, secs, addr, length):
    for start, end, off, name in secs:
        if start <= addr < end:
            n = min(length, end - addr)
            return data[off + (addr - start):off + (addr - start) + n], name
    return b"", None


def main():
    dol, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    want_hex = "--hex" in sys.argv
    data = open(dol, "rb").read()
    secs = sections(data)
    raw, name = read(data, secs, start, end - start)
    if not raw:
        print("address 0x%X is not mapped in this DOL" % start, file=sys.stderr)
        return 1
    print("; %s 0x%X-0x%X (%d bytes)" % (name, start, start + len(raw), len(raw)))
    if want_hex:
        for i in range(0, len(raw), 16):
            chunk = raw[i:i + 16]
            print("%08X  %s" % (start + i, " ".join("%02X" % b for b in chunk)))
        return 0
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False
    n = 0
    for ins in md.disasm(raw, start):
        print("%08X  %-10s %s" % (ins.address, ins.mnemonic, ins.op_str))
        n += 1
    if n == 0:
        print("; no instructions decoded", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
