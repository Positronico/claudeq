# Claudeq — device ↔ bridge WebSocket protocol

JSON messages over a WebSocket. Bridge listens on `ws://<mac-ip>:8787`. The device
(or the browser mock) is the client. Binary frames carry audio (mic PCM upload, sound-clip download).

**Pairing required.** A connection starts able to exchange only `hello`/`hello_ack` and the pairing/auth
handshake types below — nothing else. Once a device and a bridge have paired (see "Pairing"), every
reconnect runs a fresh authentication handshake and all application traffic (`sessions`, `ask`, `focus`,
`answer`, macros, voice, activity, alerts, sound clips — every row in the two tables below) travels wrapped
in an AES-256-GCM `sec` envelope. See "Authentication & encryption" for the full model.

## Pairing
Either side can start pairing against an already-connected socket: on the device, open Settings → **Paired
Bridges**, where any connected-but-unpaired bridge shows up as a tappable **"New bridge — tap to pair →"**
row (tapping it starts the ceremony); or the bridge operator runs **`claudeq pair`**. Both derive an ephemeral
X25519 key pair, exchange public keys, and independently compute a 6-digit code
(`HMAC-SHA256(ecdh_shared, sorted(pub_a, pub_b))`, truncated). The device displays its code on-screen; the
bridge CLI prints its own and asks "Codes match? [y/N]". This is a Bluetooth/Signal-style **Short
Authentication String** — a live ECDH exchange checked by a human, not a shared secret typed anywhere. If
an active MITM had relayed/substituted the exchange, the two independently-computed codes would differ.

Only when **both** sides confirm (device taps Confirm, bridge operator answers `y`) do they derive a
persistent PSK (`HMAC-SHA256(ecdh_shared, "claudeq-psk-v1")`) and store a trust record — device in NVS
(bounded table, `main/trust.cpp`), bridge in `~/.claudeq/trust.json` (override the dir with
`$CLAUDEQ_STATE_DIR`) keyed by the device's persistent `device_id`. Either a reject or a ~90s timeout aborts with nothing persisted. Ephemeral keys and the raw
ECDH shared secret are discarded immediately after deriving the PSK.

| type | direction | fields | meaning |
|---|---|---|
| `pair_request` | either | `pub` (base64 X25519 pubkey) | Starts a ceremony — sender generates a fresh ephemeral keypair for this pairing only |
| `pair_response` | the other side | `pub` (base64 X25519 pubkey) | Completes the exchange; both sides can now compute the SAS code |
| `pair_confirm` | either | — | This side's human confirmed the two displayed codes match |
| `pair_reject` | either | `reason`: `user`\|`timeout`\|`busy`\|`mismatch` | Abort before persisting anything |
| `pair_ack` | either | `ok`, `label` | Sent after this side has locally persisted the new trust record |

Management: bridge — `claudeq pair` (interactive), `claudeq devices [list]`, `claudeq devices disconnect
<id>`, `claudeq devices forget <id>`. Device — Settings → Paired Bridges: each paired bridge is a row with
a live-connected dot and **Disc.** (drops the live connection, trust untouched, re-authenticates on
reconnect) / **Forget** (revokes trust — a fresh pairing ceremony is required afterward) buttons; any
connected-but-unpaired bridge appears below them as a tappable **"New bridge — tap to pair →"** row that
starts a ceremony.

## Authentication & encryption
Every reconnect between an already-paired device and bridge runs a mutual handshake **before** any
application message is allowed, combining the stored PSK with a **fresh** ephemeral X25519 exchange (so
each connection gets its own session key — a stolen PSK alone doesn't decrypt a past session, and doesn't
let a passive eavesdropper decrypt any session, only lets an active attacker who steals the PSK authenticate
future connections).

| type | direction | fields | meaning |
|---|---|---|
| `auth_hello` | Device→Bridge | `nonce`, `pub` (fresh ephemeral X25519 pubkey) | Sent automatically once `hello_ack.paired` is true |
| `auth_challenge` | Bridge→Device | `nonce`, `pub` (fresh ephemeral X25519 pubkey) | Sent only if the device's `device_id` is in the bridge's trust store |
| `auth_verify` | either | `mac` | Proof of possession of the derived session key (device sends first, then the bridge) — mutual authentication completes once both are checked |
| `auth_reject` | Bridge→Device | `reason`: `unknown_device`\|`bad_mac` | No trust record for this device, or its `auth_verify` failed |

