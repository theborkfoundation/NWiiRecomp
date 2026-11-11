#!/usr/bin/env python3
"""Minimal PowerPC disassembler for a 32-bit big-endian ELF (Gekko/Broadway).

Usage: dis.py <elf> <start_hex> <end_hex>
"""
import struct
import sys

from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN


def load(elf_path):
    data = open(elf_path, "rb").read()
    assert data[:4] == b"\x7fELF", "not an ELF"
    assert data[4] == 1, "not 32-bit"
    assert data[5] == 2, "not big-endian"
    e_shoff = struct.unpack_from(">I", data, 0x20)[0]
    e_shentsize = struct.unpack_from(">H", data, 0x2E)[0]
    e_shnum = struct.unpack_from(">H", data, 0x30)[0]
    e_shstrndx = struct.unpack_from(">H", data, 0x32)[0]

    def sh(i):
        off = e_shoff + i * e_shentsize
        (name, typ, flags, addr, offset, size,
         link, info, align, entsize) = struct.unpack_from(">10I", data, off)
        return dict(name=name, type=typ, addr=addr, offset=offset, size=size)

    sections = [sh(i) for i in range(e_shnum)]
    strtab = sections[e_shstrndx]
    strdata = data[strtab["offset"]:strtab["offset"] + strtab["size"]]

    for s in sections:
        end = strdata.find(b"\x00", s["name"])
        s["sname"] = strdata[s["name"]:end].decode("latin-1")
    return data, sections


def read_mem(data, sections, addr, length):
    """Read `length` bytes at virtual address `addr`, spanning sections."""
    out = bytearray()
    cur = addr
    while len(out) < length:
        hit = None
        for s in sections:
            if s["type"] == 8:  # SHT_NOBITS
                continue
            if s["addr"] <= cur < s["addr"] + s["size"]:
                hit = s
                break
        if hit is None:
            break
        off = hit["offset"] + (cur - hit["addr"])
        n = min(length - len(out), hit["addr"] + hit["size"] - cur)
        out += data[off:off + n]
        cur += n
    return bytes(out) if out else None


def main():
    elf, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    data, sections = load(elf)
    blob = read_mem(data, sections, start, end - start)
    if blob is None:
        print("address not mapped")
        return
    if len(blob) < end - start:
        print(f"warning: only {len(blob)} of {end - start} bytes mapped")
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = True
    off = 0
    while off < len(blob):
        ins_list = list(md.disasm(blob[off:off + 4], start + off))
        if not ins_list:
            print(f"0x{start + off:08x}: {blob[off:off + 4].hex():<8} .word    {struct.unpack_from('>I', blob, off)[0]:#010x}")
            off += 4
            continue
        for ins in ins_list:
            print(f"0x{ins.address:08x}: {ins.bytes.hex():<8} {ins.mnemonic:<8} {ins.op_str}")
        off += 4


if __name__ == "__main__":
    main()
