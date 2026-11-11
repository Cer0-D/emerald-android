#!/usr/bin/env python3
"""A release recipe as the APK's builder reads it.

    export_recipe.py RECIPE OUTDIR

The desktop builder's recipe (builder/emerald3ds_builder/recipe.py) is LZMA
over JSON; the app (PackBuilder.java) has no LZMA, and the APK is a zip that
compresses its assets anyway. So it gets the same recipe unpacked:

    OUTDIR/recipe.json   the metadata (entries, inputs, generated, bitmaps)
    OUTDIR/literals.bin  the literal pool
    OUTDIR/abi.txt       the engine ABI, hex (what Setup.java checks a pack against)

Every generated file must carry its program (tools/voxel_precompile.py): the
app cannot run the generators.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import Recipe  # noqa: E402


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    recipe = Recipe.load(Path(sys.argv[1]))
    out = Path(sys.argv[2])
    missing = [e["path"] for e in recipe.generated if not e.get("ops")]
    if missing:
        raise SystemExit("export_recipe: no program for %s (run tools/voxel_precompile.py)" % ", ".join(missing))
    out.mkdir(parents=True, exist_ok=True)
    meta = {"engine_abi": recipe.engine_abi, "rom_sha1": recipe.rom_sha1, "release": recipe.release,
            "entries": recipe.entries, "bitmaps": recipe.bitmaps, "generated": recipe.generated,
            "inputs": recipe.inputs}
    (out / "recipe.json").write_text(json.dumps(meta, separators=(",", ":")), encoding="utf-8")
    (out / "literals.bin").write_bytes(recipe.literals)
    (out / "abi.txt").write_text("%08x\n" % recipe.engine_abi, encoding="ascii")
    print("export_recipe: %d files, %d inputs, %d generated, %d literal bytes, ABI %08x"
          % (len(recipe.entries), len(recipe.inputs), len(recipe.generated), len(recipe.literals),
             recipe.engine_abi))


if __name__ == "__main__":
    main()
