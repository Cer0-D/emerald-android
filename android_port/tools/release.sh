#!/usr/bin/env bash
# A release APK: the library, the engine files and the recipe the app builds
# the game data from on first boot (Setup.java, PackBuilder.java) - never any
# game data. The 3DS release's steps (tools/build_release.py), for Android:
#
#   android_port/tools/release.sh ROM.gba [--locale CODE]
#
# With --locale (es, fr, it, de, ja or pt), the port is built for that
# language's own clean ROM: the tree is bootstrapped and localized in
# build/release-<code> (tools/bootstrap.py), the recipe is made from that ROM
# with the game's own reference ELF (build/<stem>-reference.s) and the image
# map, and the APK is written to build/android-release-<code>/. It installs
# beside the English app (com.cer0.emerald.<code>), so a French or Italian
# save never opens in another language's game.
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

ROM="$(realpath "${1:?usage: release.sh ROM.gba [--locale CODE]}")"
shift || true
LOCALE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --locale) LOCALE="${2:?--locale needs a code: es, fr, it, de, ja or pt}"; shift 2 ;;
        -h|--help) echo "usage: release.sh ROM.gba [--locale CODE]"; exit 0 ;;
        *) echo "release: unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$LOCALE" in
    ""|es|fr|it|de|ja|pt) ;;
    *) echo "release: unknown locale: $LOCALE" >&2; exit 2 ;;
esac
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
# English: the development tree. Another language: its own tree, so staging
# one does not reset the other.
TREE_ROOT="$REPO/build/upstream"
[[ -z "$LOCALE" ]] || TREE_ROOT="$REPO/build/release-$LOCALE"
TREE="$TREE_ROOT/3ds_port"
OUT="$REPO/build/android-release${LOCALE:+-$LOCALE}"
GBA_ELF="${GBA_ELF:-$REPO/build/gba-elf/pokeemerald/pokeemerald.elf}"
PY="${PYTHON:-$REPO/build/venv/bin/python}"
# llvm-nm reads both the arm64 library and the GBA's ARM ELF; the NDK has it.
NDK_BIN="${NDK_BIN:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/*/toolchains/llvm/prebuilt/*/bin 2>/dev/null | sort -V | tail -1)}"
NM="${NM:-$NDK_BIN/llvm-nm}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )}"
IMAGE="docker.io/library/$(sed -n 's/^image = "\(python:[^"]*\)"/\1/p' "$REPO/upstream.lock")"
PILLOW="$(sed -n 's/^pillow = "\([^"]*\)"/\1/p' "$REPO/upstream.lock")"

mkdir -p "$OUT"
# What the build is doing, as it does it: each step's time when it ends, the
# Python steps' own lines as they print them (unbuffered, into a log too).
export PYTHONUNBUFFERED=1
STEP=""
STEP_START=$SECONDS
step() {
    [[ -z "$STEP" ]] || printf '   (%s: %s)\n' "$STEP" "$(elapsed $((SECONDS - STEP_START)))"
    STEP="$1"
    STEP_START=$SECONDS
    [[ -z "$1" ]] || echo "== $1"
}
elapsed() { printf '%dm%02ds' $(($1 / 60)) $(($1 % 60)); }

# The language's tree. tools/bootstrap.py resets it when the patches, the pin
# or the language changed. A current tree is reused, but its copy of the port
# is refreshed on every release: bootstrap.py's last step copies 3ds_port/,
# builder/ and tools/ into the tree, and skipping that would build the second
# release from the first one's snapshot.
bootstrap_locale() {
    local flag game pin head marker digest current
    case "$LOCALE" in
        es) flag=--spanish-rom;  game=BPES ;;
        fr) flag=--french-rom;   game=BPEF ;;
        it) flag=--italian-rom;  game=BPEI ;;
        de) flag=--german-rom;   game=BPED ;;
        ja) flag=--japanese-rom; game=BPEJ ;;
        pt) flag=--port-lang;    game=PT-BR ;;
    esac
    pin=$("$PY" -c 'import sys,tomllib;print(tomllib.load(open(sys.argv[1],"rb"))["pokeemerald"]["commit"])' \
        "$REPO/upstream.lock")
    head=$(git -C "$TREE_ROOT" rev-parse HEAD 2>/dev/null || true)
    marker=$(cat "$TREE_ROOT/.emerald3ds-patches" 2>/dev/null || true)
    digest=$("$PY" - "$REPO" <<'PY'
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(sys.argv[1]) / "tools"))
import bootstrap
print(bootstrap.patch_digest(sorted((pathlib.Path(sys.argv[1]) / "patches" / "pokeemerald").glob("*.patch"))))
PY
)
    current=0
    if [[ "$head" == "$pin" && "$marker" == "$digest" && -f "$TREE_ROOT/.emerald3ds-locale" ]] \
        && grep -q "$game" "$TREE_ROOT/.emerald3ds-locale"; then
        current=1
    fi
    if [[ "$current" == 0 ]]; then
        echo "release: staging the $LOCALE tree in $TREE_ROOT"
        if [[ "$LOCALE" == pt ]]; then
            "$PY" "$REPO/tools/bootstrap.py" --dir "$TREE_ROOT" --port-lang pt_br --python "$PY" -j"$JOBS"
        else
            "$PY" "$REPO/tools/bootstrap.py" --dir "$TREE_ROOT" "$flag" "$ROM" --python "$PY" -j"$JOBS"
        fi
    fi
    # bootstrap.py's last step, on every release: the tree must carry the
    # 3ds_port/, builder/ and tools/ being released, not the ones it was
    # staged with.
    "$PY" - "$REPO" "$TREE_ROOT" <<'PY'
