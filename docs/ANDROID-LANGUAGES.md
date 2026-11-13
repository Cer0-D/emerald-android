# Other languages

Emerald for Android builds one variant per cartridge language, as Emerald 3DS
ships one 3DSX per language. Each variant is its own APK: the port's own
tables (font widths, keyboard, Pokédex order, credits) come from that
language's clean ROM at build time, and its recipe describes how the app
rebuilds the game's data from the player's own cartridge. No game data is
distributed in either.

| Language | ROM | Game code |
|---|---|---|
| English | Pokémon Emerald (USA, Europe) | BPEE |
| French | Pokémon Émeraude (France) | BPEF |
| Spanish | Pokémon Esmeralda (España) | BPES |
| Italian | Pokémon Smeraldo (Italia) | BPEI |
| German | Pokémon Smaragd (Deutschland) | BPED |
| Japanese | ポケモンエメラルド (日本) | BPEJ |

The checksums and the localization each language needs are the ones Emerald
3DS uses: [FRENCH.md](FRENCH.md), [SPANISH.md](SPANISH.md),
[ITALIAN.md](ITALIAN.md), [GERMAN.md](GERMAN.md), [JAPANESE.md](JAPANESE.md).
Brazilian Portuguese has no cartridge of its own; it is a translation over the
English game ([PORTUGUESE.md](PORTUGUESE.md)) and is built with `--locale pt`
from the English ROM.

## Building

```
android_port/tools/release.sh "Pokémon Émeraude (France).gba" --locale fr
```

The script stages the language in `build/release-fr` (the same tree the 3DS
release uses) with `tools/bootstrap.py --french-rom`, builds the library and
the data there, makes the recipe from that ROM with the game's own reference
ELF (`build/french-reference.s`) and the image map, and writes
`build/android-release-fr/emerald-android-fr.apk`. Running it again reuses the
staged tree. Without `--locale` it builds the English variant as before.

The variant installs as `com.cer0.emerald.<code>` and shows its own launcher
label, so two languages can stay installed side by side: a save from one game
never opens in another.

`podman` runs the voxel precompile in the release image. On a machine with the
pinned interpreter and Pillow (`build/venv`), `VOXEL_LOCAL=1` runs it directly.

## Status

The French and Italian variants build, and each recipe rebuilds its data pack
byte for byte with the app's Java builder (`PackBuilder`) and with the desktop
builder (6,394 files, ABI `770cc65b` for French and `a6ddd39a` for Italian).

Both have also run on the Android emulator (Android 13, with a second display
simulated through `overlay_display_devices`): the setup screen, the title
screens, the intro text, the in-game map and the bottom touch UI render in the
right language, and the pack is rebuilt from the cartridge on first boot.
What is left is the Thor itself: the bottom screen's exact geometry, touch
input, audio and performance still need that hardware.
