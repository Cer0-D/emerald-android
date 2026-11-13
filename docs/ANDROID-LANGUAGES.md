# Other languages

Emerald for Android is one app for every supported cartridge language. The
player picks their own ROM; the app recognizes which cartridge it is by its
SHA-1, builds that language's game data from it and plays it. There is no
way to switch languages in the app yet. No game data is distributed.

| Language | ROM | Game code | In the app |
|---|---|---|---|
| English | Pokémon Emerald (USA, Europe) | BPEE | yes |
| French | Pokémon Émeraude (France) | BPEF | yes |
| Italian | Pokémon Smeraldo (Italia) | BPEI | yes |
| German | Pokémon Smaragd (Deutschland) | BPED | yes |
| Spanish | Pokémon Esmeralda (España) | BPES | yes |
| Japanese | ポケモンエメラルド (日本) | BPEJ | not yet |

The checksums and the localization each language needs are the ones Emerald
3DS uses: [FRENCH.md](FRENCH.md), [ITALIAN.md](ITALIAN.md),
[GERMAN.md](GERMAN.md), [SPANISH.md](SPANISH.md), [JAPANESE.md](JAPANESE.md).
A Japanese ROM is recognized and the app says it is not supported yet.

## How it works

Each language is its own build of the game: its own library (`GAME_LANGUAGE`
and the localized sources are compiled in), its own engine files and its own
recipe (the clean ROM's SHA-1, and the literals pointing into that library).
The APK carries one set per language:

```
lib/arm64-v8a/libemerald_<code>.so       one game library per language
lib/arm64-v8a/libemeraldboot.so          loads the chosen one below 4 GiB
assets/lang/<code>/engine/               its engine files
assets/lang/<code>/builder/              recipe.json, literals.bin, abi.txt, rom.txt
```

On the device each language plays from its own root, `<files>/<code>/`
(engine files, and `sdmc/`: data pack, save, settings, follower files);
`<files>/language.txt` names the language played, which starts at once while
its data pack matches this build. The app asks for a ROM only when it has to
build a pack (first start, or an update that changed the data); the ROM picked
decides the language. Keeping each language's files apart means a ROM of
another language picked at that point cannot overwrite a save made with a
different cartridge. An update from the one-language app (0.5.0.1) moves its
files into `<files>/en/`.

## Building

```
android_port/tools/build.sh                  # development APK, installed over adb
android_port/tools/build.sh --release        # the release APK, audited against every ROM
```

The clean ROMs are found by SHA-1 in `--roms DIR` (default
`~/Documents/gba_roms`). `LANGS=en,es` builds only those languages; a
development build leaves out a language whose ROM is missing, a release
needs them all. English builds in the development tree, `build/upstream`;
each other language is staged once in `build/release-<code>` (the tree the
3DS release uses) with `tools/bootstrap.py --<language>-rom`, and its recipe
is made with the game's own reference ELF (`build/<language>-reference.s`) and
the image map. A language's engine files, recipe and voxel programs are only
made again when its library, data, ROM or the tools changed. The languages build
side by side, all at once (`PARALLEL=n` caps it; one takes about 600 MiB),
each logging to `build/android-<mode>/logs/<code>.log`.

The first build of a language regenerates its whole voxel world (its
tilesets differ a little: the Poké Center and Poké Mart signs, the shop and
Battle Dome lettering), which takes a while. A change to
`patches/pokeemerald/` stages every language tree again. A change to a game
source file a language manifest pins needs
`tools/rebase_locale_manifests.py`, or that language's build stops.

`podman` runs the voxel precompile in the release image. On a machine with the
pinned interpreter and Pillow (`build/venv`), `VOXEL_LOCAL=1` runs it directly.

The development app (`com.cer0.emerald.dev`, "Emerald Dev") plays ROMs pushed
over adb:

```
android_port/tools/dev_rom.sh "Pokemon - Version Emeraude (France).gba"
```

It keeps them in `<files>/roms/` and builds a language's pack again by itself
when a new build changed it.

## Status

Status of the French, Italian and German builds when they were separate
APKs (PRs #1-#3): each built, and each recipe rebuilt its
data pack byte for byte with the app's Java builder (`PackBuilder`) and with
the desktop builder (6,394 files for French and Italian, 6,393 for German;
ABI `770cc65b`, `a6ddd39a` and `dee20f98`).

All three have also run on the Android emulator (Android 13, with a second
display simulated through `overlay_display_devices`): the setup screen, the
title screens, the intro text, the in-game map and the bottom touch UI render
in the right language, and the pack is rebuilt from the cartridge on first
boot.
What is left is the Thor itself: the bottom screen's exact geometry, touch
input, audio and performance still need that hardware.
