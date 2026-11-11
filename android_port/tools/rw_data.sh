#!/usr/bin/env bash
# A linked asm data object for a shared library: its read-only sections hold
# absolute pointers (.int symbol), which a PIC link may not leave in
# read-only memory (no text relocations on Android). The copy names them
# .data.rel.ro.*, writable until the dynamic linker has relocated them.
#   rw_data.sh OBJCOPY READELF in.o out.o
set -euo pipefail
objcopy="$1" readelf="$2" in="$3" out="$4"
args=()
while read -r name; do
    args+=(--rename-section "$name=.data.rel.ro${name#.rodata},alloc,load,contents,data")
done < <("$readelf" -SW "$in" | sed -nE 's/^\s*\[\s*[0-9]+\]\s+(\.rodata[^ ]*)\s.*/\1/p' | sort -u)
mkdir -p "$(dirname "$out")"
"$objcopy" "${args[@]}" "$in" "$out"