import pathlib, shutil, sys
root, tree = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for name in ("3ds_port", "builder", "tools"):
    if (root / name).is_dir():
        shutil.copytree(root / name, tree / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "build", "dist"))
PY
}

# A ROM that is not the English one needs its own symbol table (--gba-elf: the
# localization writes it as assembly) and the image's map, which the literal
# classification reads.
REFERENCE="$GBA_ELF"
RECIPE_EXTRA=()
if [[ -n "$LOCALE" ]]; then
    bootstrap_locale
    if [[ "$LOCALE" != pt ]]; then
        case "$LOCALE" in
            es) stem=spanish ;; fr) stem=french ;; it) stem=italian ;;
            de) stem=german ;; ja) stem=japanese ;;
        esac
        REFERENCE="$OUT/$stem-reference.o"
        [[ -f "$TREE_ROOT/build/$stem-reference.s" ]] \
            || { echo "release: $TREE_ROOT/build/$stem-reference.s is missing" >&2; exit 1; }
        REF_AS="${REF_AS:-$(command -v arm-none-eabi-as || echo "${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-as")}"
        "$REF_AS" -o "$REFERENCE" "$TREE_ROOT/build/$stem-reference.s"
        RECIPE_EXTRA=(--image-elf "$TREE/build/gamedata_image.elf" --image-map "$TREE/build/gamedata_image.map")
    fi
fi
[[ -f "$REFERENCE" ]] || { echo "release: no original game ELF at $REFERENCE (GBA_ELF=...)" >&2; exit 1; }
[[ -x "$NM" ]] || { echo "release: no nm tool at $NM (NM=...)" >&2; exit 1; }

step "1. library and staging"
make -C "$TREE" -f "$HERE/android.mk" -j"$JOBS" lib romfs-data build/libemerald-stripped.so build/libemeraldboot.so
[[ -z "$LOCALE" || "$LOCALE" == pt ]] || \
    make -C "$TREE" -f "$HERE/android.mk" build/gamedata_image.map

step "2. engine files"
rm -rf "$OUT/assets"
"$PY" "$REPO/tools/port_common/staging.py" engine --romfs "$TREE/romfs" --out "$OUT/assets/engine"
# A release reads its game data from the pack: no embedded-data marker.
rm -f "$OUT/assets/engine/data.embedded"

step "3. recipe"
"$PY" "$REPO/tools/gen_recipe.py" --romfs "$TREE/romfs" --rom "$ROM" --out "$OUT/emerald3ds.recipe" \
    --release "android-$(git -C "$REPO" rev-parse --short HEAD)" --elf "$TREE/emerald3ds.elf" \
    --gba-elf "$REFERENCE" --nm "$NM" --decomp "$TREE_ROOT" --report "$OUT/recipe-report.txt" \
    ${RECIPE_EXTRA[@]+"${RECIPE_EXTRA[@]}"}

step "4. voxel programs ($IMAGE)"
if [[ "${VOXEL_LOCAL:-0}" == 1 ]]; then
    # The pinned interpreter and Pillow, without a container (build/venv).
    "$PY" "$REPO/tools/voxel_precompile.py" --recipe "$OUT/emerald3ds.recipe" --rom "$ROM" \
        --port "$TREE" --outputs "$TREE/romfs" --report "$OUT/precompile.json"
else
    podman run --rm --userns=keep-id -e HOME=/tmp -w "$REPO" \
        -v "$REPO:$REPO:Z" -v "$ROM:/rom.gba:ro" "$IMAGE" sh -c \
        "pip install -q --user --disable-pip-version-check pillow==$PILLOW && \
         python tools/voxel_precompile.py --recipe '$OUT/emerald3ds.recipe' --rom /rom.gba --port '$TREE' \
             --outputs '$TREE/romfs' --report '$OUT/precompile.json'"
fi

step "5. recipe for the app"
"$PY" "$HERE/tools/export_recipe.py" "$OUT/emerald3ds.recipe" "$OUT/assets/builder"
# What Setup.java says this build's ROM is (the app has no other way to know).
case "$LOCALE" in
    "") rom_label="Pokémon Emerald (USA, Europe)" ;;
    es) rom_label="Pokémon Esmeralda (España, BPES)" ;;
    fr) rom_label="Pokémon Émeraude (France, BPEF)" ;;
    it) rom_label="Pokémon Smeraldo (Italia, BPEI)" ;;
    de) rom_label="Pokémon Smaragd (Deutschland, BPED)" ;;
    ja) rom_label="ポケモンエメラルド (日本, BPEJ)" ;;
    pt) rom_label="Pokémon Emerald (traduction pt-BR, BPEE)" ;;
