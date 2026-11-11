#!/usr/bin/env bash
# Asks the running app for a capture of both screens and converts it to PNGs.
#   android_port/tools/capture.sh [out-dir] [name]
# Writes <out-dir>/<name>_top.png (432x240) and <name>_bottom.png (320x280).
# The app overwrites its two capture files each time (nothing accumulates).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../build/shots}"
NAME="${2:-shot}"
PY="${PYTHON:-$HERE/../../build/venv/bin/python}"
D=/sdcard/Android/data/com.cer0.emerald.dev/files
mkdir -p "$OUT"
adb shell am start -n com.cer0.emerald.dev/com.cer0.emerald.MainActivity --ez capture true >/dev/null
sleep 0.5
adb exec-out cat "$D/capture_top.rgba" > "$OUT/$NAME.top.rgba"
adb exec-out cat "$D/capture_bottom.rgb565cm" > "$OUT/$NAME.bottom.raw"
"$PY" - "$OUT" "$NAME" <<'EOF'
import sys
from PIL import Image
out, name = sys.argv[1], sys.argv[2]
top = Image.frombytes("RGBA", (432, 240), open(f"{out}/{name}.top.rgba", "rb").read())
top.convert("RGB").save(f"{out}/{name}_top.png")
raw = open(f"{out}/{name}.bottom.raw", "rb").read()
img = Image.new("RGB", (320, 280))
px = img.load()
for x in range(320):
    for y in range(280):
        i = (x * 280 + 279 - y) * 2
        c = raw[i] | (raw[i + 1] << 8)
        px[x, y] = (((c >> 11) & 31) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3)
img.save(f"{out}/{name}_bottom.png")
print(f"{out}/{name}_top.png {out}/{name}_bottom.png")
EOF
