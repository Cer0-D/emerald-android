#!/usr/bin/env python3
"""Move the game's linked read-only data out of the executable.

The linker script (emerald3ds.ld.in) gathers the .rodata of every game
translation unit into one output section, .gamedata. The build links twice
with the same objects and the same layout: once with the section's contents
(the image, build/gamedata_image.elf) and once with the section reserved as
NOLOAD (the shipped executable). This script checks that both links placed
every symbol at the same address, then takes the section's bytes and its
relocation sites from the image and writes the bundle the console loads into
the reserved region before AgbMain (3ds_port/src/3ds_script_loader.c):

  romfs/gamedata/gamedata.bin   the payload (game data)
  romfs/gamedata/gamedata.rel   header and relocation sites (engine file)

The payload uses the same position-independent encoding as the script and
song bundles (scripts/ctr_bundle.py): pointers into the region are stored as
region-relative offsets, pointers elsewhere as link-time addresses.

An arm64 library (Android, CTR_PTR64=1) has two widths of pointer in the
section: the C units' (8 bytes, R_AARCH64_ABS64) and the assembled data's
32-bit addresses (R_AARCH64_ABS32), which lld leaves as zeros and only this
bundle carries. Its relocations are RELA, the value being the symbol's plus
the addend. Such a bundle is version 2: two more lists, of 8-byte sites.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctr_bundle import HEADER, HEADER_V2_EXTRA, REFERENCE_SYMBOL, VERSION, elf_symbols, elf_tool  # noqa: E402

MAGIC = b"C3GD"
SECTION = ".gamedata"


def sections(elf: Path) -> dict[str, tuple[str, int, int, int]]:
    out = subprocess.check_output([elf_tool("readelf"), "-S", "-W", str(elf)], text=True)
    found = {}
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            found[m.group(1)] = (m.group(2), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16))
    return found


def global_symbols(elf: Path) -> dict[str, int]:
    """Global symbols by name. Local labels are not compared: the NOLOAD link
    does not keep every local label of a section without contents."""
    out = subprocess.check_output([elf_tool("nm"), "--defined-only", str(elf)],
                                  text=True, errors="ignore")
    result = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1].isupper():
            result[parts[2]] = int(parts[0], 16)
    return result


RELOC_LINE = re.compile(r"^([0-9a-f]{8,16})\s+[0-9a-f]+\s+(R_[A-Z0-9_]+)\s*([0-9a-f]+)?\s*(\S+)?\s*(?:([+-])\s*([0-9a-f]+))?")


def relocations(elf: Path, section: str | None) -> list[tuple[int, str, int | None, str]]:
    """(site, type, symbol value + addend or None for REL, target section) of
    every relocation applying to `section` (all of them when None)."""
    out = subprocess.check_output([elf_tool("readelf"), "-r", "-W", str(elf)],
                                  text=True, errors="ignore")
    found = []
    target = None
    for line in out.splitlines():
        if line.startswith("Relocation section"):
            name = line.split("'")[1]
            target = name[len(".rela"):] if name.startswith(".rela") else name[len(".rel"):]
            continue
        if target is None or (section is not None and target != section):
            continue
        m = RELOC_LINE.match(line)
        if not m:
            continue
        value = None
        if m.group(3) is not None and m.group(5) is not None:
            addend = int(m.group(6), 16)
            value = int(m.group(3), 16) + (addend if m.group(5) == "+" else -addend)
        elif m.group(3) is not None and name.startswith(".rela"):
            value = int(m.group(3), 16)
        found.append((int(m.group(1), 16), m.group(2), value, target))
    return found


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, required=True, help="link with the section's contents")
    ap.add_argument("--elf", type=Path, required=True, help="shipped link, section NOLOAD")
    ap.add_argument("--out-dir", type=Path, required=True)
    args = ap.parse_args()

    img_secs = sections(args.image)
    elf_secs = sections(args.elf)
    if SECTION not in img_secs or SECTION not in elf_secs:
        raise SystemExit("gen_gamedata_bundle: %s missing from a link" % SECTION)
    kind, start, offset, size = img_secs[SECTION]
    if kind != "PROGBITS" or elf_secs[SECTION][0] != "NOBITS":
        raise SystemExit("gen_gamedata_bundle: expected PROGBITS in the image and NOBITS in the executable")
    if (start, size) != (elf_secs[SECTION][1], elf_secs[SECTION][3]):
        raise SystemExit("gen_gamedata_bundle: %s differs between the two links" % SECTION)
    a, b = global_symbols(args.image), global_symbols(args.elf)
    diff = sorted(name for name in set(a) | set(b) if a.get(name) != b.get(name))
    if diff:
        raise SystemExit("gen_gamedata_bundle: the two links disagree on %d global symbols, e.g. %s"
                         % (len(diff), diff[:10]))

    blob = args.image.read_bytes()
    payload = bytearray(blob[offset:offset + size])
    end = start + size
    internal, external, internal64, external64 = [], [], [], []
    unknown = {}
    # PC-relative relocations (jump tables, aarch64 literals) need nothing:
    # the region keeps its place relative to the code it is linked beside.
    relative = {"R_AARCH64_PREL32", "R_AARCH64_PREL64", "R_AARCH64_PREL16", "R_ARM_REL32", "R_ARM_PREL31"}
    for site, kind, value, _ in relocations(args.image, SECTION):
        rel = site - start
        if kind in ("R_ARM_ABS32", "R_ARM_TARGET1"):
            value = struct.unpack_from("<I", payload, rel)[0]
            width, lists = 4, (internal, external)
        elif kind == "R_AARCH64_ABS32":
            width, lists = 4, (internal, external)
        elif kind == "R_AARCH64_ABS64":
            width, lists = 8, (internal64, external64)
        elif kind in relative:
            continue
        else:
            unknown[kind] = unknown.get(kind, 0) + 1
            continue
        if value is None:
            raise SystemExit("gen_gamedata_bundle: no value for the %s at %08X" % (kind, site))
        fmt = "<I" if width == 4 else "<Q"
        if start <= value <= end:
            struct.pack_into(fmt, payload, rel, value - start)
            lists[0].append(rel)
        else:
            if width == 4 and value > 0xFFFFFFFF:
                raise SystemExit("gen_gamedata_bundle: a 32-bit address beyond 4 GiB at %08X" % site)
            struct.pack_into(fmt, payload, rel, value)
            lists[1].append(rel)
    if unknown:
        raise SystemExit("gen_gamedata_bundle: unsupported relocation types %s" % unknown)
    # A 32-bit address anywhere else is left zero by lld (android.mk): only
    # this bundle fills them in, so they must all be in the region.
    # (Debug sections are never loaded: their 32-bit offsets are DWARF's.)
    stray = sorted({target for _, kind, _, target in relocations(args.image, None)
                    if kind == "R_AARCH64_ABS32" and target != SECTION and not target.startswith(".debug")})
    if stray:
        raise SystemExit("gen_gamedata_bundle: 32-bit addresses outside %s, in %s" % (SECTION, stray))

    symbols = elf_symbols(args.elf)
    if REFERENCE_SYMBOL not in symbols:
        raise SystemExit("gen_gamedata_bundle: %s not found" % REFERENCE_SYMBOL)
    for sites in (internal, external, internal64, external64):
        sites.sort()
    wide = bool(internal64 or external64)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "gamedata.bin").write_bytes(payload)
    with (args.out_dir / "gamedata.rel").open("wb") as f:
        f.write(HEADER.pack(MAGIC, 2 if wide else VERSION, len(payload), symbols[REFERENCE_SYMBOL],
                            len(internal), len(external)))
        if wide:
            f.write(HEADER_V2_EXTRA.pack(len(internal64), len(external64)))
        for sites in (internal, external, internal64, external64):
            f.write(b"".join(struct.pack("<I", x) for x in sites))
    print("gen_gamedata_bundle: payload=%d bytes, internal=%d, external=%d, 64-bit %d + %d, region at %08X"
          % (len(payload), len(internal), len(external), len(internal64), len(external64), start))


if __name__ == "__main__":
    main()
