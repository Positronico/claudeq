# Claudeq

A Waveshare **ESP32-S3-Touch-LCD-3.49** turned into a physical control surface for **Claude Code**.

When Claude asks you something, the options pop up on the little touchscreen and you **tap to answer**.
The screen shows what Claude is doing, chirps when it needs you, and has a macro deck and tap-to-talk
voice — with an on-screen **Send/Cancel** so nothing reaches Claude until you approve it. Run it across
**as many Claude sessions as you like** — switch between them from a one-tap session picker. It even
**updates itself over the air**.

<p align="center"><i>Tap to answer · glance at status · fire macros · talk to Claude — for every session, from one little screen.</i></p>

<p align="center"><img src="photos/claudeq-demo.gif" alt="Claudeq demo: a session's activity feed filling in, a second session's chip appearing, an AskUserQuestion prompt appearing, and a real tap answering it" width="360"></p>

## What it does
- **Tap to answer** — Claude's `AskUserQuestion` options appear on-screen; tap one. Multi-question prompts step through each.
- **Live activity feed** — a running view of what Claude is doing (reading, editing, running, thinking), and its replies.
- **Every session on one screen** — sessions show under the same names Claude Code uses locally (`/rename` included); a bell badge marks the one that needs you, and a question auto-focuses whoever asked.
- **Across computers** — decks auto-discover every bridge on the LAN (mDNS) and over your **Tailscale** tailnet, merging all their sessions.
- **Macro deck** — one tap fires a saved prompt or slash-command into the focused session.
- **Tap-to-talk voice** — speak, review the local transcription (no API key), then **Send** — nothing reaches Claude until you approve.
- **Speaker alerts** — it chirps when Claude needs you (mutable).
- **Physical controls + low power** — BOOT-button brightness, **lock** (pocket mode), auto-sleep, wake-on-event, and a battery gauge.
- **On-device setup** — WiFi and Tailscale are entered on the device via a captive-portal; no rebuild to change networks.
- **Over-the-air updates** — check for and install new firmware straight from the Settings tab, downloaded from GitHub over HTTPS. No cable.

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
Download the model to the bridge's default location (works for both the `brew` and from-source installs):
```bash
brew install whisper-cpp
mkdir -p ~/.claudeq/whisper
curl -L -o ~/.claudeq/whisper/ggml-base.en.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
```
That's it — the bridge looks in `~/.claudeq/whisper/` by default (set `CCDECK_WHISPER_MODEL` only for a
non-standard path). `whisper-cli` is found on your `PATH`; set `CCDECK_WHISPER_BIN` only if it lives
somewhere unusual. If the bridge was already running, `claudeq restart-bridge` picks the model up.

---

# Use

In any project, start Claude Code with **`claudeq`** instead of `claude`:
```bash
cd ~/my-project
claudeq
```
That's the whole workflow. `claudeq` starts the bridge if needed, runs Claude in a tmux session, and
enables the deck. On the deck the session appears under **the same name Claude Code shows locally**
(the auto-generated session title, kept in sync when you `/rename` it), falling back to the project
folder name on Claude versions that don't expose it. Plain `claude` is unaffected.

`claudeq` also has a few subcommands (run **`claudeq help`** for the full list):

| Command | What |
|---|---|
| `claudeq flash [port]` | Write the firmware to the device over USB — no toolchain needed. |
| `claudeq pair` | Pair a connected-but-untrusted deck (interactive). |
| `claudeq devices [list]` | List paired decks + their live connection status (`claudeq devices disconnect <id>` / `forget <id>` to manage). |
| `claudeq start-bridge` / `stop-bridge` / `restart-bridge` | Start, stop, or restart the background bridge on its own (aliases: `start` / `stop` / `restart`). Run `restart-bridge` to pick up edits to macros, sounds, or the trust store. |

**On the screen:**

| Do this | What happens |
|---|---|
| Claude asks a question | The options appear — **tap one to answer**. Multi-question prompts step through each. |
| **Tap the session bar** (top) | Opens the full-screen **session picker** — every session is a big tappable row; tap one to switch. The bar shows the focused session's name, the session count, and a bell badge when *another* session needs you (the picker opens scrolled to it). |
| **Macros** tab | Tap a saved prompt/slash-command → sent to the focused session. |
| **Voice** (mic) | **Tap** the mic and speak, then **Stop** → review the transcript and tap **Send** (or Cancel). |
| **BOOT button** (side) | **Short press** cycles screen brightness; **hold** (~1.5s) **locks** the deck (screen dark, touch ignored — safe to pocket). Another **hold** unlocks and restores the brightness. |
| **Top bar** (icons only) | Four evenly-spaced indicators: the Claude mascot + **count of connected bridges**, the **Tailscale** logo (green when the tailnet is up, shown only if configured), **WiFi**, and the **battery** gauge (green → amber → red as it drains). |
| **Bottom strip** (above the buttons) | The focused session's live **status** (dot + text) — moved here so it has room to breathe. |
| **Settings** tab | Toggle **WiFi** / **Tailscale** / **Auto sleep** / **Sounds**, hold **WiFi portal** to reconfigure, or tap **Check for update** to update the firmware over-the-air. |

