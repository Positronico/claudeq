# Claudeq

A Waveshare **ESP32-S3-Touch-LCD-3.49** turned into a physical control surface for **Claude Code**.

When Claude asks you something, the options pop up on the little touchscreen and you **tap to answer**.
The screen shows what Claude is doing, chirps when it needs you, and has a macro deck and tap-to-talk
voice — with an on-screen **Send/Cancel** so nothing reaches Claude until you approve it. Run it across
**as many Claude sessions as you like** — each becomes a tappable chip.

<p align="center"><i>Tap to answer · glance at status · fire macros · talk to Claude — for every session, from one little screen.</i></p>

---

# Install

**No compiler, no ESP-IDF, no Python.** Just bought the same device? You're three steps from running.
(Prefer to build it yourself? See **Build from source** at the bottom.)

### 1. Flash the firmware — *from your browser*
Open **https://positronico.github.io/claudeq/** in **Chrome or Edge** on a desktop, plug the device into
USB-C, and click **Install**. ~30 seconds, nothing to download.

*(No browser handy? After step 2 you can also run `claudeq flash`.)*

### 2. Install the bridge
```bash
brew install Positronico/tap/claudeq      # pulls node + tmux + ffmpeg, puts `claudeq` on your PATH
```

### 3. Connect it to WiFi — *on the device, no rebuild*
On first boot (or any time it can't reach your network) the deck shows a **WiFi setup** screen and
creates a temporary hotspot:
1. On your phone, **join the WiFi `Claudeq-setup`**.
2. A setup page opens automatically (or browse to **http://192.168.4.1**).
3. Enter your **2.4 GHz WiFi** and **Save**. *(Leave Bridge address blank — the deck finds every
   bridge on your network automatically via mDNS.)*

The deck restarts and connects. Settings live on the device. To change networks later, open the
**Settings** tab and **hold** "WiFi portal" — or just power it up somewhere new.

### Voice (optional) — local transcription, no API key
Download the model to a fixed location and point the bridge at it (works for both the `brew` and
from-source installs):
```bash
brew install whisper-cpp
mkdir -p ~/.claudeq/whisper
curl -L -o ~/.claudeq/whisper/ggml-base.en.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
echo 'export CCDECK_WHISPER_MODEL="$HOME/.claudeq/whisper/ggml-base.en.bin"' >> ~/.zshrc
source ~/.zshrc
```
`whisper-cli` is found on your `PATH`; set `CCDECK_WHISPER_BIN` only if it lives somewhere unusual.

---

# Use

In any project, start Claude Code with **`claudeq`** instead of `claude`:
```bash
cd ~/my-project
claudeq
```
That's the whole workflow. `claudeq` starts the bridge if needed, runs Claude in a tmux session named
after the project (so it shows up as a chip), and enables the deck. Plain `claude` is unaffected.

**On the screen:**

| Do this | What happens |
|---|---|
| Claude asks a question | The options appear — **tap one to answer**. Multi-question prompts step through each. |
| **Tap a session chip** (top) | Switch which session the deck controls. Chips glow when a session needs you. |
| **Macros** tab | Tap a saved prompt/slash-command → sent to the focused session. |
| **Voice** (mic) | **Tap** the mic and speak, then **Stop** → review the transcript and tap **Send** (or Cancel). |
| **BOOT button** (side) | **Short press** cycles screen brightness; **hold** (~1.5s) toggles standby (screen off). Touch or a new event wakes it. |
| **Top bar** (icons only) | Four evenly-spaced indicators: the Claude mascot + **count of connected bridges**, the **Tailscale** logo (green when the tailnet is up, shown only if configured), **WiFi**, and the **battery** gauge (green → amber → red as it drains). |
| **Bottom strip** (above the buttons) | The focused session's live **status** (dot + text) — moved here so it has room to breathe. |
| **Settings** tab | Toggle **WiFi** / **Tailscale** / **Auto sleep** / **Sounds**, or hold **WiFi portal** to reconfigure. |

**Standby / low power:** the deck's biggest draw is the backlight, so standby just turns the **screen** off
(backlight + panel) while WiFi and the WebSocket stay up — so it's still reachable and lights back up the
moment Claude **asks a question, raises an alert, or replies/finishes** (not on routine tool-use churn). With
**Auto sleep** on it dims out after ~60s of no touch; press BOOT or touch it to wake. Of the three side
buttons, **RESET** reboots the deck and **PWR** (middle) is the hardware power button (hold to power off) —
only **BOOT** is software-controlled.

**Multiple sessions — even across computers:** run `claudeq` in more terminals or projects, on this Mac
*or any other machine on the same network*. The deck auto-discovers every bridge (mDNS) and merges all
their sessions into one chip strip. Whichever chip is focused is the one your taps, macros, and voice
drive; a question auto-focuses the session that asked, wherever it's running.

---

# Customize
- **Macros** — edit `bridge/macros.json` (`id` / `icon` / `label` / `prompt`), restart the bridge.
- **Sounds** — drop any audio file at `bridge/sounds/alert.*` or `done.*` (auto-converted via ffmpeg).
- **Voice** — `CCDECK_VOICE_AUTOSUBMIT=0` types the transcript without pressing Enter;
  `CCDECK_WHISPER_MODEL=/path` points at the model (set during voice setup above; needed for a `brew`
  install, where there's no in-repo default); `CCDECK_WHISPER_BIN=/path` if `whisper-cli` isn't on `PATH`.
- **Machine name** — when two computers run a session with the same project name, the deck tags those
  chips with `@host` to tell them apart. The name is the hostname by default; set `CCDECK_HOST=mylaptop`
  before launching a bridge for a friendlier label.
- **Port** — `CCDECK_PORT` (bridge, launcher, and hooks all honor it).
- **Re-flash** — `claudeq flash` (auto-detects the port) or the browser flasher; firmware lives in
  `firmware/dist/`.

# Troubleshooting
- **Deck won't connect** — confirm the deck and the computer are on the **same 2.4 GHz network** and the
  bridge is up (`curl localhost:8787/health`). The deck finds the bridge over mDNS; if your network blocks
  mDNS/Bonjour, set a fixed *Bridge address* on the deck's WiFi-setup screen.
- **Questions not reaching the deck** — make sure you launched via `claudeq` (not plain `claude`) and
  `curl localhost:8787/health` shows `clients ≥ 1`.
- **No deck connected** — hooks fall through to Claude Code's normal in-terminal picker, so nothing breaks.
- **Browser won't flash** — WebSerial needs desktop **Chrome/Edge**; Safari/Firefox can't. Or use
  `claudeq flash`.
- **`claudeq flash` finds no device** — plug in USB-C; pass the port explicitly if needed
  (`claudeq flash /dev/cu.usbmodemXXXX`).
- **Tofu boxes on screen** — firmware text must be ASCII or `LV_SYMBOL_*`; the device font has no emoji.

---

<details>
<summary><b>Build from source</b> (developers — change the firmware or run the bridge unpackaged)</summary>

You only need the toolchain to **change** the firmware; flashing a finished build never does.

```bash
# prerequisites
brew install cmake ninja node tmux ffmpeg
# + ESP-IDF v5.4.1 — https://docs.espressif.com
```

**Firmware:**
```bash
git submodule update --init --recursive   # microlink (Tailscale client) is a submodule
. ~/esp/esp-idf/export.sh
cd firmware/companion
idf.py build
idf.py -p "$(ls /dev/cu.usbmodem* | head -1)" flash     # or: ../dist/build-dist.sh && claudeq flash
```
`../dist/build-dist.sh` repackages a fresh build into `firmware/dist/` + `flasher/` (merged image +
ESP Web Tools manifest). Bump `firmware/dist/VERSION` for releases. See `firmware/dist/README.md`.

> _Optional:_ to pre-seed WiFi at build time and skip the on-device portal, create
> `firmware/companion/main/wifi_secret.h` (`#define WIFI_SSID …` / `WIFI_PASSWORD …`) and set
> `BRIDGE_HOST` in `app_config.h`. The on-device setup always overrides these.

**Bridge, without brew:** run it straight from the repo — put `bridge/` on your `PATH` and use
`claudeq` directly (it resolves its own siblings). `npm install` in `bridge/` first.

</details>

<details>
<summary><b>How it works</b> (architecture &amp; internals)</summary>

```
 Claude Code ×N            Claude Code ×N         (each launched with `claudeq`, in tmux)
   │ hooks                    │ hooks
   ▼                          ▼
 bridge.mjs (Mac A) ─┐      bridge.mjs (Mac B) ─┐   each advertises itself via mDNS (_claudeq._tcp)
   :8787             │        :8787             │
                     └──WS──▶ ESP32-S3 deck ◀─WS─┘   discovers & connects to ALL bridges
                             (portrait 172×640)      (taps / focus / voice route back per-bridge)
```

Each computer runs its own `bridge.mjs`, which advertises on the LAN via mDNS (`_claudeq._tcp`) and keeps
a registry of *that machine's* live Claude sessions (keyed by `session_id`) — each with a tmux target,
title, status, and pending question. The deck discovers and connects to every bridge, merges their
sessions into one chip strip, and focuses one at a time; taps/macros/voice route back to the focused
session's own bridge. The bridge IP is never configured — it's found automatically.

Two ways the deck reaches a session:
- **Answering questions** rides the hook's own HTTP long-poll → no tmux needed, works across machines.
- **Macros / voice** are typed in via `tmux send-keys` into the focused session's pane → local only.

| Path | What |
|---|---|
| `firmware/companion/` | ESP-IDF firmware (LVGL UI, WiFi, WebSocket client); native portrait 172×640 |
| `firmware/dist/` | pre-built binaries + merged image + `flash.sh` + `build-dist.sh` (no toolchain to flash) |
| `flasher/` | browser firmware flasher (ESP Web Tools); deployed to GitHub Pages |
| `bridge/bridge.mjs` | Node bridge: hooks intake (HTTP) + device link (WS) + per-session tmux injection |
| `bridge/claudeq` | the launcher — starts the bridge, names the session, runs Claude (`claudeq flash` too) |
| `bridge/cc` | back-compat shim → forwards to `claudeq` |
| `bridge/ccdeck-settings.json`, `bridge/hooks/` | Claude Code hooks (carry session id + tmux target) |
| `bridge/mock-device.html` | browser stand-in for the device (`http://localhost:8787/`; `?b=8787,8788` to mock several bridges) |
| `packaging/homebrew/` | Homebrew formula for the bridge |
| `docs/PINOUT.md` | hardware pinout, V1/V2 detection, audio gotchas |
| `docs/PROTOCOL.md` | device ↔ bridge WebSocket message protocol |
| `docs/HARDWARE.md` | alternative boards for a quick hardware swap, ranked by port effort |

</details>
