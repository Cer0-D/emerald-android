#!/usr/bin/env bash
# The release APK: android_port/tools/build.sh --release, which builds every
# supported language into the one APK (com.cer0.emerald) from the clean ROMs
# in --roms DIR (default $EMERALD_ROMS or ~/Documents/gba_roms), and audits it
# against each. Signed with the release key when KEYSTORE/KEYSTORE_PASS are
# set (build_apk.sh), else with the debug key - for testing only.
#
#   android_port/tools/release.sh [--roms DIR]
#
# There is no per-language APK any more: the app picks the language from the
# player's ROM (docs/ANDROID-LANGUAGES.md).
set -euo pipefail
exec "$(dirname "$0")/build.sh" --release "$@"
