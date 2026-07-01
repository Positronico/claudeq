# Claudeq — device ↔ bridge WebSocket protocol

JSON messages over a WebSocket. Bridge listens on `ws://<mac-ip>:8787`. The device
(or the browser mock) is the client. Binary frames are reserved for audio.

## Discovery & multiple bridges
Each bridge advertises itself on the LAN over **mDNS** as `_claudeq._tcp` (port 8787 by default).
The deck browses for that service and opens a WebSocket to **every** bridge it finds — so bridges
on several computers all feed one deck. The deck tags each session with the connection it came from
and routes `answer`/`focus`/`macro`/voice back to that same bridge. No bridge IP is configured on the
deck; an explicit address can still be set to force a single fixed bridge.

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
| `sessions` | `list[]` = `{sid, title, needs}`, `focus` (sid), `host`, `bridge_id` | Session chips + which is focused + the bridge's machine name + a stable per-process id. `needs` = pending question/attention. The deck shows `host` on a chip only when that title also exists on another bridge (a cross-machine clash). `bridge_id` lets the deck recognize that the same bridge reached via the LAN *and* the tailnet (two different IPs) is one machine, and show its sessions once (keeping both connections, preferring the LAN path, failing over instantly if it drops). Override with `CCDECK_HOST` / `CCDECK_BRIDGE_ID`. |
| `status` | `sid`, `state` (`idle`/`thinking`/`working`/`waiting`/`done`/`error`), `text`, `tool` | Live state of the focused session for the status strip |
| `ask` | `id`, `sid`, `questions[]` = `{question, header, multiSelect, options[]:{label,description}}` | A question to render; user taps to choose. Sent **after** the `sessions` message that focuses `sid` |
| `ask_cancel` | `sid`, `id?`, `reason` | Question resolved/cancelled (or focus moved to a session with no pending question) — clear it |
| `hud` | `sid`, `model`, `elapsedMs`, `tokens`, `costUSD`, `lastTool`, `todos[]`, `cwd` | Telemetry of the focused session; shown as a compact strip at the bottom of the Session screen |
| `activity` | `sid`, and **either** `line` = `{ts, kind, label, detail, full?}` (append one) **or** `feed[]` (full snapshot, replace) | Live "what's Claude doing" feed for the Session screen. `kind` ∈ `tool`/`prompt`/`notify`/`done`/`reply`/`start`; a `reply` line also carries `full` = the complete (deck-safe, ≤2000 char) reply text — tapping the reply row opens it in the deck's landscape reader. `label` is the tool name (or `you`/`done`), `detail` the target (file, command, pattern…). A snapshot is sent on focus/connect (**after** any `ask`, so the deck has adopted the new focus before it lands); single lines stream as events arrive. Only for the focused session. The deck clears its feed on every focus change so one session's activity never appends onto another's. |
| `macros` | `items[]` = `{id,label,icon,prompt}` | The macro deck contents (global) |
| `alert` | `sid`, `level` (`info`/`attn`/`error`), `text`, `sound` | Pull-attention: chirp + flash |
| `transcript` | `id`, `text`, `sid` | Voice: the transcript for capture `id`, awaiting on-device confirmation. Replaces the old auto-inject — the deck shows `text` with **Send/Cancel** and the bridge holds it until `voice_commit`/`voice_cancel` (or 120 s). Empty `text` = nothing recognized. |

## Device → Bridge
| type | fields | meaning |
|---|---|---|
| `hello` | `name`, `fw`, `ip`, `caps[]` | Device announces itself on connect |
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

## CC-facing answer mechanism (bridge ↔ Claude Code)
Decided empirically (see live tmux test):
- **A. Hook short-circuit** (preferred): the `PreToolUse` hook on `AskUserQuestion`
  long-polls the bridge and prints the chosen answer JSON to stdout, bypassing the
  TUI picker. No tmux needed.
- **B. tmux keystroke injection** (fallback): the hook forwards options + exits; on
  tap the bridge runs `tmux send-keys` against the CC pane to drive the picker.