Once authenticated, every other message (in both directions) is wrapped: `{"type":"sec","n":<counter>,
"ct":<base64 AES-256-GCM ciphertext+tag>}`. Directional keys are derived from the session key (`d2b`/`b2d`);
`n` is a strictly-increasing per-direction counter (also the AES-GCM IV) — a non-increasing counter, or a
ciphertext that fails to authenticate, closes the connection rather than being silently dropped. Binary
frames get an equivalent lightweight `[8-byte counter][ciphertext][16-byte tag]` envelope instead of
base64+JSON, sharing the same per-direction counter sequence. A socket that never completes this handshake
may only exchange the pairing/auth message types above — every other message type is dropped.

All of this uses only primitives every platform here supports natively — X25519 ECDH, HMAC-SHA256,
AES-256-GCM — via `node:crypto` (bridge), WebCrypto `SubtleCrypto` (the browser mock), and mbedTLS +
microlink's vendored `x25519.c` (firmware). No PAKE (e.g. EC-JPAKE): Node has no built-in support for one
and there's no well-maintained npm package, so hand-rolling one just for the bridge side would be exactly
the kind of custom crypto reimplementation to avoid.

## Firmware updates (OTA) — out of band
Firmware updates do **not** use this WebSocket protocol or the bridge at all. The deck pulls directly
from GitHub Pages over HTTPS: it fetches `https://positronico.github.io/claudeq/ota.json`
(`{version, app, sha256}`), compares `version` to its own `DEVICE_FW`, and — on the user's tap in
Settings — streams `claudeq-app.bin` into the inactive OTA slot via `esp_https_ota`, then reboots. The
device already reports its running version to the bridge in the `hello` handshake (`fw`), but that is
informational only; the update path is device→GitHub. See the README "Updating (over-the-air)" section.

## Discovery & multiple bridges
Each bridge advertises itself on the LAN over **mDNS** as `_claudeq._tcp` (port 8787 by default).
The deck browses for that service and opens a WebSocket to **every** bridge it finds — so bridges
on several computers all feed one deck. The deck tags each session with the connection it came from
and routes `answer`/`focus`/`macro`/voice back to that same bridge. No bridge IP is configured on the
deck; an explicit address can still be set to **add** a fixed bridge (useful when mDNS is blocked) — it is
additive, not exclusive: mDNS and tailnet discovery keep connecting to every other bridge alongside it.

## Multi-session model
Each bridge tracks every live Claude Code session on its machine (keyed by `session_id`), each with a
tmux target, project `title`, status, an optional pending question, and an attention flag. The deck
**merges sessions from all connected bridges** into one chip strip and remembers which single session is
**focused** (and on which bridge). Status/HUD/ask messages a bridge pushes are for *its* focused session;
the deck displays only the ones matching its globally-focused session. Macros and voice inject into the
focused session's tmux target on its bridge. A question auto-focuses the session that asked, on any bridge.

