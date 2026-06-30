#!/bin/bash
# flash.sh — write the pre-built Claudeq firmware to the device. No ESP-IDF / no compiler.
#
#   firmware/dist/flash.sh                 # auto-detect the port, flash the bundled image
#   firmware/dist/flash.sh /dev/cu.usbmodemXXXX   # or name the port explicitly
#   CLAUDEQ_FIRMWARE=/path/to.bin firmware/dist/flash.sh   # override the image
#
# Uses esptool (ships with ESP-IDF; otherwise `pip install esptool`) or espflash
# (`brew install espflash`). If neither is installed it points you at the browser flasher,
# which needs nothing at all.
set -euo pipefail

DIST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${CLAUDEQ_FIRMWARE:-$DIST_DIR/claudeq-firmware.bin}"
CHIP="esp32s3"

[ -f "$BIN" ] || { echo "error: firmware image not found: $BIN" >&2
  echo "       run firmware/dist/build-dist.sh, or set CLAUDEQ_FIRMWARE." >&2; exit 1; }

# ---- port: 1st arg > $CLAUDEQ_PORT > autodetect ----
PORT="${1:-${CLAUDEQ_PORT:-}}"
if [ -z "$PORT" ]; then
  case "$(uname -s)" in
    Darwin) PORT="$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1 || true)" ;;
    *)      PORT="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)" ;;
  esac
fi

web_flasher_hint() {
  echo ""
  echo "No command-line flasher found. Two options:"
  echo "  • Browser (nothing to install): open the web flasher in Chrome/Edge and click Install:"
  echo "        https://positronico.github.io/claudeq/"
  echo "    (from a source checkout, serve it first: 'cd flasher && python3 -m http.server',"
  echo "     then open http://localhost:8000 — opening the .html file directly won't work.)"
  echo "  • CLI: 'pip install esptool'  or  'brew install espflash', then re-run this."
}

if [ -z "$PORT" ]; then
  echo "error: no serial device found — plug the deck into USB." >&2
  echo "       then: firmware/dist/flash.sh /dev/cu.usbmodemXXXX" >&2
  exit 1
fi

echo "Flashing $(basename "$BIN") → $PORT  ($CHIP)"
echo "Hold steady; the deck reboots when it's done (~20s)."
echo ""

if command -v esptool >/dev/null 2>&1; then
  exec esptool --chip "$CHIP" --port "$PORT" --baud 921600 write_flash 0x0 "$BIN"
elif python3 -c "import esptool" >/dev/null 2>&1; then
  exec python3 -m esptool --chip "$CHIP" --port "$PORT" --baud 921600 write_flash 0x0 "$BIN"
elif command -v espflash >/dev/null 2>&1; then
  exec espflash write-bin --port "$PORT" 0x0 "$BIN"
else
  web_flasher_hint
  exit 1
fi
