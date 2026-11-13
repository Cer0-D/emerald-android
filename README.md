<h1 align="center">Emerald for Android</h1>

<p align="center">
  <strong>Hoenn on two screens, natively on Android.</strong><br>
  An Android port of <a href="https://github.com/ZallaxDev/emerald-3ds">Emerald 3DS</a> for dual-screen handhelds, starting with the Ayn Thor.
</p>

> **Work in progress.** This port is under active development. Some things may
> not work correctly yet, and some features of Emerald 3DS are not ported yet
> (see [Status](#status)). Back up your save before updating, and expect bugs.

---

Emerald for Android runs the Pokémon Emerald decompilation **natively** on
Android - no emulator - with the same two-screen design as Emerald 3DS: the
game on the top screen, and a touch interface on the bottom screen in place of
the START menu. The optional **voxel overworld** lets you explore Hoenn from a
new angle, rendered at the device's full resolution.

## What's new in 0.5.0.1

- **Local wireless play over Bluetooth.** Two devices meet in the Pokémon
  Center's link rooms with no router or internet: Bluetooth LE finds and
  connects them. Trades and battles are not fully tested yet (see
  [Local wireless play](#local-wireless-play)).
- **64-bit (arm64).** The app is now a 64-bit build, so it installs on newer
  devices that no longer run 32-bit apps. Saves are the same file as before.
- **Much faster 3D.** The voxel world's drawing costs a fraction of what it
  did: the busiest places now hold 60 fps where they used to drop to 30.
- **Day/night lighting fixed.** The outdoor light now follows the clock
  continuously.
- **The PokéNav** opens in its window on the bottom screen, as on
  Emerald 3DS.
- **FPS counter** on the top screen, an option in the bottom screen's
  settings, as on Emerald 3DS.
- **An app icon**: the Emerald 3DS HOME Menu icon.

Updating from 0.5.0 keeps your save, but the app asks for the ROM once more:
the 64-bit version builds its game data anew.

## Goal of the project

The goal is to **port everything Emerald 3DS does to Android**. Every feature
of Emerald 3DS is meant to come over in time. Alongside that, this project may
add **features of its own** that exist only in the Android version (for
example **3D SPRITES**, below). So: every Emerald 3DS feature, ported as
faithfully as possible, plus a few extras.

## Getting started

Everything happens inside the app: **no website, no desktop tool, no separate
builder.**

1. Download the latest APK from this repository's
   [Releases](../../releases/latest) page and install it.
2. Put your **own clean, supported Pokémon Emerald ROM** somewhere on the
   device (see [supported cartridges](#supported-cartridges)).
3. Open the app. On first launch it asks for the ROM: tap **Choose ROM** and
   pick it.
4. The app builds the game's data from your ROM on the device (a progress bar
   shows each step), then starts the game. From then on it opens straight into
   the game.

The ROM is only read, to build the game data: it is never copied, modified,
uploaded or sent anywhere. The APK itself contains no game content; it only
holds the port's code and a *recipe* describing how to rebuild the game data
from your own cartridge.

After an update that changes the game data, the app asks for the ROM again
and rebuilds it. Your save is kept.

### Requirements

- Android 13 or newer, with OpenGL ES 3.0, on a 64-bit ARM device
  (`arm64-v8a`).
- Bluetooth LE for local wireless play (optional).
- **Target device: the [Ayn Thor](https://www.ayntec.com/).** The port is
  developed, laid out and tested on it.
- **Only dual-screen devices are supported for now**: the bottom screen's
  interface needs a second display. Other dual-screen devices may work but are
  untested. A **single-screen mode** for ordinary phones and handhelds is
  planned.

### Supported cartridges

Only a clean, unmodified dump is accepted:

| Language | Cartridge | Game code |
| :--- | :--- | :--- |
| English | Pokémon Emerald Version (USA, Europe) | BPEE |

Emerald 3DS also supports Spanish, French, Italian, German, Japanese and a
Brazilian Portuguese translation; those are not available in the Android
version yet. ROM hacks are not supported.

## Features

| | What to expect |
| :--- | :--- |
| **Native Android** | The game code runs natively on the device's ARM CPU, drawn with OpenGL ES 3 and played through AAudio. No emulator. |
| **Two screens, one adventure** | The game on the top screen, the touch interface on the bottom screen. On the Thor the bottom interface is laid out for its taller screen: bigger party entries, a longer bag list and larger settings rows. |
| **Built on the device** | First launch turns your own ROM into the game's data inside the app. |
| **Optional voxel overworld** | Modelled buildings, trees, signposts and terrain relief from Emerald 3DS, rendered at the screen's full resolution, with adjustable camera angle and zoom. |
| **Full-screen presentation** | The intro, title, menus, naming screen, clock and other full-screen scenes fill the whole top screen. |
| **HD battle interface** | Battles laid out for the wide screen, with optional 3D battles over the voxel world. |
| **Day/night lighting** | Optional outdoor lighting, lit windows and street lamps, and a faster clock, as in Emerald 3DS. |
| **Local wireless play** | The Pokémon Center's link rooms between two devices nearby, over Bluetooth LE: no router, no internet. |
| **Enhancements** | Physical/special split, IVs and EVs on the summary, infinite TMs, running indoors, Repel prompt, visible wild Pokémon, fast-forward (ZR/ZL) and more, all optional. |
| **3D SPRITES** *(Android only)* | People and Pokémon in the voxel world drawn 1 to 3 voxels deep instead of flat (OPTION → WORLD → 3D). |

### A new perspective on Hoenn

The voxel overworld is **off by default**. Open **OPTION** on the bottom
screen and enable **VOXEL 3D** (WORLD → 3D), then adjust **3D ANGLE** and
**3D ZOOM**. **3D SPRITES** gives the characters depth.

> **3D mode is a work in progress** in Emerald 3DS itself: many towns and
> routes have modelled buildings and relief, but large parts of the map and
> most interiors are still flat.

### Local wireless play

Two devices with the app play together over Bluetooth, the way two 3DS
consoles do over local wireless: walk into a Pokémon Center's link rooms on
both and they find each other. No router or internet is needed, only
Bluetooth. The first time, the app asks for the **Nearby devices**
permission, and to turn Bluetooth on if it is off.

Both devices need the **same version** of the app: different versions do not
link. Going to the background (the home screen, another app) leaves the
session.

What works so far: both devices join the link rooms, see and walk among each
other, and leave cleanly, even when one side quits. **Trades, battles and the
minigames (Berry Blender, Pokémon Jump, Dodrio Berry Picking) are not fully
tested yet**: they will most likely work, but back up your save before trying
them.

## Versioning

Releases follow Emerald 3DS's version numbers: **0.5.0** here is Emerald
3DS 0.5.0 ported to Android, and each new Emerald 3DS release is ported as
its own version. Features added only in this project get a fourth number on
top of the version they build on: **0.5.0.1**, **0.5.0.2**...

## Status

What works and what does not yet, compared with Emerald 3DS:

| | Status |
| :--- | :--- |
| Game, both screens, touch interface | Working |
| Voxel overworld and 3D battles | Working |
| Sound and music | Working |
| Fast-forward | Working |
| Day/night lighting | Working |
| Building the game data in the app | Working (English cartridge) |
| Other languages (ES, FR, IT, DE, JA, PT-BR) | Not ported yet |
| Follower Pokémon (HeartGold/SoulSilver sprites) | Not ported yet: the app does not download them |
| Local wireless play (Bluetooth) | Working: link rooms. Trades, battles and minigames not fully tested |
| Single-screen devices | Not supported yet (planned) |
| Stereoscopic 3D | Not applicable (New 3DS only) |

Report problems in this repository's issues, with your device, the app
version and what you were doing. Please do not report problems with this
port to the Emerald 3DS project.

## About this repository

**This repository contains no game content.** It holds the port's own code,
the tools that build it, and the recipe tooling that lets the app rebuild the
game data from *your own* cartridge. Nothing derived from the ROM is
distributed here or in the releases.

| Component | Location | Role |
| :--- | :--- | :--- |
| Engine | [pret/pokeemerald](https://github.com/pret/pokeemerald) · [`upstream.lock`](upstream.lock) | The Pokémon Emerald decompilation, pinned to a specific revision. |
| Port patches | [`patches/pokeemerald/`](patches/pokeemerald) | Emerald 3DS's changes to the engine, plus the Android build's (`0090-android-clang.patch`). |
| Shared port code | [`3ds_port/`](3ds_port) | Emerald 3DS's game bridge, touch interface and voxel overworld, which the Android build reuses. |
| Android backend | [`android_port/`](android_port) | Video (OpenGL ES 3), audio (AAudio), input, the two-screen shell, and the in-app data builder. |
| Data builder | [`builder/`](builder/) | Emerald 3DS's builder; the recipe format the app's builder (`android_port/app/java/.../PackBuilder.java`) follows. |

## Building from source

```sh
python tools/bootstrap.py --make       # pinned upstream + patches + port -> build/upstream
cd build/upstream/3ds_port
make -f ../../../android_port/android.mk -j16 push-data  # the whole game data, once, over adb
make -f ../../../android_port/android.mk -j16 install    # the development APK
```

It needs the Android SDK and NDK (`ANDROID_HOME`, default `~/Android/Sdk`),
a Python with Pillow in `build/venv`, and libclang's Python bindings for the
system `python3` (the arm64 build's layout check, `android_port/tools/check_overlays.py`).
A release also needs podman.

A development install reads the game data pushed to the device over adb,
built from your own ROM by the tree above. A release APK is made with
`android_port/tools/release.sh ROM.gba`: the library, the engine files and
the recipe the app builds the game data from, checked by
`tools/release_audit.py`. It reports each step and its time as it goes.

The build is 64-bit (`arm64-v8a`). `ANDROID_ABI=armeabi-v7a` still builds
the 32-bit library, unsupported; build it in a tree of its own, as the two
share `build/`.

The [development guide](docs/DEVELOPMENT.md), [architecture](docs/ARCHITECTURE.md)
and [asset pipeline](docs/ASSET_PIPELINE.md) documents come from Emerald 3DS
and describe the shared code; the Android-specific parts live in
`android_port/`.

## Credits

This project is a fork of **[Emerald 3DS](https://github.com/ZallaxDev/emerald-3ds)
by Zallax and contributors**, and would not exist without it: the touch
interface, the voxel overworld, the enhancements, the data builder and the
recipe system are their work. This fork ports it to Android: a new
Android backend (video, audio, input, two-screen shell), an in-app data
builder, layouts for the Thor's screens, and Android-only features.

If you enjoy this, consider supporting the original project: follow
[@DustZallax on X](https://x.com/DustZallax), join the
[Emerald 3DS Discord](https://discord.com/invite/tfqHF8496P), or
[buy Zallax a coffee](https://ko-fi.com/zallax).

Also built on:

- **[pret/pokeemerald](https://github.com/pret/pokeemerald)**: the Pokémon
  Emerald decompilation the game runs on.
- **[pokeemerald-multiplatform](https://github.com/gradenGnostic/pokeemerald-multiplatform)**
  (MIT): the voxel renderer's original logic, adapted by Emerald 3DS
  (see [`3ds_port/src/voxel/NOTICE.md`](3ds_port/src/voxel/NOTICE.md)).

## Licence

The original Emerald 3DS code, tools and documentation, and this fork's own
additions, are licensed under [CC BY-NC-SA 4.0](LICENSE-PORT.md): you may
fork, change and share them with credit, not for profit, and under the same
licence. The decompilation, the game and third-party components keep their
own terms; see [NOTICE.md](NOTICE.md) and [docs/PROVENANCE.md](docs/PROVENANCE.md).
AI-assisted tooling was used during development: [AI_DISCLOSURE.md](AI_DISCLOSURE.md).

Emerald for Android is an unofficial fan project, not affiliated with or
endorsed by Nintendo, Game Freak, Creatures, The Pokémon Company or the
Emerald 3DS project. Pokémon and Pokémon Emerald are trademarks of their
respective owners.
