#!/usr/bin/env bash
# A release APK: the library, the engine files and the recipe the app builds
# the game data from on first boot (Setup.java, PackBuilder.java) - never any
# game data. The 3DS release's steps (tools/build_release.py), for Android:
#
#   android_port/tools/release.sh ROM.gba
#
# 1. the library and the staged romfs/ (android.mk lib romfs-data);
# 2. the engine files of that staging (staging.py engine) -> assets/engine;
# 3. the recipe (tools/gen_recipe.py) from the staging and the ROM, with the
#    library's symbols for the game-data bundle's hints and the
#    original game's ELF for the voxel inputs (GBA_ELF, default
#    build/gba-elf/pokeemerald/pokeemerald.elf: the pinned pokeemerald built
#    with the pinned agbcc, rebuilding the supported ROM byte for byte);
# 4. the voxel programs (tools/voxel_precompile.py --outputs: the very files
#    this library was built with), in the pinned Python (upstream.lock
#    [python]) through podman;
# 5. the recipe unpacked for the app (export_recipe.py) -> assets/builder;
# 6. the APK, build/android-release/emerald-android.apk: not debuggable, signed
#    with the release key (KEYSTORE, KEYSTORE_PASS, KEY_ALIAS: build_apk.sh),
#    or with the debug key for testing - never to publish;
# 7. tools/release_audit.py over it.
#
# The ROM is read, never copied: into the container it goes read-only.
set -euo pipefail

ROM="$(realpath "${1:?usage: release.sh ROM.gba}")"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
TREE="$REPO/build/upstream/3ds_port"
OUT="$REPO/build/android-release"
GBA_ELF="${GBA_ELF:-$REPO/build/gba-elf/pokeemerald/pokeemerald.elf}"
PY="${PYTHON:-$REPO/build/venv/bin/python}"
NM="${NM:-llvm-nm}"
IMAGE="docker.io/library/$(sed -n 's/^image = "\(python:[^"]*\)"/\1/p' "$REPO/upstream.lock")"
PILLOW="$(sed -n 's/^pillow = "\([^"]*\)"/\1/p' "$REPO/upstream.lock")"

[[ -f "$GBA_ELF" ]] || { echo "release: no original game ELF at $GBA_ELF (GBA_ELF=...)" >&2; exit 1; }
mkdir -p "$OUT"

echo "== 1. library and staging"
make -C "$TREE" -f "$HERE/android.mk" -j"$(nproc)" lib romfs-data build/libemerald-stripped.so

echo "== 2. engine files"
rm -rf "$OUT/assets"
"$PY" "$REPO/tools/port_common/staging.py" engine --romfs "$TREE/romfs" --out "$OUT/assets/engine"
# A release reads its game data from the pack: no embedded-data marker.
rm -f "$OUT/assets/engine/data.embedded"

echo "== 3. recipe"
"$PY" "$REPO/tools/gen_recipe.py" --romfs "$TREE/romfs" --rom "$ROM" --out "$OUT/emerald3ds.recipe" \
    --release "android-$(git -C "$REPO" rev-parse --short HEAD)" --elf "$TREE/emerald3ds.elf" \
    --gba-elf "$GBA_ELF" --nm "$NM" --decomp "$REPO/build/upstream" --report "$OUT/recipe-report.txt"

echo "== 4. voxel programs ($IMAGE)"
podman run --rm --userns=keep-id -e HOME=/tmp -w "$REPO" \
    -v "$REPO:$REPO:Z" -v "$ROM:/rom.gba:ro" "$IMAGE" sh -c \
    "pip install -q --user --disable-pip-version-check pillow==$PILLOW && \
     python tools/voxel_precompile.py --recipe '$OUT/emerald3ds.recipe' --rom /rom.gba --port '$TREE' \
         --outputs '$TREE/romfs' --report '$OUT/precompile.json'"

echo "== 5. recipe for the app"
"$PY" "$HERE/tools/export_recipe.py" "$OUT/emerald3ds.recipe" "$OUT/assets/builder"

echo "== 6. APK"
APK_OUT="$OUT/emerald-android.apk" ASSETS="$OUT/assets" RELEASE=1 \
    bash "$HERE/build_apk.sh" "$TREE/build/libemerald-stripped.so"

echo "== 7. audit"
"$PY" "$REPO/tools/release_audit.py" --zip "$OUT/emerald-android.apk" --rom "$ROM" --strict \
    || { echo "release: the audit failed: do not publish this APK" >&2; exit 1; }
if [[ -z "${KEYSTORE:-}" ]]; then
    echo "release: $OUT/emerald-android.apk is signed with the DEBUG key: for testing only." >&2
    echo "release: publish only an APK built with KEYSTORE=... KEYSTORE_PASS=... (build_apk.sh)" >&2
    exit 2
fi
echo "release: $OUT/emerald-android.apk"