esac
printf '%s\n%s\n' "${LOCALE:-en}" "$rom_label" > "$OUT/assets/builder/rom.txt"

step "6. APK"
case "$LOCALE" in
    "") app_label="Emerald for Android" ;;
    es) app_label="Esmeralda para Android" ;;
    fr) app_label="Émeraude pour Android" ;;
    it) app_label="Smeraldo per Android" ;;
    de) app_label="Smaragd für Android" ;;
    ja) app_label="ポケモンエメラルド for Android" ;;
    pt) app_label="Emerald para Android" ;;
esac
APK="$OUT/emerald-android${LOCALE:+-$LOCALE}.apk"
APK_OUT="$APK" ASSETS="$OUT/assets" RELEASE=1 \
PACKAGE="com.cer0.emerald${LOCALE:+.$LOCALE}" APP_LABEL="$app_label" \
    bash "$HERE/build_apk.sh" "$TREE/build/libemerald-stripped.so" "$TREE/build/libemeraldboot.so"

step "7. audit"
"$PY" "$REPO/tools/release_audit.py" --zip "$APK" --rom "$ROM" --strict \
    || { echo "release: the audit failed: do not publish this APK" >&2; exit 1; }
if [[ -z "${KEYSTORE:-}" ]]; then
    echo "release: $APK is signed with the DEBUG key: for testing only." >&2
    echo "release: publish only an APK built with KEYSTORE=... KEYSTORE_PASS=... (build_apk.sh)" >&2
    exit 2
fi
step ""
echo "release: $APK ($(elapsed $SECONDS) in all)"
