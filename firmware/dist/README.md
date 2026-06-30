# Pre-built Claudeq firmware

These are ready-to-flash binaries for the **Waveshare ESP32-S3-Touch-LCD-3.49**. Flashing them
needs **no compiler and no ESP-IDF** — only a small flasher (or just a browser).

| File | What |
|---|---|
| `claudeq-firmware.bin` | **Single merged image** — flash at `0x0`. Used by the browser flasher and `claudeq flash`. |
| `bootloader.bin` / `partition-table.bin` / `claudeq-app.bin` | The individual parts (`0x0` / `0x8000` / `0x10000`), for reference or advanced flashing. |
| `manifest.json` | [ESP Web Tools](https://esphome.github.io/esp-web-tools/) manifest for the browser flasher. |

## Flash it

**Browser (nothing to install):** use the hosted flasher at **https://positronico.github.io/claudeq/** in
Chrome or Edge and click **Install**. (From a source checkout, serve it first — `cd flasher &&
python3 -m http.server`, then open http://localhost:8000 — opening the `.html` file directly won't
work, since the flasher fetches its manifest over http.)

**Command line:**
```bash
firmware/dist/flash.sh                 # auto-detects the port
firmware/dist/flash.sh /dev/cu.usbmodemXXXX   # or name it
# or, once the bridge is installed:
claudeq flash
```
`flash.sh` uses `esptool` (ships with ESP-IDF, else `pip install esptool`) or `espflash`
(`brew install espflash`). With neither, it points you at the browser flasher.

After flashing, the deck boots into **WiFi setup** — join the `Claudeq-setup` hotspot from your
phone and enter your network. No rebuild to change WiFi later.

## Regenerate (after changing the firmware)

```bash
cd firmware/companion && idf.py build     # needs ESP-IDF — only to *build*, not to flash
../dist/build-dist.sh                      # merges + copies into dist/ and flasher/
```
`build-dist.sh` reads `build/flasher_args.json`, so it tracks the real offsets/flash settings and
survives the app being renamed. Bump `firmware/dist/VERSION` for a new release.
