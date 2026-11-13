#!/usr/bin/env bash
# Packages the Android APK without Gradle: javac + d8 for the Java shell,
# aapt2 for the manifest, the NDK's clang for the native library (built
# elsewhere and passed in, or the step-1 smoke test by default), then
# zipalign + apksigner: the debug keystore, or the release key (KEYSTORE).
#
#   ABI=arm64-v8a android_port/build_apk.sh [path/to/libemerald.so path/to/libemeraldboot.so]
# ABI: arm64-v8a (the default) or armeabi-v7a, the libraries' architecture.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
BT="$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)"
PLATFORM="$SDK/platforms/$(ls "$SDK/platforms" | sort -V | tail -1)/android.jar"
NDK="$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)"
ABI="${ABI:-arm64-v8a}"
case "$ABI" in
    arm64-v8a) CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang" ;;
    armeabi-v7a) CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi33-clang" ;;
    *) echo "build_apk.sh: unknown ABI $ABI" >&2; exit 1 ;;
esac
# The version: Emerald 3DS's (3ds_port/Makefile), plus this port's subversion
# (subversion.txt) when it is not 0 - 0.5.0, 0.5.0.1... The version code orders
# updates: major * 1000000 + minor * 10000 + patch * 100 + subversion.
UPSTREAM_VERSION="$(sed -n 's/^APP_VERSION := //p' "$HERE/../3ds_port/Makefile")"
SUBVERSION="$(tr -d '[:space:]' < "$HERE/subversion.txt")"
IFS=. read -r V_MAJOR V_MINOR V_PATCH <<< "$UPSTREAM_VERSION"
VERSION_NAME="$UPSTREAM_VERSION"
[[ "$SUBVERSION" == 0 ]] || VERSION_NAME="$UPSTREAM_VERSION.$SUBVERSION"
VERSION_CODE=$(( V_MAJOR * 1000000 + V_MINOR * 10000 + V_PATCH * 100 + SUBVERSION ))
OUT="$HERE/build/apk"
APK="${APK_OUT:-$HERE/build/emerald-thor.apk}"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/lib/$ABI"

LIB="${1:-}"
BOOT="${2:-}"
if [[ -z "$LIB" ]]; then
    LIB="$OUT/libemerald.so"
    "$CC" -shared -fPIC -O2 -Wall -Werror -o "$LIB" "$HERE/app/jni/hello.c" \
        -landroid -lEGL -lGLESv3 -llog
fi
if [[ -z "$BOOT" ]]; then
    # Java loads libemeraldboot.so, which loads libemerald.so (boot/emerald_boot.c).
    BOOT="$OUT/libemeraldboot.so"
    "$CC" -shared -fPIC -O2 -Wall -Werror -o "$BOOT" "$HERE/boot/emerald_boot.c" -ldl -llog
fi
cp "$LIB" "$OUT/lib/$ABI/libemerald.so"
cp "$BOOT" "$OUT/lib/$ABI/libemeraldboot.so"

javac -source 11 -target 11 -Xlint:-options -classpath "$PLATFORM" -d "$OUT/classes" \
    $(find "$HERE/app/java" -name '*.java')
"$BT/d8" --min-api 33 --lib "$PLATFORM" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')

# The assets: a release's engine files and recipe (ASSETS), and in every APK
# the list of the follower files the app downloads (FollowerFetcher.java):
# builder/emerald3ds_builder/followers_sources.py as JSON.
mkdir -p "$OUT/assets/followers"
[[ -z "${ASSETS:-}" ]] || cp -r "$ASSETS/." "$OUT/assets/"
"${PYTHON:-python3}" -I -c 'import json, runpy, sys
json.dump(runpy.run_path(sys.argv[1])["SOURCES"], open(sys.argv[2], "w"), separators=(",", ":"))' \
    "$HERE/../builder/emerald3ds_builder/followers_sources.py" "$OUT/assets/followers/sources.json"

# A development APK is debuggable and installs as com.cer0.emerald.dev, beside
# the release app (debug-signed, it could not update it); a release one
# (RELEASE=1, tools/release.sh) is com.cer0.emerald, not debuggable.
# The resources: the launcher icon (app/res, made by tools/make_icon.py).
# A development APK is also named apart on the launcher (APP_LABEL, default
# "Emerald Dev"), so the two installs can be told apart.
MANIFEST="$HERE/app/AndroidManifest.xml"
if [[ -z "${RELEASE:-}" ]]; then
    MANIFEST="$OUT/AndroidManifest.xml"
    sed "s|android:label=\"Emerald for Android\"|android:label=\"${APP_LABEL:-Emerald Dev}\"|" \
        "$HERE/app/AndroidManifest.xml" > "$MANIFEST"
    grep -q "android:label=\"${APP_LABEL:-Emerald Dev}\"" "$MANIFEST" \
        || { echo "build_apk.sh: could not rename the development app" >&2; exit 1; }
fi
"$BT/aapt2" compile --dir "$HERE/app/res" -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" --manifest "$MANIFEST" "$OUT/res.zip" \
    --version-name "$VERSION_NAME" --version-code "$VERSION_CODE" --replace-version \
    -A "$OUT/assets" $([[ -n "${RELEASE:-}" ]] || echo --debug-mode --rename-manifest-package com.cer0.emerald.dev)
( cd "$OUT/dex" && zip -q "$OUT/base.apk" classes.dex )
( cd "$OUT" && zip -q -0 "$OUT/base.apk" "lib/$ABI/libemerald.so" "lib/$ABI/libemeraldboot.so" )

"$BT/zipalign" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
# The release key when KEYSTORE is set (KEYSTORE_PASS: a file holding its
# password; KEY_ALIAS, default "emerald"), the SDK's debug key otherwise.
# Every published update must be signed with the same release key.
if [[ -n "${KEYSTORE:-}" ]]; then
    "$BT/apksigner" sign --ks "$KEYSTORE" --ks-key-alias "${KEY_ALIAS:-emerald}" \
        --ks-pass "file:${KEYSTORE_PASS:?KEYSTORE_PASS: the file with the keystore password}" \
        --out "$APK" "$OUT/aligned.apk"
else
    "$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
        --key-pass pass:android --out "$APK" "$OUT/aligned.apk"
fi
echo "built $APK ($VERSION_NAME, version code $VERSION_CODE)"