## Bridge → Device
| type | fields | meaning |
|---|---|---|
| `hello_ack` | `bridge_id`, `host`, `fw`, `paired` | Always sent in reply to `hello`, on any connection regardless of trust — identity only, no session data. `paired` tells the device whether to automatically start the auth handshake. |
| `sessions` | `list[]` = `{sid, title, needs}`, `focus` (sid), `host`, `bridge_id` | Session chips + which is focused + the bridge's machine name + a stable per-process id. `needs` = pending question/attention. The deck shows `host` on a chip only when that title also exists on another bridge (a cross-machine clash). `bridge_id` lets the deck recognize that the same bridge reached via the LAN *and* the tailnet (two different IPs) is one machine, and show its sessions once (keeping both connections, preferring the LAN path, failing over instantly if it drops). Override with `CCDECK_HOST` / `CCDECK_BRIDGE_ID`. Only sent post-authentication (wrapped in `sec`), but `bridge_id` is learned earlier from `hello_ack` so dedup works before any session data arrives. |
| `status` | `sid`, `state` (`idle`/`thinking`/`working`/`waiting`/`done`/`error`), `text`, `tool` | Live state of the focused session for the status strip |
| `ask` | `id`, `sid`, `questions[]` = `{question, header, multiSelect, options[]:{label,description}}` | A question to render; user taps to choose. Sent **after** the `sessions` message that focuses `sid` |
| `ask_cancel` | `sid`, `id?`, `reason` | Question resolved/cancelled (or focus moved to a session with no pending question) — clear it |
| `hud` | `sid`, `model`, `elapsedMs`, `tokens`, `costUSD`, `lastTool`, `todos[]`, `cwd` | Telemetry of the focused session. Still sent by the bridge, but the deck no longer renders a HUD strip (the session state moved to the bottom status strip). |
| `activity` | `sid`, and **either** `line` = `{ts, kind, label, detail, full?}` (append one) **or** `feed[]` (full snapshot, replace) | Live "what's Claude doing" feed for the Session screen. `kind` ∈ `tool`/`prompt`/`notify`/`done`/`reply`/`start`; a `reply` line also carries `full` = the complete (deck-safe, ≤2000 char) reply text — tapping the reply row opens it in the deck's landscape reader. `label` is the tool name (or `you`/`done`), `detail` the target (file, command, pattern…). The `start` line's `detail` is the **model name** (falling back to the project folder if unknown). The deck wakes the screen from standby on `ask`/`alert`/`reply`/`done`/`notify`, not on routine `tool`/`prompt`/`start`. A snapshot is sent on focus/connect (**after** any `ask`, so the deck has adopted the new focus before it lands); single lines stream as events arrive. Only for the focused session. The deck clears its feed on every focus change so one session's activity never appends onto another's. |
| `macros` | `items[]` = `{id,label,icon,prompt}` | The macro deck contents (global) |
| `alert` | `sid`, `level` (`info`/`attn`/`error`), `text`, `sound` (`chirp`\|`soft`\|`error`) | Pull-attention: flash + play the named built-in tone. The `sound` field is **omitted** when a custom clip exists at `~/.claudeq/sounds/alert.*` (for `attn`) or `~/.claudeq/sounds/done.*` — the bridge streams that PCM clip instead, and dropping the tone field keeps the tone and the clip from double-playing. |
| `transcript` | `id`, `text`, `sid` | Voice: the transcript for capture `id`, awaiting on-device confirmation. Replaces the old auto-inject — the deck shows `text` with **Send/Cancel** and the bridge holds it until `voice_commit`/`voice_cancel` (or 120 s). Empty `text` = nothing recognized. |

## Device → Bridge
| type | fields | meaning |
|---|---|---|
| `hello` | `name`, `fw`, `device_id`, `caps[]` | Device announces itself on connect — always allowed, no trust required. `device_id` is a persistent identifier generated once on first boot (NVS), replacing the old fixed `"claudeq"` literal every unit used to send. |
| `focus` | `sid` | Switch the focused session (user tapped a chip) |
| `answer` | `id`, `answers` = `{ "<question text>": "<label>" }` (multi = comma-joined) | User's selection(s) for an `ask`. Resolved by the global `id`, so it is robust even if focus changed |
| `macro` | `id` | Fire a macro by id → injected into the focused session |
| `perm` | `id`, `decision` (`allow`/`deny`) | Answer a permission prompt (if used) |
| `voice_start` / `voice_end` | `id` | Brackets a mic capture (audio as binary PCM frames between). On `voice_end` the bridge transcribes and returns a `transcript` message (id-matched) instead of injecting directly |
| `voice_commit` | `id` | Confirm the held transcript for capture `id` → injected (and submitted, per `CCDECK_VOICE_AUTOSUBMIT`) into the session that was focused when it was captured |
| `voice_cancel` | `id` | Discard the held transcript for capture `id` |
| `ping` | — | Keepalive |

## How the bridge learns a session's identity
Every hook carries `session_id` + `cwd` (from Claude Code). The `claudeq` launcher additionally
exports `CLAUDEQ_TMUX` (the tmux target) and `CLAUDEQ_TITLE` (project name); the hooks forward
those — `event.sh` via `x-claudeq-tmux` / `x-claudeq-title` headers, `askquestion.mjs` in the
`/ask` body's `session` object. The first event from a session registers it; a `tmux has-session`
sweep prunes it when its pane is gone.

The hook commands in `ccdeck-settings.json` locate their scripts via **`$CLAUDEQ_HOME`**, which the
`claudeq` launcher exports (pointing at the bridge install dir). Running `claude --settings
.../ccdeck-settings.json` **directly, without the launcher**, leaves `$CLAUDEQ_HOME` unset, so every hook
path fails to resolve and **all deck events break** — always go through `claudeq`.

## CC-facing answer mechanism (bridge ↔ Claude Code)
Decided empirically (see live tmux test):
- **A. Hook short-circuit** (preferred): the `PreToolUse` hook on `AskUserQuestion`
  long-polls the bridge and prints the chosen answer JSON to stdout, bypassing the
  TUI picker. No tmux needed.
- **B. tmux keystroke injection** (fallback): the hook forwards options + exits; on
  tap the bridge runs `tmux send-keys` against the CC pane to drive the picker.
