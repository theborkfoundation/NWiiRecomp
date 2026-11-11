#!/usr/bin/env python3
"""Find branch-link (bl) call sites targeting a given address.

Usage: ppcxref.py <elf> <target_hex> [--ctx N]
"""
import struct
import sys

from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from ppcdis import load, read_mem  # noqa: E402


def main():
    elf, target = sys.argv[1], int(sys.argv[2], 16)
    data, sections = load(elf)
    text = None
    for s in sections:
        if s["sname"] == ".text":
            text = s
            break
    assert text, ".text not found"
    blob = data[text["offset"]:text["offset"] + text["size"]]
    base = text["addr"]

    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    for off in range(0, len(blob), 4):
        w = struct.unpack_from(">I", blob, off)[0]
        if (w >> 26) != 18:  # b/bl
            continue
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        dest = (base + off + li) & 0xFFFFFFFF
        if dest != target:
            continue
        ins = next(md.disasm(blob[off:off + 4], base + off), None)
        lk = "bl" if (w & 1) else "b"
        print(f"0x{base + off:08x}: {lk} 0x{dest:08x}  (lr set)" if lk == "bl" else f"0x{base + off:08x}: b 0x{dest:08x}")


if __name__ == "__main__":
    main()
