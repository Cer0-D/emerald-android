#!/usr/bin/env bash
# Runs an arm64 link made with --noinhibit-exec and fails it on any diagnostic
# but the expected one: lld cannot leave a 32-bit address (R_AARCH64_ABS32)
# for the dynamic linker, and the game data's are filled in by the bundle
# loader instead (android.mk). Anything else - an undefined symbol, a
# relocation of another kind - is an error again.
#   lld_check.sh <link command...>
set -uo pipefail
log="$(mktemp)"
"$@" 2>"$log"
status=$?
unexpected="$(grep -v -E '^ld\.lld: warning: relocation R_AARCH64_ABS32 cannot be used against (local )?symbol|^>>> ' "$log" \
    | grep -v -E '^ld\.lld: warning: too many (errors|warnings)' || true)"
expected="$(grep -c 'R_AARCH64_ABS32 cannot be used' "$log" || true)"
rm -f "$log"
if [[ $status -ne 0 || -n "$unexpected" ]]; then
    printf '%s\n' "$unexpected" >&2
    echo "lld_check: the link failed or reported something besides 32-bit data addresses" >&2
    exit 1
fi
echo "lld_check: $expected 32-bit data addresses left to the bundle loader"
