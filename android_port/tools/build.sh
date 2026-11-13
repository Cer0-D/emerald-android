#!/usr/bin/env bash
# The one Emerald for Android APK, every supported cartridge language in it:
#
#   android_port/tools/build.sh [--release] [--roms DIR] [--no-install]
#
# LANGS=en,fr,it,de,es narrows the languages (default: every supported one whose
# clean ROM is in DIR - default $EMERALD_ROMS or ~/Documents/gba_roms - found
# by SHA-1). A development build (the default) skips a language without its
# ROM, is debuggable, installs as com.cer0.emerald.dev ("Emerald Dev") and is
# installed with adb (ADB_SERIAL picks the device); its ROMs are pushed with
# tools/dev_rom.sh. --release needs every language, builds com.cer0.emerald
# and audits the APK against each ROM. The app picks the language from the
# player's ROM (Languages.java, Setup.java).
#
# Per language, the steps tools/build_release.py takes for the 3DS:
#  1. its tree: English is the development tree, build/upstream; another
#     language is bootstrapped and localized in build/release-<code>
#     (tools/bootstrap.py, reset when the patches or the pin change); both
#     get this checkout's 3ds_port/, builder/ and tools/;
#  2. the library and the staged romfs/ (android.mk lib romfs-data);
#  3. the engine files of that staging (staging.py engine)
#     -> assets/lang/<code>/engine;
#  4. the recipe (tools/gen_recipe.py) from the staging and the ROM, with the
#     original game's symbols (build/gba-elf for English, the localization's
#     reference for another language);
#  5. the voxel programs (tools/voxel_precompile.py) in the pinned Python
#     (upstream.lock [python]) through podman, or build/venv with VOXEL_LOCAL=1;
#  6. the recipe unpacked for the app (export_recipe.py) and rom.txt
#     -> assets/lang/<code>/builder.
# Steps 3-6 are skipped for a language whose library, staging, ROM and tools
# are what they were when they last ran (build/android-<mode>/stamps).
# Then one APK (build_apk.sh) with every language's library and assets.
#
# The ROMs are read, never copied: into the container they go read-only.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
MODE=dev
INSTALL=1
ROMS="${EMERALD_ROMS:-$HOME/Documents/gba_roms}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) MODE=release; INSTALL=0; shift ;;
        --roms) ROMS="${2:?--roms needs a directory}"; shift 2 ;;
        --no-install) INSTALL=0; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "build: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# The supported cartridges: code, clean ROM SHA-1, the name the app shows.
SUPPORTED=(en fr it de es)
declare -A SHA1=(
    [en]=f3ae088181bf583e55daf962a92bb46f4f1d07b7
    [fr]=ca666651374d89ca439007bed54d839eb7bd14d0
    [it]=1692db322400c3141c5de2db38469913ceb1f4d4
    [de]=61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3
    [es]=fe1558a3dcb0360ab558969e09b690888b846dd9
)
declare -A LABEL=(
    [en]="Pokémon Emerald (USA, Europe)"
    [fr]="Pokémon Émeraude (France)"
    [it]="Pokémon Smeraldo (Italia)"
    [de]="Pokémon Smaragd (Deutschland)"
    [es]="Pokémon Esmeralda (España)"
)
declare -A STEM=([fr]=french [it]=italian [de]=german [es]=spanish)
declare -A FLAG=([fr]=--french-rom [it]=--italian-rom [de]=--german-rom [es]=--spanish-rom)
declare -A GAME=([fr]=BPEF [it]=BPEI [de]=BPED [es]=BPES)

