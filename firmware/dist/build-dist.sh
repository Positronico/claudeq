#!/bin/bash
# build-dist.sh — turn a fresh `idf.py build` into the distributable artifacts.
#
# Produces, from firmware/companion/build/:
#   firmware/dist/claudeq-firmware.bin   single merged image, flash at 0x0 (web flasher + `claudeq flash`)
#   firmware/dist/bootloader.bin         individual parts (reference / advanced flashing)
#   firmware/dist/partition-table.bin
#   firmware/dist/claudeq-app.bin
#   firmware/dist/manifest.json          ESP Web Tools manifest
# and copies the merged image + manifest into flasher/ so the web flasher is self-contained.
#
# Requires: esptool (ships with ESP-IDF; `pip install esptool` otherwise). No full toolchain needed.
#
#   cd firmware/companion && idf.py build      # rebuild the firmware first
#   ../dist/build-dist.sh                       # then regenerate the dist
set -euo pipefail

DIST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$DIST_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/firmware/companion/build"
FLASHER_DIR="$REPO_ROOT/flasher"
ARGS_JSON="$BUILD_DIR/flasher_args.json"

[ -f "$ARGS_JSON" ] || { echo "error: $ARGS_JSON not found — run 'idf.py build' first" >&2; exit 1; }

ESPTOOL=(esptool)
command -v esptool >/dev/null 2>&1 || ESPTOOL=(python3 -m esptool)

VERSION="$(cat "$DIST_DIR/VERSION" 2>/dev/null || echo 0.1.0)"

# Pull chip, flash settings, and the offset→file map straight out of the build's own recipe,
# so this keeps working after the app/project is renamed.
read -r CHIP MODE FREQ SIZE < <(python3 - "$ARGS_JSON" <<'PY'
import json,sys
a=json.load(open(sys.argv[1]))
s=a["flash_settings"]; e=a["extra_esptool_args"]
print(e["chip"], s["flash_mode"], s["flash_freq"], s["flash_size"])
PY
)

# offset file  (one pair per line, in flash order) — while-read loop for bash 3.2 (macOS) which lacks mapfile
MERGE_ARGS=()
while read -r off rel; do
  [ -n "$off" ] || continue
  MERGE_ARGS+=("$off" "$BUILD_DIR/$rel")
done < <(python3 - "$ARGS_JSON" <<'PY'
import json,sys
a=json.load(open(sys.argv[1]))
for off,f in sorted(a["flash_files"].items(), key=lambda kv:int(kv[0],16)):
    print(off, f)
PY
)

echo "merge: chip=$CHIP mode=$MODE freq=$FREQ size=$SIZE"
# Underscore spellings work on both esptool v4 (the one ESP-IDF bundles — used by the documented
# `idf.py build` regen flow) and v5 (deprecation warning, same bytes). The dashed forms are v5-only.
"${ESPTOOL[@]}" --chip "$CHIP" merge_bin \
  -o "$DIST_DIR/claudeq-firmware.bin" \
  --flash_mode "$MODE" --flash_freq "$FREQ" --flash_size "$SIZE" \
  "${MERGE_ARGS[@]}"

# Copy individual parts with friendly, stable names.
cp "$BUILD_DIR/bootloader/bootloader.bin"             "$DIST_DIR/bootloader.bin"
cp "$BUILD_DIR/partition_table/partition-table.bin"   "$DIST_DIR/partition-table.bin"
# the app bin is whatever sits at 0x10000
APP_REL="$(python3 - "$ARGS_JSON" <<'PY'
import json,sys
a=json.load(open(sys.argv[1]))
print(a["flash_files"]["0x10000"])
PY
)"
cp "$BUILD_DIR/$APP_REL" "$DIST_DIR/claudeq-app.bin"

# ESP Web Tools manifest (single merged part at offset 0).
CHIP_FAMILY="$(echo "$CHIP" | tr 'a-z' 'A-Z' | sed 's/ESP32S3/ESP32-S3/')"
cat > "$DIST_DIR/manifest.json" <<JSON
{
  "name": "Claudeq",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "$CHIP_FAMILY",
      "parts": [
        { "path": "claudeq-firmware.bin", "offset": 0 }
      ]
    }
  ]
}
JSON

# Make the web flasher self-contained.
mkdir -p "$FLASHER_DIR"
cp "$DIST_DIR/claudeq-firmware.bin" "$FLASHER_DIR/claudeq-firmware.bin"
cp "$DIST_DIR/manifest.json"        "$FLASHER_DIR/manifest.json"

echo "ok: dist written to $DIST_DIR (v$VERSION); flasher/ updated"
ls -la "$DIST_DIR"/*.bin "$DIST_DIR"/manifest.json
