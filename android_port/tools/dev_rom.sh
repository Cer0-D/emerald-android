#!/usr/bin/env bash
# Plays a ROM in the development app (com.cer0.emerald.dev, built by
# tools/build.sh): pushes it into the app's <files>/roms/ and starts the app
# on it. The app finds its language by SHA-1, builds that language's data
# pack (the first time, or when a build changed its recipe) and plays it,
# with that language's own save and settings (<files>/<code>/).
#
#   android_port/tools/dev_rom.sh ROM.gba
#
# The ROM stays in roms/, so later starts of the app play the same language
# without pushing it again. ADB_SERIAL picks the device.
set -euo pipefail

ROM="${1:?usage: dev_rom.sh ROM.gba}"
[[ -f "$ROM" ]] || { echo "dev_rom: no file $ROM" >&2; exit 1; }
PKG=com.cer0.emerald.dev
D=/sdcard/Android/data/$PKG/files/roms
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
# Named by its SHA-1: no spaces or accents for the shell on the device, and
# pushing the same ROM again replaces it.
NAME="$(sha1sum "$ROM" | cut -d' ' -f1).gba"

"${ADB[@]}" shell pm path "$PKG" >/dev/null || { echo "dev_rom: $PKG is not installed (tools/build.sh)" >&2; exit 1; }
# adb push cannot chown into the app's dir: through a shell instead.
"${ADB[@]}" exec-in "mkdir -p $D && cat > $D/$NAME.tmp" < "$ROM"
"${ADB[@]}" shell "mv $D/$NAME.tmp $D/$NAME && chmod 660 $D/$NAME"
echo "dev_rom: $ROM -> $D/$NAME"
# -S: the app stopped first, so it starts on this ROM in a fresh process.
"${ADB[@]}" shell am start -S -n "$PKG/com.cer0.emerald.MainActivity" --es rom "$NAME" >/dev/null
echo "dev_rom: started $PKG on it"