OUT="$REPO/build/android-$MODE"
GBA_ELF="${GBA_ELF:-$REPO/build/gba-elf/pokeemerald/pokeemerald.elf}"
PY="${PYTHON:-$REPO/build/venv/bin/python}"
NDK_BIN="${NDK_BIN:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/*/toolchains/llvm/prebuilt/*/bin 2>/dev/null | sort -V | tail -1)}"
NM="${NM:-$NDK_BIN/llvm-nm}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )}"
IMAGE="docker.io/library/$(sed -n 's/^image = "\(python:[^"]*\)"/\1/p' "$REPO/upstream.lock")"
PILLOW="$(sed -n 's/^pillow = "\([^"]*\)"/\1/p' "$REPO/upstream.lock")"
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
[[ -x "$NM" ]] || { echo "build: no nm tool at $NM (NM=...)" >&2; exit 1; }

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

# The ROMs in $ROMS, by SHA-1.
declare -A ROM=()
if [[ -d "$ROMS" ]]; then
    while IFS= read -r -d '' f; do
        sum=$(sha1sum "$f" | cut -d' ' -f1)
        for code in "${SUPPORTED[@]}"; do
            [[ "$sum" == "${SHA1[$code]}" ]] && ROM[$code]="$(realpath "$f")"
        done
    done < <(find "$ROMS" -maxdepth 1 -type f -iname '*.gba' -print0)
fi
IFS=, read -r -a WANT <<< "${LANGS:-$(IFS=,; echo "${SUPPORTED[*]}")}"
CODES=()
for code in "${WANT[@]}"; do
    [[ -n "${SHA1[$code]:-}" ]] || { echo "build: unsupported language: $code (${SUPPORTED[*]})" >&2; exit 2; }
    if [[ -z "${ROM[$code]:-}" ]]; then
        [[ "$MODE" == dev ]] || { echo "build: no clean ${LABEL[$code]} ROM in $ROMS" >&2; exit 1; }
        echo "build: no ${LABEL[$code]} ROM in $ROMS: $code left out"
        continue
    fi
    CODES+=("$code")
done
[[ ${#CODES[@]} -gt 0 ]] || { echo "build: no language to build (ROMs in $ROMS?)" >&2; exit 1; }
echo "build: $MODE, languages ${CODES[*]}"
mkdir -p "$OUT/stamps"
rm -rf "$OUT/assets.new"

# This checkout's 3ds_port/, builder/ and tools/ into a tree (bootstrap.py's
# last step): every build builds what is checked out, not a stale copy.
sync_tree() {
    "$PY" - "$REPO" "$1" <<'PY'
import pathlib, shutil, sys
root, tree = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for name in ("3ds_port", "builder", "tools"):
    if (root / name).is_dir():
        shutil.copytree(root / name, tree / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "build", "dist"))
PY
}

# A language's tree, bootstrapped and localized unless it is current.
bootstrap_language() {
    local code="$1" root="$2" rom="$3" pin head marker digest
    pin=$("$PY" -c 'import sys,tomllib;print(tomllib.load(open(sys.argv[1],"rb"))["pokeemerald"]["commit"])' \
        "$REPO/upstream.lock")
    head=$(git -C "$root" rev-parse HEAD 2>/dev/null || true)
    marker=$(cat "$root/.emerald3ds-patches" 2>/dev/null || true)
    digest=$("$PY" - "$REPO" <<'PY'
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(sys.argv[1]) / "tools"))
import bootstrap
print(bootstrap.patch_digest(sorted((pathlib.Path(sys.argv[1]) / "patches" / "pokeemerald").glob("*.patch"))))
PY
)
    if ! [[ "$head" == "$pin" && "$marker" == "$digest" && -f "$root/.emerald3ds-locale" ]] \
        || ! grep -qs "${GAME[$code]}" "$root/.emerald3ds-locale"; then
        echo "build: staging the $code tree in $root"
        "$PY" "$REPO/tools/bootstrap.py" --dir "$root" "${FLAG[$code]}" "$rom" --python "$PY" -j"$JOBS"
    fi
}

# What a language's steps 3-6 were made from.
stamp_of() {
    local code="$1" tree="$2"
    {
        echo "$MODE ${ROM[$code]##*/} ${SHA1[$code]}"
        sha256sum "$tree/build/libemerald-stripped.so" "$tree/emerald3ds.elf" \
            "$REPO/tools/gen_recipe.py" "$REPO/tools/voxel_precompile.py" "$HERE/tools/export_recipe.py" \
            "$REPO/tools/port_common/staging.py" | cut -d' ' -f1
        ( cd "$tree/romfs" && find . -type f -print0 | sort -z | xargs -0 sha256sum )
    } | sha256sum | cut -d' ' -f1
}

build_language() {
    local code="$1" rom="${ROM[$1]}" root tree work dest reference extra=()
    if [[ "$code" == en ]]; then root="$REPO/build/upstream"; else root="$REPO/build/release-$code"; fi
    tree="$root/3ds_port"
    work="$OUT/work/$code"
    dest="$OUT/assets.new/lang/$code"
    mkdir -p "$work"

    step "$code: tree"
    [[ "$code" == en ]] || bootstrap_language "$code" "$root" "$rom"
    sync_tree "$root"
    reference="$GBA_ELF"
    if [[ "$code" != en ]]; then
        reference="$work/${STEM[$code]}-reference.o"
        [[ -f "$root/build/${STEM[$code]}-reference.s" ]] \
            || { echo "build: $root/build/${STEM[$code]}-reference.s is missing" >&2; exit 1; }
        "${REF_AS:-$(command -v arm-none-eabi-as || echo "${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-as")}" \
            -o "$reference" "$root/build/${STEM[$code]}-reference.s"
        extra=(--image-elf "$tree/build/gamedata_image.elf" --image-map "$tree/build/gamedata_image.map")
    fi
    [[ -f "$reference" ]] || { echo "build: no original game ELF at $reference (GBA_ELF=...)" >&2; exit 1; }

    step "$code: library and staging"
    make -C "$tree" -f "$HERE/android.mk" -j"$JOBS" lib romfs-data build/libemerald-stripped.so build/libemeraldboot.so
    [[ "$code" == en ]] || make -C "$tree" -f "$HERE/android.mk" build/gamedata_image.map

    local stamp
    stamp=$(stamp_of "$code" "$tree")
    if [[ "$stamp" == "$(cat "$OUT/stamps/$code" 2>/dev/null)" && -d "$OUT/assets/lang/$code/builder" ]]; then
        step "$code: engine files, recipe and voxel programs unchanged"
        mkdir -p "$(dirname "$dest")"
        cp -r "$OUT/assets/lang/$code" "$dest"
        return
    fi

    step "$code: engine files"
    "$PY" "$REPO/tools/port_common/staging.py" engine --romfs "$tree/romfs" --out "$dest/engine"
    # The app reads its game data from the pack: no embedded-data marker.
    rm -f "$dest/engine/data.embedded"

    step "$code: recipe"
    "$PY" "$REPO/tools/gen_recipe.py" --romfs "$tree/romfs" --rom "$rom" --out "$work/emerald3ds.recipe" \
        --release "android-$(git -C "$REPO" rev-parse --short HEAD)" --elf "$tree/emerald3ds.elf" \
        --gba-elf "$reference" --nm "$NM" --decomp "$root" --report "$work/recipe-report.txt" \
        ${extra[@]+"${extra[@]}"}

    step "$code: voxel programs"
    if [[ "${VOXEL_LOCAL:-0}" == 1 ]]; then
        "$PY" "$REPO/tools/voxel_precompile.py" --recipe "$work/emerald3ds.recipe" --rom "$rom" \
            --port "$tree" --outputs "$tree/romfs" --report "$work/precompile.json"
    else
        podman run --rm --userns=keep-id -e HOME=/tmp -w "$REPO" \
            -v "$REPO:$REPO:Z" -v "$rom:/rom.gba:ro" "$IMAGE" sh -c \
            "pip install -q --user --disable-pip-version-check pillow==$PILLOW && \
             python tools/voxel_precompile.py --recipe '$work/emerald3ds.recipe' --rom /rom.gba --port '$tree' \
                 --outputs '$tree/romfs' --report '$work/precompile.json'"
    fi

    step "$code: recipe for the app"
    "$PY" "$HERE/tools/export_recipe.py" "$work/emerald3ds.recipe" "$dest/builder"
    # What Languages.java knows of this language's cartridge.
    printf '%s\n%s\n%s\n' "$code" "${LABEL[$code]}" "${SHA1[$code]}" > "$dest/builder/rom.txt"
    stamp_of "$code" "$tree" > "$OUT/stamps/$code"
}

for code in "${CODES[@]}"; do build_language "$code"; done
rm -rf "$OUT/assets"
mv "$OUT/assets.new" "$OUT/assets"

step "APK"
LIBS=()
for code in "${CODES[@]}"; do
    if [[ "$code" == en ]]; then tree="$REPO/build/upstream/3ds_port"; else tree="$REPO/build/release-$code/3ds_port"; fi
    LIBS+=("$code=$tree/build/libemerald-stripped.so")
done
BOOT="$REPO/build/upstream/3ds_port/build/libemeraldboot.so"
[[ -f "$BOOT" ]] || BOOT="$(dirname "${LIBS[0]#*=}")/libemeraldboot.so"
if [[ "$MODE" == release ]]; then
    APK="$OUT/emerald-android.apk"
    APK_OUT="$APK" ASSETS="$OUT/assets" RELEASE=1 bash "$HERE/build_apk.sh" "$BOOT" "${LIBS[@]}"
else
    APK="$OUT/emerald-android-dev.apk"
    APK_OUT="$APK" ASSETS="$OUT/assets" bash "$HERE/build_apk.sh" "$BOOT" "${LIBS[@]}"
fi

if [[ "$MODE" == release ]]; then
    step "audit"
    for code in "${CODES[@]}"; do
        "$PY" "$REPO/tools/release_audit.py" --zip "$APK" --rom "${ROM[$code]}" --strict \
            || { echo "build: the audit failed ($code): do not publish this APK" >&2; exit 1; }
    done
fi
if [[ "$INSTALL" == 1 ]]; then
    step "install"
    "${ADB[@]}" install -r "$APK"
fi
step ""
echo "build: $APK (${CODES[*]}, $(elapsed $SECONDS) in all)"
if [[ "$MODE" == release && -z "${KEYSTORE:-}" ]]; then
    echo "build: $APK is signed with the DEBUG key: for testing only." >&2
    echo "build: publish only an APK built with KEYSTORE=... KEYSTORE_PASS=... (build_apk.sh)" >&2
    exit 2
fi