**Standby / low power:** the deck's biggest draw is the backlight, so standby just turns the **screen** off
(backlight + panel) while WiFi and the WebSocket stay up — so it's still reachable and lights back up the
moment Claude **asks a question, raises an alert, or replies/finishes** (not on routine tool-use churn). With
**Auto sleep** on it dims out after ~60s of no touch; press BOOT or touch it to wake. Of the three side
buttons, **RESET** reboots the deck and **PWR** (middle) is the hardware power button (hold to power off) —
only **BOOT** is software-controlled.

**Lock (pocket mode):** **hold BOOT** (~1.5s) to **lock** — the screen goes dark and both **touch and
incoming events are ignored**, so a stray touch in your pocket can't drive the focused Claude session.
Unlike auto-sleep, a locked deck stays dark and won't wake on a question or a tap; **only another
long-press** unlocks it, restoring the previous brightness and re-enabling touch. Sounds still play while
locked (if not muted), so you're still notified — you just can't act until you unlock.

**Updating (over-the-air):** open the **Settings** tab and tap **Check for update**. The deck asks GitHub
whether a newer firmware exists and, if so, offers **Update** — it then downloads the new image over HTTPS
straight from GitHub (no cable, no computer), shows a progress bar, and reboots into it. If anything goes
wrong mid-update the deck keeps running its current firmware, and a bad image that won't boot is
automatically rolled back. The deck needs internet on its WiFi for this (a tailnet-only path can reach your
bridges but not GitHub).

> **One-time catch for existing decks:** OTA needs a new flash-partition layout, and the partition layout
> itself can't be changed over-the-air. A deck flashed with a **pre-OTA** build (v1.0.0 or earlier) must be
> updated **once** over USB — via the [browser flasher](https://positronico.github.io/claudeq/) or
> `claudeq flash` — to the first OTA-capable release. **After that, every update is over-the-air.**

**Multiple sessions — even across computers:** run `claudeq` in more terminals or projects, on this Mac
*or any other machine on the same network*. The deck auto-discovers every bridge (mDNS) and merges all
their sessions into one picker. Whichever session is focused is the one your taps, macros, and voice
drive; a question auto-focuses the session that asked, wherever it's running.

> 🔒 **Security — pairing required.** The device↔bridge link is now authenticated and encrypted: a deck
> and a bridge must complete a one-time pairing (live numeric-comparison code, like Bluetooth/Signal
> pairing — no password to type) before the bridge will send session data or accept commands. To pair, open
> the deck's **Settings → Paired Bridges**: any connected-but-unpaired bridge appears there as a tappable
> **"New bridge — tap to pair →"** row — tap it to start the ceremony. (Or run **`claudeq pair`** on the
> bridge machine.) After pairing, every reconnect re-authenticates automatically and all traffic is AES-256-GCM
> encrypted. Manage paired devices with `claudeq devices [list]` / `claudeq devices disconnect <id>` /
> `claudeq devices forget <id>`. See `docs/PROTOCOL.md`'s "Pairing" section for the full model.
>
> **Upgrading from an older version?** Pairing wasn't there before — after upgrading both the bridge and
> the firmware, every existing deck↔bridge relationship needs one fresh pairing (there's no way to migrate
> "it just worked" into a trust relationship that never existed).

---

# Customize
Your editable state lives in **`~/.claudeq/`** (override the dir with `$CLAUDEQ_STATE_DIR`) — kept out of
the install dir so a `brew upgrade` can't wipe it; an existing setup's files are migrated there once,
automatically. After editing any of these, run **`claudeq restart-bridge`** to apply them.
- **Macros** — edit `~/.claudeq/macros.json` (`id` / `icon` / `label` / `prompt`), then `claudeq restart-bridge`.
- **Paired devices** — `~/.claudeq/trust.json` holds each paired device's persistent id and key; managed via
  `claudeq pair`/`claudeq devices`, not by hand. Delete it (then `claudeq restart-bridge`) to forget every pairing at once.
- **Sounds** — drop any audio file at `~/.claudeq/sounds/alert.*` or `~/.claudeq/sounds/done.*` (auto-converted
  via ffmpeg). With no custom clip, the deck plays a built-in tone instead.
- **Voice** — `CCDECK_VOICE_AUTOSUBMIT=0` types the transcript without pressing Enter;
  `CCDECK_WHISPER_MODEL=/path` overrides the model location (default: `~/.claudeq/whisper/ggml-base.en.bin`);
  `CCDECK_WHISPER_BIN=/path` if `whisper-cli` isn't on `PATH`.
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

> WiFi is **never** set at build/compile time — only via the on-device SoftAP setup portal (first
> boot, a failed connection, or Settings → hold: WiFi portal). A prior version of this doc suggested a
> local `wifi_secret.h` build-time fallback; that meant a locally built binary baked the real SSID/
> password into the compiled image as plaintext, and that image is what ends up committed to
> `firmware/dist/` and published. Don't do that — always provision WiFi on-device.

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
title (Claude Code's own session name, read from `~/.claude/sessions/`), status, and pending question.
The deck discovers and connects to every bridge, merges their sessions into one picker, and focuses one
at a time; taps/macros/voice route back to the focused session's own bridge. The bridge IP is never
configured — it's found automatically.

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
