# Publishing & releasing Claudeq

How the no-tools install is wired, and how to ship a new version.

## The three repos
| Repo | Holds | URL |
|---|---|---|
| `Positronico/claudeq` | this project (firmware, bridge, dist, docs, CI) | https://github.com/Positronico/claudeq |
| `Positronico/positronico.github.io` | the browser flasher, under `/claudeq/` | https://positronico.github.io/claudeq/ |
| `Positronico/homebrew-tap` | the Homebrew formula (`Formula/claudeq.rb`) | `brew install Positronico/tap/claudeq` |

## What a new owner does
1. Open **https://positronico.github.io/claudeq/** in Chrome/Edge → **Install** (firmware, ~30 s).
2. `brew install Positronico/tap/claudeq` (the bridge).
3. Join the `Claudeq-setup` WiFi on their phone → enter their network.
4. `cd project && claudeq`.

No compiler, no ESP-IDF, no Python.

## CI/CD (in `Positronico/claudeq`)
- **`.github/workflows/ci.yml`** — every push/PR: builds the ESP-IDF firmware (re-fetching managed
  components from `dependencies.lock`), repackages the dist, and sanity-checks the bridge.
- **`.github/workflows/release.yml`** — on a `vX.Y.Z` tag: builds the firmware and attaches the
  flashable binaries (`claudeq-firmware.bin`, parts, `manifest.json`) to a GitHub Release.

## Cutting a new firmware version
1. Build + repackage locally (or let CI do it): `cd firmware/companion && idf.py build && ../dist/build-dist.sh`
   (bump `firmware/dist/VERSION` first). Commit the updated `firmware/dist/claudeq-firmware.bin`.
2. Tag and push: `git tag vX.Y.Z && git push --tags` → `release.yml` publishes the binaries.
3. **Web flasher + OTA host:** copy `firmware/dist/{claudeq-firmware.bin,manifest.json,claudeq-app.bin,ota.json}`
   and `flasher/index.html` into the `claudeq/` folder of the `positronico.github.io` repo and push. This
   serves BOTH the full-flash web installer (`claudeq-firmware.bin` + `manifest.json`) AND the OTA payload the
   running deck pulls (`ota.json` → `claudeq-app.bin`). **The OTA path only works once these are live on
   Pages** — the deck fetches `https://positronico.github.io/claudeq/ota.json` and compares its `version` to
   the deck's `DEVICE_FW`, so the `ota.json` version MUST match this release.
4. **Homebrew:** in `homebrew-tap`, update `Formula/claudeq.rb` `url` to the new tag and refresh the
   `sha256`:
   ```bash
   curl -L https://github.com/Positronico/claudeq/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
   ```
   then `brew install --build-from-source Positronico/tap/claudeq` to smoke-test. **Also sync the in-repo
   copy `packaging/homebrew/claudeq.rb`** to the same `url`/`sha256` and commit it here — it's the
   reference copy kept in lockstep with the tap (see the `homebrew: sync in-repo formula copy to vX.Y.Z`
   commits); letting it drift from `homebrew-tap`'s `Formula/claudeq.rb` is a release bug.

## Secrets / hygiene
`app_config.h` only ever compiles in placeholder WiFi credentials (`YOUR_WIFI_SSID`/`YOUR_WIFI_PASSWORD`)
— **never** a real SSID/password. A prior version of this project supported a local-only
`wifi_secret.h` build-time override for developer convenience; that meant a locally built binary with
real credentials baked in was exactly what got committed to `firmware/dist/` and published (git
history, GitHub Releases, the web flasher, Homebrew) — a real secret leak. That mechanism was removed;
WiFi is provisioned on-device only (the SoftAP setup portal), never at compile time. **Before cutting any
release, scan the actual published binaries — not just the source — for your real WiFi SSID and password.**
Source can read clean while a stale, locally built image sitting in `firmware/dist/` still carries baked-in
credentials, so grep the bytes that actually ship:
```bash
strings firmware/dist/claudeq-app.bin      | grep -i '<your-wifi-ssid>'    # must print nothing
strings firmware/dist/claudeq-firmware.bin | grep -i '<your-wifi-ssid>'    # repeat for the password
```
Any match means a credential leaked into the flashable image — **stop, rebuild from clean sources, and
re-scan before releasing.** (This is mandatory: a real password once shipped in every published binary.)
The top-level `.gitignore` also keeps the 141 MB whisper model, `firmware/*/build`,
`firmware/*/managed_components` (~200 MB, re-fetched on build), `vendor/` reference SDKs, and logs out
of the repo. `bridge/node_modules` **is** committed on purpose so `brew install` works offline
(Homebrew sandboxes network during install).
