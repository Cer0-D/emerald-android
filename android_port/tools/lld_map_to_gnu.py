#!/usr/bin/env python3
"""Rewrite lld's link map in the shape of GNU ld's, as far as the asset
table generator reads it (tools/port_common/gen_asset_table.py: each
`.rodata.<symbol>` input section of a game object, then its address, size
and object on the next line).

    lld_map_to_gnu.py build/libemerald.lld.map build/emerald3ds.map
"""
import re
import sys

LINE = re.compile(
    r"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\d+\s+"
    r"(build/root/\S+\.o):\((\.rodata\.[A-Za-z_][A-Za-z0-9_]*)\)\s*$")


def main() -> None:
    src, dst = sys.argv[1], sys.argv[2]
    count = 0
    with open(src, encoding="utf-8", errors="ignore") as f, open(dst, "w", encoding="utf-8") as out:
        out.write("Linker script and memory map\n\n")
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            addr, size, obj, section = m.groups()
            out.write(" %s\n                0x%08x %#10x %s\n" % (section, int(addr, 16), int(size, 16), obj))
            count += 1
    print("lld_map_to_gnu: %d sections" % count)


if __name__ == "__main__":
    main()
