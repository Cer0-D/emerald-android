"""What another language's voxel generators may take from English's.

The languages' trees differ from English's in their translated text: in the
voxel world, a few tiles of it in the maps' drawing (signs). Where a
generator would read nothing but English's inputs, its work is English's,
and another language takes it from English's tree instead of doing it again
(inherit_relief.py: the terrain's relief; gen_voxel_buildings.py: the rooms'
pieces). This is what they share: whether everything but the tilesets'
drawing is English's, each tree's tilesets as the generators read them, and
the wait for English's tree to be built in the same run.

The environment says where English's tree is: VOXEL_INHERIT_FROM (its
3ds_port), and, in a run that builds it, VOXEL_INHERIT_TOKEN (the run's) and
VOXEL_INHERIT_WAIT (seconds; android_port/tools/build.sh sets them).
"""

from __future__ import annotations

import filecmp
import os
import struct
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def u16(path):
    raw = open(path, "rb").read()
    return struct.unpack("<%dH" % (len(raw) // 2), raw[:len(raw) // 2 * 2])


def english_tree():
    """English's 3ds_port, when this is another language's tree and it is
    named; None otherwise."""
    english = os.environ.get("VOXEL_INHERIT_FROM")
    if not english:
        return None
    english = os.path.abspath(english)
    if not os.path.isdir(english) or os.path.samefile(english, os.path.dirname(HERE)):
        return None
    return english


def same_tree(a, b, skip=()):
    """The first file that differs (or is in one tree only) under a and b,
    names in `skip` aside; None when none does."""
    def files(root):
        out = set()
        for d, dirs, names in os.walk(root):
            dirs[:] = [n for n in dirs if n != "__pycache__"]
            for n in names:
                if n not in skip and not n.endswith(".pyc"):
                    out.add(os.path.relpath(os.path.join(d, n), root))
        return out
    fa, fb = files(a), files(b)
    if fa != fb:
        return sorted(fa ^ fb)[0]
    for rel in sorted(fa):
        if not filecmp.cmp(os.path.join(a, rel), os.path.join(b, rel), shallow=False):
            return rel
    return None


def same_inputs(english, mine):
    """None when everything the voxel generators read is English's but the
    tilesets' tiles and metatiles (Drawing compares those); otherwise what
    differs: the voxel scripts and assets, the layouts and their block data,
    every map's map.json, every tileset's palettes, attributes and
    animations, and the headers that say where the tilesets are."""
    eroot, mroot = os.path.dirname(english), os.path.dirname(mine)
    for sub in ("scripts", os.path.join("assets", "voxel")):
        diff = same_tree(os.path.join(english, sub), os.path.join(mine, sub))
        if diff:
            return "3ds_port/%s/%s differs" % (sub, diff)
    diff = same_tree(os.path.join(eroot, "data", "layouts"), os.path.join(mroot, "data", "layouts"))
    if diff:
        return "data/layouts/%s differs" % diff
    maps = os.path.join(mroot, "data", "maps")
    if sorted(os.listdir(os.path.join(eroot, "data", "maps"))) != sorted(os.listdir(maps)):
        return "data/maps has other maps"
    for name in sorted(os.listdir(os.path.join(eroot, "data", "maps"))):
        a = os.path.join(eroot, "data", "maps", name, "map.json")
        b = os.path.join(maps, name, "map.json")
        if os.path.exists(a) != os.path.exists(b) or (os.path.exists(a) and not filecmp.cmp(a, b, shallow=False)):
            return "data/maps/%s/map.json differs" % name
    diff = same_tree(os.path.join(eroot, "data", "tilesets"), os.path.join(mroot, "data", "tilesets"),
                     skip=("tiles.4bpp", "tiles.4bpp.lz", "tiles.png", "metatiles.bin"))
    if diff:
        return "data/tilesets/%s differs" % diff
    diff = same_tree(os.path.join(eroot, "src", "data", "tilesets"), os.path.join(mroot, "src", "data", "tilesets"))
    if diff:
        return "src/data/tilesets/%s differs" % diff     # where the tilesets are
    return None


class Drawing:
    """A tree's tilesets as the generators read them: each tileset's tiles
    (32 bytes each) and metatile entries (eight each), by tileset name."""

    def __init__(self, root):
        self.root = root
        self._meta, self._tiles = {}, {}

    def _load(self, name):
        """As the generators find them: the tiles where voxel_art's
        tileset_dir looks, the metatiles where metatiles.h's INCBIN says."""
        import voxel_art as va
        if name not in self._meta:
            d = va.tileset_dir(name) if name else None
            tiles = os.path.join(self.root, os.path.relpath(d, va.ROOT), "tiles.4bpp") if d else ""
            meta = os.path.join(self.root, os.path.relpath(va.METATILES[name], va.ROOT)) \
                if name in va.METATILES else ""
            self._meta[name] = u16(meta) if os.path.exists(meta) else ()
            self._tiles[name] = open(tiles, "rb").read() if os.path.exists(tiles) else b""
        return self._meta[name], self._tiles[name]

    def tile(self, tileset, t):
        return self._load(tileset)[1][t * 32:t * 32 + 32]

    def entries(self, tileset, m):
        return tuple(self._load(tileset)[0][m * 8:m * 8 + 8])


def drawn_alike(ed, md, primary, secondary, m):
    """Whether metatile m of a primary/secondary pair draws the same in both
    trees (Drawing): the same entries, drawing the same tiles."""
    tileset, index = (primary, m) if m < 512 else (secondary, m - 512)
    entries = ed.entries(tileset, index)
    if entries != md.entries(tileset, index):
        return False
    for e in entries:
        t = e & 0x3FF
        ts, i = (primary, t) if t < 512 else (secondary, t - 512)
        if ed.tile(ts, i) != md.tile(ts, i):
            return False
    return True


def wait_for(ready, token, label):
    """Wait until the file `ready` holds `token` (English's tree built in this
    run): True then; False when it says "failed <token>" or after
    VOXEL_INHERIT_WAIT seconds."""
    limit = time.time() + float(os.environ.get("VOXEL_INHERIT_WAIT", "1800"))
    said = False
    while True:
        try:
            said_ready = open(ready).read().strip()
            if said_ready == token:
                return True
            if said_ready == "failed " + token:
                return False
        except OSError:
            pass
        if time.time() > limit:
            return False
        if not said:
            print("%s: waiting for the English tree" % label, flush=True)
            said = True
        time.sleep(2)
