#!/usr/bin/env python3
"""Turn an assembled game data object (ELF32 ARM) into one an arm64 link takes.

The game's data (data/*.s, sound, the generated map and bundle files) is
assembled as for the 32-bit build, by arm-none-eabi-as: the GBA sources use
its syntax (`@` comments, its macros), and assembling them once keeps the
data byte for byte the same on both architectures - the save layout and the
bundles depend on that. These objects hold data only, and their only
relocation is R_ARM_ABS32 (a 32-bit address, `.int symbol`).

This rewrites such an object as ELF64 AArch64: the same sections, symbols and
contents, every REL relocation as RELA with its addend taken from the data.
R_ARM_ABS32 becomes R_AARCH64_ABS32 - the arm64 build keeps the whole game
image below 4 GiB, where a 32-bit address is the address (android_port/src
android_boot.c). In sections named with --abs64, where each pointer is
written `.int symbol, 0`, it becomes R_AARCH64_ABS64 over the 8 bytes.

  elf32to64.py [--abs64 SECTION]... in.o out.o
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SHT_NULL, SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 0, 1, 2, 3, 4, 8, 9
SHT_ARM_ATTRIBUTES = 0x70000003
EM_ARM, EM_AARCH64 = 40, 183
R_ARM_NONE, R_ARM_ABS32 = 0, 2
R_AARCH64_ABS64, R_AARCH64_ABS32 = 257, 258


def fail(message: str) -> None:
    sys.exit("elf32to64: " + message)


def convert(data: bytes, abs64: set[str]) -> bytes:
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail("not a little-endian ELF32 file")
    (e_type, e_machine, _, _, _, e_shoff, e_flags, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_type != 1 or e_machine != EM_ARM:
        fail("not an ARM relocatable object")

    shdrs = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    shstr = shdrs[e_shstrndx]
    names = [data[shstr[4] + h[0]:].split(b"\0", 1)[0].decode() for h in shdrs]

    # Every section is kept, so section symbols keep their indices. ARM
    # attributes have no arm64 meaning: they become plain data the linker
    # discards (SHF_EXCLUDE).
    keep = list(range(e_shnum))
    index = {old: new for new, old in enumerate(keep)}

    def remap(shndx: int) -> int:
        if shndx == 0 or shndx >= 0xFF00:
            return shndx
        if shndx not in index:
            fail("a symbol is defined in a dropped section")
        return index[shndx]

    contents: dict[int, bytearray] = {}
    for i in keep:
        h = shdrs[i]
        contents[i] = bytearray(b"" if h[1] in (SHT_NOBITS, SHT_NULL) else data[h[4]:h[4] + h[5]])

    # Symbols: Elf32_Sym (name, value, size, info, other, shndx) -> Elf64_Sym.
    for i in keep:
        if shdrs[i][1] != SHT_SYMTAB:
            continue
        old = contents[i]
        new = bytearray()
        for off in range(0, len(old), 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", old, off)
            new += struct.pack("<IBBHQQ", st_name, st_info, st_other, remap(st_shndx), st_value, st_size)
        contents[i] = new

    # Relocations: Elf32_Rel (offset, info) -> Elf64_Rela (offset, info, addend).
    rel_names = {}
    for i in keep:
        h = shdrs[i]
        if h[1] == SHT_RELA:
            fail("RELA input is not expected from arm-none-eabi-as")
        if h[1] != SHT_REL:
            continue
        target = h[7]
        target_name = names[target]
        wide = target_name in abs64
        body = contents[target]
        new = bytearray()
        for off in range(0, len(contents[i]), 8):
            r_offset, r_info = struct.unpack_from("<II", contents[i], off)
            sym, kind = r_info >> 8, r_info & 0xFF
            if kind == R_ARM_NONE:
                continue
            if kind != R_ARM_ABS32:
                fail("relocation type %d in %s: only R_ARM_ABS32 is handled" % (kind, target_name))
            addend = struct.unpack_from("<i", body, r_offset)[0]
            struct.pack_into("<I", body, r_offset, 0)
            if wide:
                if body[r_offset + 4:r_offset + 8] != b"\0\0\0\0":
                    fail("%s+%#x: an --abs64 pointer must be written `.int symbol, 0`" % (target_name, r_offset))
                new_kind = R_AARCH64_ABS64
            else:
                new_kind = R_AARCH64_ABS32
            new += struct.pack("<QQq", r_offset, (sym << 32) | new_kind, addend)
        contents[i] = new
        rel_names[i] = ".rela" + names[i][len(".rel"):]

    # Section names: .rel* becomes .rela*, so the string table is rebuilt.
    shstrtab = bytearray(b"\0")
    name_off = {}
    for i in keep:
        name = rel_names.get(i, names[i])
        name_off[i] = len(shstrtab)
        shstrtab += name.encode() + b"\0"
    contents[e_shstrndx] = shstrtab

    out = bytearray(64)
    offsets = {}
    for i in keep:
        if not contents[i]:
            offsets[i] = len(out)
            continue
        align = max(shdrs[i][8], 1)
        if shdrs[i][1] in (SHT_SYMTAB, SHT_REL):
            align = 8
        while len(out) % align:
            out.append(0)
        offsets[i] = len(out)
        out += contents[i]
    while len(out) % 8:
        out.append(0)
    shoff = len(out)
    for i in keep:
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = shdrs[i]
        if sh_type == SHT_ARM_ATTRIBUTES:
            sh_type, sh_flags = SHT_PROGBITS, 0x80000000  # SHF_EXCLUDE
        size = sh_size if sh_type == SHT_NOBITS else len(contents[i])
        if sh_type == SHT_SYMTAB:
            entsize, align = 24, 8
        elif sh_type == SHT_REL:
            sh_type, entsize, align = SHT_RELA, 24, 8
        else:
            entsize, align = sh_entsize, sh_addralign
        link = index.get(sh_link, 0) if sh_link else 0
        info = sh_info
        if sh_type == SHT_RELA:
            info = index[sh_info]
            sh_flags |= 0x40  # SHF_INFO_LINK
        out += struct.pack("<IIQQQQIIQQ", name_off[i], sh_type, sh_flags, 0,
                           offsets[i] if sh_type != SHT_NULL else 0, size, link, info, align, entsize)
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    struct.pack_into("<16sHHIQQQIHHHHHH", out, 0, ident, 1, EM_AARCH64, 1, 0, 0, shoff, 0,
                     64, 0, 0, 64, len(keep), index[e_shstrndx])
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--abs64", action="append", default=[], metavar="SECTION")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(convert(args.input.read_bytes(), set(args.abs64)))


if __name__ == "__main__":
    main()
