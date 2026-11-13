#!/usr/bin/env python3
"""Fail the build if the save's layout differs between 32-bit ARM and arm64.

A save file is the save blocks' bytes as they are in memory, so the arm64
build can read a GBA save, an emulator's or the 32-bit app's, and they its,
only while every member of those structures sits at the same offset. Pointers
are what move on a 64-bit build; the saved ones are kept 4 bytes wide (u32,
PTR32 in global.h), and this checks nothing else slipped in.

Both compilers build the same small unit with debug information, which records
the real layout; every member, at every depth, of the saved structures is
compared, and their sizes against the GBA's.

  check_save_layout.py NDK_BIN GAME_ROOT STAMP
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

# The GBA's sizes (pokeemerald built by agbcc).
SAVED = {"SaveBlock1": 0x3D88, "SaveBlock2": 0xF2C, "PokemonStorage": 0x83D0}
TRIPLES = ("armv7a-linux-androideabi33", "aarch64-linux-android33")

UNIT = """#include "global.h"
#include "pokemon_storage_system.h"
struct SaveBlock1 gCheckSaveBlock1;
struct SaveBlock2 gCheckSaveBlock2;
struct PokemonStorage gCheckPokemonStorage;
"""


def layout(ndk_bin: Path, root: Path, triple: str, work: Path) -> dict[str, list[tuple[str, int]]]:
    source = work / "save_layout.c"
    obj = work / ("save_layout_%s.o" % triple)
    source.write_text(UNIT)
    flags = ["-DPORTABLE", "-DPLATFORM_3DS", "-DPLATFORM_ANDROID", "-DMODERN=1", "-DUBFIX", "-D__INTELLISENSE__",
             "-DPORT_BRIDGE", "-std=gnu99", "-fsigned-char", "-w", "-g", "-c",
             "-iquote", str(root / "3ds_port/compat"), "-iquote", str(root / "3ds_port/include"),
             "-iquote", str(root / "include"), "-iquote", str(root / "include/constants"), "-iquote", str(root)]
    subprocess.run([str(ndk_bin / ("%s-clang" % triple)), *flags, str(source), "-o", str(obj)], check=True)
    dump = subprocess.run([str(ndk_bin / "llvm-dwarfdump"), "--debug-info", str(obj)],
                          check=True, capture_output=True, text=True).stdout

    # Every DIE: its tag, name, member offset, type reference and size, and
    # its parent - dwarfdump indents a DIE's children under it.
    dies: dict[int, dict] = {}
    children: dict[int, list[int]] = {}
    stack: list[tuple[int, int]] = []  # (indent, DIE)
    current = None
    for line in dump.splitlines():
        m = re.match(r"^0x([0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)", line)
        if m:
            addr, indent, tag = int(m.group(1), 16), len(m.group(2)), m.group(3)
            while stack and stack[-1][0] >= indent:
                stack.pop()
            if tag == "NULL":
                current = None
                continue
            current = {"tag": tag}
            dies[addr] = current
            if stack:
                children.setdefault(stack[-1][1], []).append(addr)
            stack.append((indent, addr))
            continue
        if current is None:
            continue
        m = re.match(r"\s+(DW_AT_\w+)\s+\((.*)\)$", line)
        if not m:
            continue
        attr, value = m.groups()
        if attr == "DW_AT_name":
            current["name"] = value.strip('"')
        elif attr == "DW_AT_data_member_location":
            current["offset"] = int(value, 0)
        elif attr == "DW_AT_type":
            current["type"] = int(value.split()[0], 16)
        elif attr == "DW_AT_byte_size":
            current["size"] = int(value, 0)
        elif attr == "DW_AT_data_bit_offset":
            current["bits"] = int(value, 0)
    order = sorted(dies)

    def resolve(ref: int) -> int:
        """Through typedefs, const, volatile and arrays to the base type."""
        seen = 0
        while dies.get(ref, {}).get("tag") in ("DW_TAG_typedef", "DW_TAG_const_type",
                                                 "DW_TAG_volatile_type", "DW_TAG_array_type"):
            if "type" not in dies[ref] or seen > 32:
                break
            ref = dies[ref]["type"]
            seen += 1
        return ref

    def walk(aggregate: int, base: int, path: str, out: list[tuple[str, int]]) -> None:
        for member in children.get(aggregate, []):
            d = dies[member]
            if d["tag"] != "DW_TAG_member":
                continue
            name = path + "." + d.get("name", "?")
            offset = base + d.get("offset", 0)
            out.append((name, offset * 8 + d.get("bits", 0)))
            target = resolve(d.get("type", -1))
            tdie = dies.get(target, {})
            if tdie.get("tag") in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                walk(target, offset, name, out)
            elif tdie.get("tag") == "DW_TAG_pointer_type":
                out.append((name + " is a pointer", 0))

    result = {}
    for addr in order:
        d = dies[addr]
        if d.get("tag") == "DW_TAG_structure_type" and d.get("name") in SAVED and "size" in d:
            members: list[tuple[str, int]] = [("sizeof", d["size"])]
            walk(addr, 0, d["name"], members)
            result[d["name"]] = members
    return result


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ndk_bin, root, stamp = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    with tempfile.TemporaryDirectory() as tmp:
        a, b = (layout(ndk_bin, root, t, Path(tmp)) for t in TRIPLES)
    problems = []
    for name, gba_size in SAVED.items():
        if name not in a or name not in b:
            problems.append("%s: not found in the debug information" % name)
            continue
        for triple, members in zip(TRIPLES, (a[name], b[name])):
            if members[0][1] != gba_size:
                problems.append("%s is %d bytes on %s, the GBA's is %d" % (name, members[0][1], triple, gba_size))
            pointers = [m for m, _ in members if m.endswith(" is a pointer")]
            if pointers:
                problems.append("%s holds pointers on %s (they must be u32): %s"
                                % (name, triple, ", ".join(p[:-len(" is a pointer")] for p in pointers[:5])))
        if a[name] != b[name]:
            diff = [(x, y) for x, y in zip(a[name], b[name]) if x != y][:5]
            problems.append("%s differs between 32-bit ARM and arm64: %s" % (name, diff))
    if problems:
        sys.exit("check_save_layout: " + "\n  ".join(problems))
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text("".join("%s %d members\n" % (n, len(a[n])) for n in SAVED))
    print("check_save_layout: %s laid out as on the GBA on both architectures (%s members)"
          % (", ".join(SAVED), " + ".join(str(len(a[n]) - 1) for n in SAVED)))


if __name__ == "__main__":
    main()
