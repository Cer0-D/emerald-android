#!/usr/bin/env python3
"""Another language's relief.bin when it would be English's: copied, not made.

The terrain's relief (gen_voxel_relief.py) reads the maps' drawing, their
layouts and events, and which cells the buildings' models own. The
languages' maps differ only in a few tiles of translated text - the Pokemon
Centers' and Marts' signs, the gyms' signposts - which are drawn on a
building's model or a signpost, cells the relief leaves to them; measured,
it comes out byte for byte the same with those tiles blanked. So a
language's relief is English's when everything it reads is English's but
tiles drawn only in such cells:

  * the voxel scripts and assets, the layouts and their block data, every
    map's map.json and every tileset's palettes, attributes and animations
    are the same files;
  * where a tileset differs - a tile English's relief decoded, a metatile
    whose entries it read (build/relief.reads: gen_voxel_relief.py's
    VOXEL_RELIEF_READS, every reader of the tilesets recorded) - every map
    cell drawing that tile or metatile is a cell a building's model owns,
    in both languages' buildings.bin, or a signpost (what the relief does
    not read can differ as it likes);
  * the cells the models own are the same.

Then English's relief.bin is copied to OUTPUT and this exits 0; otherwise it
says what differs and exits 1, and the relief is made (full.mk). English's
must be current: with VOXEL_INHERIT_READY (a file) and VOXEL_INHERIT_TOKEN,
it waits for the file to hold the token - written by android_port/tools/
build.sh once English's tree is built in the same run - for up to
VOXEL_INHERIT_WAIT seconds.

  VOXEL_INHERIT_FROM=<English 3ds_port> inherit_relief.py --output romfs/voxel/relief.bin [--check]
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


from voxel_inherit import Drawing, same_inputs, u16, wait_for  # noqa: E402


def covered_cells(path):
    """{layout index: cells a building's model owns} of a buildings.bin."""
    import voxel_engine_view as ev
    bld = ev.Buildings(path)
    out = {}
    for lay, pm, x, y, *_ in bld.placements:
        model = bld.page_models[pm][0]
        w, h = bld.models[model][:2]
        for j in range(h):
            for i in range(w):
                if bld.heights[bld.model_heights[model] + j * w + i] != 0xFF:
                    out.setdefault(lay, set()).add((x + i, y + j))
    return out


def why_not(english, mine):
    """None when this tree's relief is English's (see above); otherwise what
    differs."""
    eroot, mroot = os.path.dirname(english), os.path.dirname(mine)
    reason = same_inputs(english, mine)
    if reason:
        return reason

    ecov = covered_cells(os.path.join(english, "romfs", "voxel", "buildings.bin"))
    mcov = covered_cells(os.path.join(mine, "romfs", "voxel", "buildings.bin"))
    if ecov != mcov:
        return "the buildings' models own other cells"

    # what the relief read of the tilesets, where they differ: only on a
    # model or a signpost
    reads_path = os.path.join(english, "build", "relief.reads")
    if not os.path.exists(reads_path):
        return "English's relief did not record what it reads (build/relief.reads)"
    if os.path.getmtime(reads_path) < os.path.getmtime(os.path.join(english, "romfs", "voxel", "relief.bin")) - 60:
        return "English's record of what its relief reads is older than its relief"
    tiles_read, entries_read = set(), set()
    for line in open(reads_path):
        kind, tileset, index = line.split()
        (tiles_read if kind == "T" else entries_read).add((tileset, int(index)))
    import voxel_cells as vc
    import voxel_relief_fixes as vrf
    ed, md = Drawing(eroot), Drawing(mroot)
    changed = {}     # (tileset, tile) / ("E", tileset, metatile) -> differs and was read

    def tile_read_differs(tileset, t):
        key = (tileset, t)
        if key not in changed:
            a, b = ed.tile(tileset, t), md.tile(tileset, t)
            changed[key] = a != b and key in tiles_read
        return changed[key]

    def entries_read_differ(tileset, m):
        key = ("E", tileset, m)
        if key not in changed:
            changed[key] = ed.entries(tileset, m) != md.entries(tileset, m) and (tileset, m) in entries_read
        return changed[key]

    def matters(p, s_, m):
        """Whether the relief would see this metatile drawn otherwise."""
        tileset, index = (p, m) if m < 512 else (s_, m - 512)
        if entries_read_differ(tileset, index):
            return True
        for ents in (ed.entries(tileset, index), md.entries(tileset, index)):
            for e in ents:
                t = e & 0x3FF
                if tile_read_differs(p if t < 512 else s_, t if t < 512 else t - 512):
                    return True
        return False

    events = vc.MapEvents()
    n = 0
    for entry in vc.load_layouts():
        if not entry.get("id"):
            continue
        path = os.path.join(mroot, entry["blockdata_filepath"])
        if not os.path.exists(path):
            continue
        p, s_ = entry.get("primary_tileset"), entry.get("secondary_tileset")
        blocks = u16(path)
        width = entry["width"]
        own = mcov.get(vrf.layout_index(entry["id"]), set())
        layout = None
        seen = {}
        for k, cell in enumerate(blocks):
            m = cell & 0x3FF
            if m not in seen:
                seen[m] = matters(p, s_, m)
            if not seen[m]:
                continue
            xy = (k % width, k // width)
            if xy in own:
                n += 1
                continue
            if layout is None:
                layout = vc.Layout(entry, events)
            if layout.role_at(*xy) == "signpost":
                n += 1
                continue
            return "%s %d,%d (metatile 0x%03x) is drawn otherwise where the relief reads it, " \
                   "on no model or signpost" % (entry["id"], xy[0], xy[1], m)
    print("inherit_relief: %d cell(s) drawn otherwise where the relief reads them, "
          "all on a model or a signpost" % n)
    return None


def wait_ready():
    ready, token = os.environ.get("VOXEL_INHERIT_READY"), os.environ.get("VOXEL_INHERIT_TOKEN")
    if not ready or not token:
        return True
    return wait_for(ready, token, "inherit_relief")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--output", required=True)
    ap.add_argument("--check", action="store_true", help="say whether it would be inherited, copy nothing")
    args = ap.parse_args()
    english = os.environ.get("VOXEL_INHERIT_FROM")
    if not english:
        return 1
    english = os.path.abspath(english)
    mine = os.path.dirname(HERE)
    if os.path.samefile(english, mine):
        return 1
    if not wait_ready():
        print("inherit_relief: the English tree was not built (or not in time): made here", flush=True)
        return 1
    source = os.path.join(english, "romfs", "voxel", "relief.bin")
    if not os.path.exists(source):
        print("inherit_relief: English has no relief.bin: made here", flush=True)
        return 1
    reason = why_not(english, mine)
    if reason:
        print("inherit_relief: %s: made here" % reason, flush=True)
        return 1
    if args.check:
        print("inherit_relief: would be English's", flush=True)
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    tmp = args.output + ".inherit"
    shutil.copyfile(source, tmp)
    os.replace(tmp, args.output)
    print("inherit_relief: %s is English's" % args.output, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
