#!/usr/bin/env node
// Claudeq bridge: connects Claude Code (via hooks) to the ESP32 deck (via WebSocket).
//   - POST /ask    : AskUserQuestion hook long-polls here; resolves when the deck taps a choice.
//   - POST /event  : other hooks push status/HUD/alerts here.
//   - WS  /        : deck (or browser mock) connects; receives ask/status/hud/macros/sessions/alert,
//                    sends answer/macro/voice/focus.
//
// Multi-session: every hook carries the Claude session id (+ the launcher's tmux target & project
// title). The bridge keeps a registry of live sessions and routes macros/voice to the *focused*
// one. The deck shows a chip per session; tapping a chip focuses it. Questions auto-focus the asker.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import os from 'node:os';
import { spawn, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import { WebSocketServer } from 'ws';
import * as trust from './trust.mjs';
import * as pcrypto from './crypto.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.env.CCDECK_PORT || '8787', 10);
const SHORT_HOST = (process.env.CCDECK_HOST || os.hostname() || 'host').replace(/\.local$/, '').split('.')[0].slice(0, 16);  // for the deck to label chips on title clashes; override with CCDECK_HOST
// Stable per-process id so the deck can tell that this bridge — reached via the LAN and via the tailnet
// at two different IPs — is ONE machine, and show its sessions once. Persisted in trust.json (unlike a
// bare per-process randomUUID()) so pairing survives a bridge restart; override with CCDECK_BRIDGE_ID.
const BRIDGE_ID = trust.BRIDGE_ID;
// Single source of truth for the release version: firmware/dist/VERSION ships in both the repo and the
// Homebrew keg (../firmware/dist relative to this file in both layouts). package.json is only a fallback.
const BRIDGE_FW = (() => {
  try { return fs.readFileSync(path.join(__dirname, '..', 'firmware', 'dist', 'VERSION'), 'utf8').trim(); } catch {}
  try { return JSON.parse(fs.readFileSync(path.join(__dirname, 'package.json'), 'utf8')).version || '0.0.0'; } catch { return '0.0.0'; }
})();
const DEFAULT_TMUX = process.env.CCDECK_TMUX_TARGET || 'ccdeck';   // fallback for legacy `cc`
const ASK_TIMEOUT_MS = parseInt(process.env.CCDECK_ASK_TIMEOUT_MS || '280000', 10);
const PRUNE_GRACE_MS = parseInt(process.env.CCDECK_PRUNE_GRACE_MS || '20000', 10);
const PAIR_TIMEOUT_MS = parseInt(process.env.CCDECK_PAIR_TIMEOUT_MS || '90000', 10);
// Mutable/user-customizable state lives in ~/.claudeq (see trust.mjs) — NOT the install dir, which under
// Homebrew is the versioned keg: files there are silently wiped on every `brew upgrade`.
const STATE_DIR = trust.STATE_DIR;
const MACROS_FILE = path.join(STATE_DIR, 'macros.json');
const LEGACY_MACROS_FILE = path.join(__dirname, 'macros.json');
const MOCK_FILE = path.join(__dirname, 'mock-device.html');
const SOUNDS_DIRS = [path.join(STATE_DIR, 'sounds'), path.join(__dirname, 'sounds')];  // user dir first, bundled second
const SOUND_SR = 16000;
const SOUND_EXTS = ['wav', 'mp3', 'm4a', 'ogg', 'flac', 'aiff', 'aif'];
const soundCache = {}; // name -> { mtime, pcm }

// Find sounds/<name>.<ext>, convert to 16k mono s16le PCM via ffmpeg, cache by mtime.
// Drop any audio file at ~/.claudeq/sounds/alert.* or done.* to customize — picked up automatically.
function loadSound(name) {
  let file = null;
  outer: for (const dir of SOUNDS_DIRS) {
    for (const e of SOUND_EXTS) { const p = path.join(dir, `${name}.${e}`); if (fs.existsSync(p)) { file = p; break outer; } }
  }
  if (!file) return null;
  const mt = fs.statSync(file).mtimeMs;
  const c = soundCache[name];
  if (c && c.mtime === mt) return c.pcm;
  const r = spawnSync('ffmpeg', ['-v', 'error', '-i', file, '-ar', String(SOUND_SR), '-ac', '1', '-f', 's16le', '-acodec', 'pcm_s16le', '-'], { maxBuffer: 64 * 1024 * 1024 });
  if (r.status !== 0 || !r.stdout || !r.stdout.length) { console.error(`[sound] ffmpeg failed for ${file}`); return c ? c.pcm : null; }
  soundCache[name] = { mtime: mt, pcm: r.stdout };
  console.log(`[sound] ${name} <- ${path.basename(file)} (${r.stdout.length} bytes pcm)`);
  return r.stdout;
}
function sendSound(name) {
  const pcm = loadSound(name);
  if (!pcm || !pcm.length) return;
  for (const ws of clients) sendSecureBinary(ws, pcm);
}

// ---------- state ----------
const clients = new Set();
const sessions = new Map();          // sid -> session object
const pendingAsks = new Map();        // ask id -> { resolve, timer, sid }
let focusedSid = null;
let sessionSeq = 0;
let macros = loadMacros();

// ---------- session names from Claude Code's own registry ----------
// Every running Claude Code process maintains ~/.claude/sessions/<pid>.json with its sessionId and
// live session `name` — the same title shown locally in /resume and updated by /rename. Chips use
// that name so the deck matches what the user sees in the terminal; the launcher's CLAUDEQ_TITLE
// (project folder) stays as the fallback for CC versions without the registry. Files are keyed by
// pid and can linger after exit, so match by sessionId and let the freshest updatedAt win.
const CC_SESSIONS_DIR = path.join(os.homedir(), '.claude', 'sessions');
const CC_NAMES_TTL_MS = 2000;   // events can arrive in bursts (every tool call) — cap registry reads
let ccNamesCache = { at: 0, bySid: new Map() };
function ccSessionNames() {
  if (Date.now() - ccNamesCache.at < CC_NAMES_TTL_MS) return ccNamesCache.bySid;
  const bySid = new Map();
  let files = [];
  try { files = fs.readdirSync(CC_SESSIONS_DIR); } catch {}
  for (const f of files) {
    if (!f.endsWith('.json')) continue;
    try {
      const d = JSON.parse(fs.readFileSync(path.join(CC_SESSIONS_DIR, f), 'utf8'));
      if (!d.sessionId || !d.name) continue;
      const prev = bySid.get(d.sessionId);
      if (!prev || (d.updatedAt || 0) > prev.updatedAt) bySid.set(d.sessionId, { name: String(d.name), updatedAt: d.updatedAt || 0 });
    } catch {}
  }
  // Claude Code rewrites these files IN PLACE (truncate+write, no rename), so a read can catch a
  // torn/empty file mid-write. Keep the previous resolution for any tracked sid missing from this
  // scan instead of flapping to the fallback title and back (two spurious broadcasts).
  for (const [sid, v] of ccNamesCache.bySid) if (!bySid.has(sid) && sessions.has(sid)) bySid.set(sid, v);
  ccNamesCache = { at: Date.now(), bySid };
  return bySid;
}
// The chip title for a session: Claude Code's live session name (registry), else the SessionStart
// hook's session_title, else the launcher's project title. Pure — never mutates session state.
function sessionTitle(s) {
  const cc = ccSessionNames().get(s.sid);
  return (cc && cc.name) || s.hookTitle || s.title;
}
// Re-resolve every session's name; true if any chip title changed (caller broadcasts). This is the
// ONLY place shownTitle (the acknowledged-as-broadcast name) may be written: call it ONLY from paths
// that broadcast on change (touchSession, sweepSessions) — a non-broadcasting caller would consume
// the pending change and the rename would never reach the deck.
function refreshTitles() {
  let changed = false;
  for (const s of sessions.values()) {
    const t = sessionTitle(s);
    if (t !== s.shownTitle) {
      if (s.shownTitle != null) console.log(`[session] title: ${s.shownTitle} -> ${t}`);
      s.shownTitle = t; changed = true;
    }
  }
  return changed;
}

function newSession(sid) {
  return {
    sid, seq: ++sessionSeq, tmux: DEFAULT_TMUX, title: 'session', cwd: null,
    hookTitle: null,                       // session_title from the SessionStart hook payload, if any
    shownTitle: null,                      // last title broadcast to the deck (registry name or fallback)
    state: 'idle', text: 'idle', tool: null, attn: false,
    hud: { model: null, elapsedMs: 0, lastTool: null, todos: [], cwd: null },
    feed: [],                              // recent activity lines (for the "follow along" screen)
    lastReplyId: '',                       // id of the last reply shown, so a flush race can't re-push a stale turn
    promptStartedAt: 0, pendingAsk: null, lastSeen: Date.now(),
  };
}
// Create/update a session from hook metadata. Returns the session.
function touchSession(meta) {
  const sid = meta && meta.sid;
  if (!sid) return null;
  let s = sessions.get(sid);
  const isNew = !s;
  if (!s) { s = newSession(sid); sessions.set(sid, s); }
  if (meta.tmux) s.tmux = meta.tmux;
  if (meta.title) s.title = meta.title;
  if (meta.cwd) { s.cwd = meta.cwd; s.hud.cwd = meta.cwd; }
  s.lastSeen = Date.now();
  if (isNew) { s.shownTitle = sessionTitle(s); if (!focusedSid) focusedSid = sid; broadcastSessions(); }
  else if (refreshTitles()) broadcastSessions();   // a /rename (or auto-name) shows up on the next event
  return s;
}

function loadMacros() {
  // one-time migration from the pre-2.1.5 install-dir location (wiped on brew upgrade)
  if (!fs.existsSync(MACROS_FILE) && fs.existsSync(LEGACY_MACROS_FILE)) {
    try { fs.mkdirSync(STATE_DIR, { recursive: true }); fs.copyFileSync(LEGACY_MACROS_FILE, MACROS_FILE); } catch {}
  }
  try { return JSON.parse(fs.readFileSync(MACROS_FILE, 'utf8')); }
  catch { return [
    { id: 'review',   icon: '🔍', label: 'Code review', prompt: '/code-review' },
    { id: 'tests',    icon: '✅', label: 'Run tests',   prompt: 'run the tests and fix any failures' },
    { id: 'commit',   icon: '💾', label: 'Commit',      prompt: '/commit' },
    { id: 'continue', icon: '▶',  label: 'Continue',    prompt: 'continue' },
    { id: 'explain',  icon: '💡', label: 'Explain',     prompt: 'explain what you just did and why' },
    { id: 'effort',   icon: '⚙',  label: 'Effort',      prompt: '/effort' },
  ]; }
}

// ---------- ws helpers ----------
function broadcast(obj) {
  for (const ws of clients) sendSecure(ws, obj);
}
function needsAttn(s) { return !!s.pendingAsk || s.attn; }
// UTF-8-byte-safe clip: the deck stores a title in a fixed 40-byte buffer, and deck-safe titles can
// still be multi-byte (Latin-1, typographic punctuation) — clip by BYTES at a code-point boundary
// (39 max, incl. the 3-byte ellipsis when clipped) so the firmware's snprintf never cuts mid-sequence.
function clipTitleBytes(str, maxBytes = 39) {
  if (Buffer.byteLength(str, 'utf8') <= maxBytes) return str;
  let out = '';
  for (const ch of str) {
    if (Buffer.byteLength(out + ch, 'utf8') > maxBytes - 3) break;
    out += ch;
  }
  return out + '…';
}
// The wire title: resolved, stripped to deck-font glyphs, byte-clipped — with a fallback chain so a
// name made entirely of glyphs the deck lacks (e.g. a CJK /rename) never yields a blank row.
function deckTitle(s) {
  const t = clipTitleBytes(deckSafe(sessionTitle(s)).trim());
  if (t) return t;
  return clipTitleBytes(deckSafe(s.title).trim()) || 'session';
}
function sessionsList() {
  // Titles resolve at send time (they live in CC's registry and change under us) but WITHOUT touching
  // shownTitle — this is called from non-broadcasting paths (/health, sendSnapshot) that must not
  // consume the pending-change flag refreshTitles() maintains for the broadcast gates.
  return [...sessions.values()].sort((a, b) => a.seq - b.seq)
    .map((s) => ({ sid: s.sid, title: deckTitle(s), needs: needsAttn(s) }));
}
function broadcastSessions() {
  broadcast({ type: 'sessions', list: sessionsList(), focus: focusedSid, host: SHORT_HOST, bridge_id: BRIDGE_ID });
}
function statusMsg(s) { return { type: 'status', sid: s.sid, state: s.state, text: s.text, tool: s.tool }; }
function hudMsg(s) { return { type: 'hud', sid: s.sid, ...s.hud }; }
function askMsg(s) { return { type: 'ask', id: s.pendingAsk.id, sid: s.sid, questions: s.pendingAsk.questions }; }

// ---------- activity feed (live "what's Claude doing") ----------
const FEED_MAX = 40;
function shortFile(p) { return p ? String(p).split('/').pop() : ''; }
// The deck font (font_deck_12/16) covers ASCII + Latin-1 + a curated set of typographic punctuation and
// our icon glyphs, but it renders raw text (no markdown). So strip markdown MARKERS (** ` # and list
// bullets) -- syntax, not characters -- and drop any glyph the font lacks (emoji, CJK, box-drawing) so
// nothing shows as a tofu box. Real punctuation the font has (em-dash, curly quotes, ellipsis, bullet,
// arrow) passes through. Applied to feed + status text only -- NOT to ask option labels, which the deck
// echoes back verbatim as the answer (sanitizing them could break AskUserQuestion's match).
function deckSafe(v) {
  if (v == null) return '';
  const t = String(v)
    .replace(/```[\s\S]*?```/g, ' ')     // fenced code block
    .replace(/`([^`]*)`/g, '$1')          // inline code
    .replace(/\*\*([^*]+)\*\*/g, '$1')      // **bold**
    .replace(/\*([^*\n]+)\*/g, '$1')        // *italic*
    .replace(/\*{2,}/g, '')                 // stray ** from truncation
    .replace(/^\s{0,3}#{1,6}\s+/gm, '')     // # headings
    .replace(/^\s{0,3}[-*+]\s+/gm, '- ');   // bullet lists -> "- "
  // keep only glyphs the deck font actually has (must match the font_deck build ranges)
  return t.replace(/[^\t\n\r\x20-\x7E\u00A0-\u00FF\u2013\u2014\u2018\u2019\u201C\u201D\u2022\u2026\u2192]/g, '');
}
function clip(v, n = 52) { const s = String(v || '').replace(/\s+/g, ' ').trim(); return s.length > n ? s.slice(0, n - 1) + '\u2026' : s; }
// Turn a tool call into a short "target" detail, by tool.
function describeTool(name, ti = {}) {
  if (!ti) ti = {};                    // tool_input may arrive as null, not just absent
  switch (name) {
    case 'Read': case 'Edit': case 'Write': case 'NotebookEdit':
      return shortFile(ti.file_path || ti.notebook_path);
    case 'Bash': return clip(ti.command);
    case 'Grep': case 'Glob': return clip(ti.pattern);
    case 'Task': case 'Agent': return clip(ti.description || ti.subagent_type);
    case 'WebFetch': try { return new URL(ti.url).hostname; } catch { return clip(ti.url); }
    case 'WebSearch': return clip(ti.query);
    case 'Skill': return clip(ti.command || ti.skill || ti.name);
    case 'TodoWrite': return ti.todos?.length ? ti.todos.length + ' items' : '';
    default: return '';
  }
}
// Short readable model name from a raw id, e.g. "claude-opus-4-20250514" -> "opus-4", "" if unknown.
function modelName(m) {
  if (!m) return '';
  return String(m).replace(/^claude-/, '').replace(/-\d{6,}$/, '');
}
// The model of the last assistant turn in the transcript ('' if none yet — e.g. a brand-new session).
function transcriptModel(transcriptPath) {
  try {
    const lines = fs.readFileSync(transcriptPath, 'utf8').split('\n');
    for (let i = lines.length - 1; i >= 0; i--) {
      if (!lines[i].trim()) continue;
      let o; try { o = JSON.parse(lines[i]); } catch { continue; }
      const m = o.message;
      if ((o.type === 'assistant' || m?.role === 'assistant') && m?.model) return m.model;
    }
  } catch {}
  return '';
}
// Pull Claude's last text response out of the session transcript (read on Stop) so the deck can show
// what Claude actually said, not just the tool activity. Returns '' if unavailable.
function lastReply(transcriptPath) {
  try {
    const lines = fs.readFileSync(transcriptPath, 'utf8').split('\n');
    for (let i = lines.length - 1; i >= 0; i--) {
      if (!lines[i].trim()) continue;
      let o; try { o = JSON.parse(lines[i]); } catch { continue; }
      const m = o.message;
      if (o.type !== 'assistant' && m?.role !== 'assistant') continue;
      if (!Array.isArray(m?.content)) continue;
      const text = m.content.filter((b) => b?.type === 'text').map((b) => b.text).join(' ').trim();
      if (text) return { text, id: o.uuid || m.id || String(i), model: m.model || '' };  // id = turn identity
    }
  } catch {}
  return null;
}
// Push Claude's reply once the transcript has it. The Stop hook can fire before the final assistant
// text message is flushed to disk (notably after extended thinking), so we retry briefly — and wait for a
// reply from a NEW transcript message (by id), so a flush race never re-shows the prior turn AND two
// turns with identical prose ("Done.") both still show.
function pushReplyWhenReady(sid, transcriptPath, tries, prevId) {
  const r = lastReply(transcriptPath);
  if (r && r.id !== prevId) {
    const s = sessions.get(sid);
    if (s) { s.lastReplyId = r.id; if (r.model) s.hud.model = r.model; pushFeed(s, 'reply', '', clip(r.text, 120), r.text); console.log(`[reply] ${r.text.length} chars -> ${s.title}`); }
  } else if (tries > 0) {
    setTimeout(() => pushReplyWhenReady(sid, transcriptPath, tries - 1, prevId), 200);
  }
}
// Append a line to a session's feed; stream it to the deck if that session is focused.
function pushFeed(s, kind, label, detail = '', full = null) {
  const line = { ts: Date.now(), kind, label: deckSafe(label), detail: deckSafe(detail) };
  if (full != null) line.full = deckSafe(full).slice(0, 2000);   // full text for the deck's landscape reader
  s.feed.push(line);
  if (s.feed.length > FEED_MAX) s.feed.shift();
  if (s.sid === focusedSid) broadcast({ type: 'activity', sid: s.sid, line });
}
function feedMsg(s) { return { type: 'activity', sid: s.sid, feed: s.feed }; }

// Push the focused session's full state to the deck (status + hud + ask/clear).
function pushFocused() {
  const s = sessions.get(focusedSid);
  if (!s) { broadcast({ type: 'status', sid: null, state: 'idle', text: 'idle', tool: null }); return; }
  broadcast(statusMsg(s));
  broadcast(hudMsg(s));
  if (s.pendingAsk) broadcast(askMsg(s));
  else broadcast({ type: 'ask_cancel', sid: s.sid, reason: 'focus' });
  broadcast(feedMsg(s));               // AFTER the ask: a question auto-focuses its session, so the deck
                                       // only adopts this focus when it sees the ask — send the feed last
                                       // or it's dropped by the deck's focus gate.
}
function setFocus(sid, push = true) {
  if (!sessions.has(sid)) return;
  focusedSid = sid;
  const s = sessions.get(sid);
  if (s.attn) { s.attn = false; }            // looking at it clears the attention flag
  // Tell the deck the new focus FIRST, so a following `ask` is accepted for this session.
  broadcastSessions();
  if (push) pushFocused();
}

// mutate session status; stream to the deck only if it's the focused one
function setSessionStatus(s, state, text, tool) {
  s.state = state; s.text = deckSafe(text ?? state); s.tool = tool ?? s.tool;
  if (s.sid === focusedSid) broadcast(statusMsg(s));
}

// ---------- tmux injection (macros / voice) ----------
function focusedTmux() { const s = sessions.get(focusedSid); return (s && s.tmux) || DEFAULT_TMUX; }
function injectToCC(text, submit = true, target = focusedTmux()) {
  if (!text) return;
  // type literal text, then optionally send Enter as a separate key event
  const t = spawn('tmux', ['send-keys', '-t', target, '-l', '--', text]);
  t.on('close', () => { if (submit) spawn('tmux', ['send-keys', '-t', target, 'Enter']); });
  t.on('error', (e) => console.error('[tmux] inject failed:', e.message));
}

// ---- voice: mic PCM -> WAV -> whisper.cpp -> inject ----
// Resolved off PATH by default (sh -c below), so it works on Apple-Silicon, Intel, and Linuxbrew
// alike; override with CCDECK_WHISPER_BIN for a non-standard install.
const WHISPER_BIN = process.env.CCDECK_WHISPER_BIN || 'whisper-cli';
// Model resolution order: env override, then the documented per-user location (~/.claudeq/whisper — where
// the README's setup downloads it), then the dev checkout's gitignored tools/ dir. The old default was
// ONLY the tools/ path, which no published install ever has.
const WHISPER_MODEL = process.env.CCDECK_WHISPER_MODEL
  || [path.join(STATE_DIR, 'whisper', 'ggml-base.en.bin'),
      path.join(__dirname, '..', 'tools', 'whisper-models', 'ggml-base.en.bin')].find(p => fs.existsSync(p))
  || path.join(STATE_DIR, 'whisper', 'ggml-base.en.bin');
const VOICE_AUTOSUBMIT = process.env.CCDECK_VOICE_AUTOSUBMIT !== '0';   // type+Enter by default; set 0 to type only
const VOICE_CONFIRM = process.env.CCDECK_VOICE_CONFIRM !== '0';         // on-device Send/Cancel; set 0 for legacy auto-inject
let voiceSeq = 0;
function pcmToWav(pcm, sr = 16000) {
  const ch = 1, bits = 16, dataLen = pcm.length, h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + dataLen, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(sr, 24); h.writeUInt32LE(sr * ch * bits / 8, 28); h.writeUInt16LE(ch * bits / 8, 32); h.writeUInt16LE(bits, 34);
  h.write('data', 36); h.writeUInt32LE(dataLen, 40);
  return Buffer.concat([h, pcm]);
}
function transcribeWav(wavPath) {
  if (!fs.existsSync(WHISPER_MODEL)) {
    console.error(`[voice] whisper model not found at ${WHISPER_MODEL} — see the README "Voice" setup (download to ~/.claudeq/whisper/)`);
    return '';
  }
  const r = spawnSync('/bin/sh', ['-c', `"${WHISPER_BIN}" -m "${WHISPER_MODEL}" -f "${wavPath}" -nt -np`], { maxBuffer: 16 * 1024 * 1024 });
  if (r.status !== 0) { console.error('[voice] whisper failed:', (r.stderr || '').toString().slice(0, 300)); return ''; }
  return (r.stdout || '').toString().replace(/\s+/g, ' ').trim();
}
function finishVoice(ws) {
  const chunks = ws._mic || []; ws._mic = []; ws._recording = false;
  const id = ws._voiceId;
  const pcm = Buffer.concat(chunks);
  console.log(`[voice] ${pcm.length} bytes (~${(pcm.length / 2 / 16000).toFixed(1)}s)`);
  let text = '';
  if (pcm.length < 16000 * 2 * 0.3) {
    console.log('[voice] too short, ignored');
  } else {
    const tmp = path.join(os.tmpdir(), `ccdeck-voice-${process.pid}-${++voiceSeq}.wav`);
    try { fs.writeFileSync(tmp, pcmToWav(pcm)); text = transcribeWav(tmp); fs.unlink(tmp, () => {}); }
    catch (e) { console.error('[voice] error', e.message); }
  }
  const s = sessions.get(focusedSid);
  console.log(`[voice] -> ${s ? s.title : '?'}: "${text}"`);
  if (!VOICE_CONFIRM) {   // legacy: inject immediately (old firmware / mock with no confirm step)
    if (text) { if (s) setSessionStatus(s, 'working', 'voice: ' + text.slice(0, 32)); injectToCC(text, VOICE_AUTOSUBMIT); }
    return;
  }
  // Return the transcript to the deck for on-device Send/Cancel; hold it (bound to the focus AT THIS MOMENT,
  // so a later focus change can't misroute the injection) until voice_commit/voice_cancel.
  // MUST go through the sec envelope: the device drops any non-handshake plaintext, so a raw ws.send
  // here never arrives and the deck times out with "Transcription failed" — voice was broken this way
  // on every encrypted connection from v2.1.0 until this line was fixed.
  sendSecure(ws, { type: 'transcript', id, text, sid: focusedSid });
  // Hold the transcript (bound to the focus captured NOW) until the deck confirms/cancels, the next
  // capture on this socket overwrites it, or the socket closes — no wall-clock expiry, so a slow
  // confirm can never desync (a late Send still injects into the originally-focused session).
  ws._pendingVoice = text ? { id, text, sid: focusedSid, tmux: focusedTmux() } : null;
}

// ---------- http ----------
function readBody(req) {
  return new Promise((resolve) => {
    let b = ''; req.on('data', (c) => (b += c)); req.on('end', () => resolve(b));
  });
}

// Poll `check` every `intervalMs` until it returns something other than `undefined`, or `timeoutMs`
// elapses (resolves `null` on timeout). Used by the /admin/pair/* long-polls below — pairing is a rare,
// human-paced admin action, so a simple poll loop is plenty and avoids wiring an event-emitter for it.
function waitFor(check, timeoutMs, intervalMs = 150) {
  return new Promise((resolve) => {
    const start = Date.now();
    (function tick() {
      const v = check();
      if (v !== undefined) { resolve(v); return; }
      if (Date.now() - start >= timeoutMs) { resolve(null); return; }
      setTimeout(tick, intervalMs);
    })();
  });
}
function isLoopback(addr) {
  return addr === '127.0.0.1' || addr === '::1' || addr === '::ffff:127.0.0.1';
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, `http://localhost:${PORT}`);

  // The ENTIRE plain-HTTP surface is loopback-only. The deck speaks exclusively WebSocket (paired +
  // encrypted); the HTTP routes exist for local hooks (/event, /ask), local admin (claudeq pair/devices),
  // and the local mock page. Without this gate, any LAN/tailnet peer could inject fake session events,
  // fake questions, and sounds onto the deck (or read session titles from /health).
  if (!isLoopback(req.socket.remoteAddress || '')) {
    res.writeHead(403, { 'content-type': 'application/json' });
    res.end('{"error":"loopback only"}');
    return;
  }

  if (req.method === 'GET' && (u.pathname === '/' || u.pathname === '/mock' || u.pathname === '/mock-device.html')) {
    fs.readFile(MOCK_FILE, (e, data) => {
      if (e) { res.writeHead(404); res.end('mock not found'); return; }
      res.writeHead(200, { 'content-type': 'text/html' }); res.end(data);
    });
    return;
  }
  if (req.method === 'GET' && u.pathname === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: true, fw: BRIDGE_FW, clients: clients.size, sessions: sessionsList(), focus: focusedSid })); return;
  }

  // AskUserQuestion hook -> long-poll until the deck answers
  if (req.method === 'POST' && u.pathname === '/ask') {
    const body = await readBody(req);
    let payload = {}; try { payload = JSON.parse(body); } catch {}
    const ti = payload.tool_input || {};
    const id = payload.id || 'ask-' + Date.now();
    const questions = ti.questions || [];
    const meta = payload.session || {};
    const sid = meta.sid || meta.tmux || 'default';
    const s = touchSession({ sid, tmux: meta.tmux, title: meta.title, cwd: meta.cwd });
    if (clients.size === 0) { // no deck connected -> let CC show its own picker
      res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ timeout: true, reason: 'no-device' })); return;
    }
    s.pendingAsk = { id, questions };
    setSessionStatus(s, 'waiting', 'waiting for you', 'AskUserQuestion');
    setFocus(s.sid);            // jump the deck to whoever is asking
    const answers = await new Promise((resolve) => {
      const timer = setTimeout(() => { pendingAsks.delete(id); resolve(null); }, ASK_TIMEOUT_MS);
      pendingAsks.set(id, { resolve, timer, sid: s.sid });
    });
    if (s.pendingAsk && s.pendingAsk.id === id) s.pendingAsk = null;
    if (answers) {
      setSessionStatus(s, 'working', 'working');
      broadcastSessions();
      res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ answers }));
    } else {
      broadcast({ type: 'ask_cancel', id, sid: s.sid, reason: 'timeout' });
      broadcastSessions();
      res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ timeout: true }));
    }
    return;
  }

  // Generic event hook -> drive status/HUD/alerts
  if (req.method === 'POST' && u.pathname === '/event') {
    const kind = u.searchParams.get('kind') || '';
    const body = await readBody(req);
    let ev = {}; try { ev = JSON.parse(body); } catch {}
    const meta = {
      sid: ev.session_id || req.headers['x-claudeq-sid'] || null,
      tmux: req.headers['x-claudeq-tmux'] || null,
      title: req.headers['x-claudeq-title'] || null,
      cwd: ev.cwd || null,
    };
    handleEvent(kind, ev, meta);
    res.writeHead(200, { 'content-type': 'application/json' }); res.end('{"ok":true}'); return;
  }

  if (u.pathname === '/play') {  // GET/POST: stream a sound to the deck(s) — for testing/manual play
    const name = u.searchParams.get('name') || 'alert';
    sendSound(name);
    res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ ok: true, name })); return;
  }

  // Device management for `claudeq pair` / `claudeq devices ...` (bridge/claudeq). Loopback-only: unlike
  // /ask (answer one question) or /event (push telemetry), these can disconnect or revoke a live paired
  // device, so they get a stricter trust bar than the rest of this HTTP surface.
  if (u.pathname.startsWith('/admin/')) {
    if (!isLoopback(req.socket.remoteAddress || '')) {
      res.writeHead(403, { 'content-type': 'application/json' }); res.end('{"error":"loopback only"}'); return;
    }
    const json = (obj, code = 200) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(JSON.stringify(obj)); };

    if (req.method === 'GET' && u.pathname === '/admin/devices/pending') { json(pendingPairings()); return; }

    if (req.method === 'GET' && u.pathname === '/admin/devices') {
      json(trust.listDevices().map(({ psk, ...d }) => {
        const ws = findSocketByDeviceId(d.deviceId);
        return { ...d, connected: !!ws, authenticated: !!(ws && ws._authenticated) };
      }));
      return;
    }

    if (req.method === 'POST' && u.pathname === '/admin/pair/start') {
      let payload; try { payload = JSON.parse((await readBody(req)) || '{}'); } catch { json({ ok: false, reason: 'bad-json' }, 400); return; }
      const ws = findSocketByDeviceId(payload.deviceId);
      if (!ws) { json({ ok: false, reason: 'not-connected' }, 404); return; }
      const started = startPairing(ws);
      if (!started.ok) { json(started); return; }
      const code = await waitFor(() => (!ws._pairing ? null : (ws._pairing.sas || undefined)), PAIR_TIMEOUT_MS);
      json(code ? { ok: true, code } : { ok: false, reason: 'no-response' });
      return;
    }

    if (req.method === 'POST' && u.pathname === '/admin/pair/confirm') {
      let payload; try { payload = JSON.parse((await readBody(req)) || '{}'); } catch { json({ ok: false, reason: 'bad-json' }, 400); return; }
      const ws = findSocketByDeviceId(payload.deviceId);
      if (!ws) { json({ ok: false, reason: 'not-connected' }, 404); return; }
      const r = confirmPairingLocal(ws);
      if (!r.ok) { json(r); return; }
      const paired = await waitFor(() => {
        if (trust.isPaired(payload.deviceId)) return true;
        if (!ws._pairing) return false;   // aborted after our confirm (peer rejected/timed out)
        return undefined;
      }, PAIR_TIMEOUT_MS);
      json({ ok: true, paired: !!paired });
      return;
    }

    if (req.method === 'POST' && u.pathname === '/admin/pair/reject') {
      let payload; try { payload = JSON.parse((await readBody(req)) || '{}'); } catch { json({ ok: false, reason: 'bad-json' }, 400); return; }
      const ws = findSocketByDeviceId(payload.deviceId);
      json(ws ? rejectPairingLocal(ws, payload.reason || 'user') : { ok: false, reason: 'not-connected' });
      return;
    }

    const dm = u.pathname.match(/^\/admin\/devices\/([^/]+)\/(disconnect|forget)$/);
    if (req.method === 'POST' && dm) {
      const id = decodeURIComponent(dm[1]);
      const ws = findSocketByDeviceId(id);
      if (dm[2] === 'disconnect') {
        if (ws) ws.close(4001, 'admin-disconnect');
        json({ ok: true, disconnected: !!ws });
      } else {
        const existed = trust.forgetDevice(id);
        if (ws) ws.close(4001, 'admin-forget');
        json({ ok: true, existed });
      }
      return;
    }

    json({ error: 'not found' }, 404); return;
  }

  res.writeHead(404); res.end('not found');
});

function handleEvent(kind, ev, meta) {
  const s = touchSession(meta);
  if (!s) return;                       // no session id -> can't attribute; ignore
  switch (kind) {
    case 'SessionStart': {
      // The hook payload's session_title is a documented name source (unlike the ~/.claude/sessions
      // registry, which is internal) — keep it as the mid-priority fallback in sessionTitle().
      if (typeof ev.session_title === 'string' && ev.session_title.trim()) s.hookTitle = ev.session_title.trim();
      const model = ev.model || (ev.transcript_path && transcriptModel(ev.transcript_path));
      if (model) s.hud.model = model;
      s.attn = false; setSessionStatus(s, 'idle', 'ready');
      // show the model on the start row (falls back to the project folder when it's not known yet)
      pushFeed(s, 'start', 'session', s.hud.model ? modelName(s.hud.model) : shortFile(s.cwd)); break;
    }
    case 'UserPromptSubmit':
      s.promptStartedAt = Date.now(); s.hud.elapsedMs = 0; s.attn = false;
      setSessionStatus(s, 'thinking', 'thinking');
      pushFeed(s, 'prompt', 'you', clip(ev.prompt)); broadcastSessions(); break;
    case 'PreToolUse':
      if (ev.tool_name) {
        s.hud.lastTool = ev.tool_name; setSessionStatus(s, 'working', 'working: ' + ev.tool_name, ev.tool_name);
        pushFeed(s, 'tool', ev.tool_name, describeTool(ev.tool_name, ev.tool_input));
      }
      break;
    case 'PostToolUse':
      if (ev.tool_name === 'TodoWrite' && ev.tool_input?.todos) {
        s.hud.todos = ev.tool_input.todos.map((t) => ({ content: t.content || t.subject || '', status: t.status }));
      }
      break;
    case 'Notification':
      s.attn = true; setSessionStatus(s, 'waiting', 'needs you');
      pushFeed(s, 'notify', 'needs you', clip(ev.message));
      // sound: the deck's built-in tone, UNLESS a custom sounds/alert.* clip exists — then stream that
      // instead and omit the tone field so the two never double-play.
      broadcast({ type: 'alert', sid: s.sid, level: 'attn', text: ev.message || 'Claude needs you', ...(loadSound('alert') ? {} : { sound: 'chirp' }) });
      sendSound('alert'); broadcastSessions(); break;
    case 'Stop':
      if (s.promptStartedAt) s.hud.elapsedMs = Date.now() - s.promptStartedAt;
      s.attn = false; setSessionStatus(s, 'done', 'done');
      pushFeed(s, 'done', 'done', s.hud.elapsedMs ? Math.round(s.hud.elapsedMs / 1000) + 's' : '');
      if (ev.transcript_path) pushReplyWhenReady(s.sid, ev.transcript_path, 12, s.lastReplyId || '');
      broadcast({ type: 'alert', sid: s.sid, level: 'info', text: 'done', ...(loadSound('done') ? {} : { sound: 'soft' }) });
      sendSound('done'); broadcastSessions(); break;
  }
  if (s.promptStartedAt && s.state !== 'idle') s.hud.elapsedMs = Date.now() - s.promptStartedAt;
  if (s.sid === focusedSid) broadcast(hudMsg(s));
}

// ---------- liveness sweep: drop sessions whose tmux pane is gone ----------
function tmuxAlive(target) {
  if (!target) return false;
  const r = spawnSync('tmux', ['has-session', '-t', target], { stdio: 'ignore' });
  return r.status === 0;
}
function sweepSessions() {
  // An idle session generates no hook traffic, so a /rename there would otherwise never reach the
  // deck — pick up registry name changes on the sweep tick too.
  let changed = refreshTitles();
  for (const [sid, s] of sessions) {
    if (Date.now() - s.lastSeen <= PRUNE_GRACE_MS) continue;
    // Keep only if it has a real, *distinct* tmux target that still exists; otherwise prune on TTL.
    // (Sessions sharing the fallback DEFAULT_TMUX must NOT be kept alive by it, or dead chips pile up.)
    if (s.tmux && s.tmux !== DEFAULT_TMUX && tmuxAlive(s.tmux)) continue;
    sessions.delete(sid); changed = true;
    console.log(`[session] pruned ${s.title} (${sid.slice(0, 8)})`);
    if (focusedSid === sid) {
      const next = [...sessions.keys()][0] || null;
      if (next) setFocus(next);                  // canonical: broadcastSessions() (focus) before pushFocused()
      else { focusedSid = null; pushFocused(); }
    }
  }
  if (changed) broadcastSessions();
}
setInterval(sweepSessions, 8000).unref();

// ---------- pairing / authentication ----------
// Every connection starts unauthenticated and may only exchange the types below (see docs/PROTOCOL.md's
// "Pairing" / "Authentication & encryption" sections). Everything else — session data, macros, focus,
// answers, voice — requires a completed per-connection auth handshake, and travels wrapped in a `sec`
// envelope from then on. Per-socket state (`ws._deviceId`, `_authenticated`, `_pairing`, `_authPending`,
// `_sessionKeys`, `_counters`) is initialized in `wss.on('connection', ...)` below.
const HANDSHAKE_TYPES = new Set([
  'hello', 'pair_request', 'pair_response', 'pair_confirm', 'pair_reject', 'pair_ack',
  'auth_hello', 'auth_verify', 'sec', 'ping',
]);

function findSocketByDeviceId(deviceId) {
  for (const ws of clients) if (ws._deviceId === deviceId) return ws;
  return null;
}

// A fresh directional AES key + 12-byte IV built from a strictly-increasing per-direction counter — safe
// because a brand-new session_key is derived from a fresh ephemeral ECDH exchange on every reconnect
// (see handleAuthHello), so a (key, IV) pair never repeats across the bridge's lifetime.
function sendSecure(ws, obj) {
  if (!ws._authenticated) return;   // an unauthenticated socket never receives app/session data
  const n = ++ws._counters.send;
  const ct = pcrypto.gcmEncrypt(ws._sessionKeys.send, n, Buffer.from(JSON.stringify(obj), 'utf8'));
  try { ws.send(JSON.stringify({ type: 'sec', n, ct: ct.toString('base64') })); } catch {}
}
// Binary frames (audio) get a lightweight envelope instead of base64+JSON: 8-byte BE counter, then the
// AES-GCM ciphertext+tag — sharing the same per-direction counter sequence as sendSecure's `sec` frames.
function sendSecureBinary(ws, buf) {
  if (!ws._authenticated) return;
  const n = ++ws._counters.send;
  const ct = pcrypto.gcmEncrypt(ws._sessionKeys.send, n, buf);
  const header = Buffer.alloc(8); header.writeBigUInt64BE(BigInt(n), 0);
  try { ws.send(Buffer.concat([header, ct]), { binary: true }); } catch {}
}

// ---- pairing ceremony: ephemeral X25519 ECDH + a human-verified 6-digit code (see crypto.mjs) ----
function pairingAbort(ws, reason) {
  if (ws._pairing?.timer) clearTimeout(ws._pairing.timer);
  ws._pairing = null;
  try { ws.send(JSON.stringify({ type: 'pair_reject', reason })); } catch {}
}
// Bridge-initiated: an admin action (claudeq pair) wants to pair with an already-connected device.
function startPairing(ws) {
  if (ws._pairing) return { ok: false, reason: 'busy' };
  const { privateKey, rawPublic } = pcrypto.genKeyPair();
  ws._pairing = {
    myPriv: privateKey, myPub: rawPublic, theirPub: null, shared: null, sas: null,
    confirmedByMe: false, confirmedByPeer: false,
    timer: setTimeout(() => pairingAbort(ws, 'timeout'), PAIR_TIMEOUT_MS),
  };
  try { ws.send(JSON.stringify({ type: 'pair_request', pub: rawPublic.toString('base64') })); } catch {}
  return { ok: true };
}
// Peer (device) initiated pairing — we're the responder, so we can compute the SAS immediately.
function handlePairRequest(ws, m) {
  if (ws._pairing) { try { ws.send(JSON.stringify({ type: 'pair_reject', reason: 'busy' })); } catch {} return; }
  let theirPub;
  try { theirPub = Buffer.from(m.pub, 'base64'); } catch { return; }
  if (theirPub.length !== 32) return;
  const { privateKey, rawPublic } = pcrypto.genKeyPair();
  const shared = pcrypto.ecdh(privateKey, theirPub);
  const sas = pcrypto.sas6(shared, rawPublic, theirPub);
  ws._pairing = {
    myPriv: privateKey, myPub: rawPublic, theirPub, shared, sas,
    confirmedByMe: false, confirmedByPeer: false,
    timer: setTimeout(() => pairingAbort(ws, 'timeout'), PAIR_TIMEOUT_MS),
  };
  console.log(`[pair] request from device ${ws._deviceId || '?'} — code ${sas} (confirm via 'claudeq pair')`);
  try { ws.send(JSON.stringify({ type: 'pair_response', pub: rawPublic.toString('base64') })); } catch {}
}
// Reply to a bridge-initiated pair_request — now we know both pubkeys, compute the SAS.
function handlePairResponse(ws, m) {
  const p = ws._pairing;
  if (!p || p.theirPub) return;   // no outstanding bridge-initiated request on this socket
  let theirPub;
  try { theirPub = Buffer.from(m.pub, 'base64'); } catch { return; }
  if (theirPub.length !== 32) return;
  p.theirPub = theirPub;
  p.shared = pcrypto.ecdh(p.myPriv, theirPub);
  p.sas = pcrypto.sas6(p.shared, p.myPub, theirPub);
  console.log(`[pair] code for device ${ws._deviceId || '?'} — ${p.sas} (confirm via 'claudeq pair')`);
}
function maybeFinalizePairing(ws) {
  const p = ws._pairing;
  if (!p || !p.confirmedByMe || !p.confirmedByPeer) return;
  clearTimeout(p.timer);
  const deviceId = ws._deviceId;
  if (!deviceId) { pairingAbort(ws, 'mismatch'); return; }   // shouldn't happen: hello always precedes pairing
  const psk = pcrypto.derivePsk(p.shared);
  trust.addDevice(deviceId, ws._deviceName || deviceId, psk);
  ws._pairing = null;
  console.log(`[pair] paired with device ${deviceId}`);
  try { ws.send(JSON.stringify({ type: 'pair_ack', ok: true, label: SHORT_HOST })); } catch {}
}
function handlePairConfirm(ws, m) {
  const p = ws._pairing;
  if (!p || !p.sas) return;   // peer confirmed before the ECDH exchange even completed — ignore
  p.confirmedByPeer = true;
  maybeFinalizePairing(ws);
}
function handlePairReject(ws, m) {
  if (!ws._pairing) return;
  console.log(`[pair] rejected by peer (${m.reason || 'unknown'})`);
  clearTimeout(ws._pairing.timer);
  ws._pairing = null;
}
function handlePairAck(ws, m) {
  console.log(`[pair] peer confirmed pairing complete (ok=${!!m.ok})`);
}
// Admin-facing actions (used by the /admin/pair/* HTTP routes — see below).
function confirmPairingLocal(ws) {
  if (!ws._pairing || !ws._pairing.sas) return { ok: false, reason: 'not-ready' };
  ws._pairing.confirmedByMe = true;
  try { ws.send(JSON.stringify({ type: 'pair_confirm' })); } catch {}
  maybeFinalizePairing(ws);
  return { ok: true };
}
function rejectPairingLocal(ws, reason = 'user') {
  if (!ws._pairing) return { ok: false };
  pairingAbort(ws, reason);
  return { ok: true };
}
function pendingPairings() {
  const out = [];
  for (const ws of clients) {
    if (!ws._deviceId || ws._authenticated) continue;
    out.push({
      deviceId: ws._deviceId, name: ws._deviceName,
      pairing: ws._pairing ? { code: ws._pairing.sas || null, confirmedByMe: ws._pairing.confirmedByMe, confirmedByPeer: ws._pairing.confirmedByPeer } : null,
    });
  }
  return out;
}

// ---- per-connection auth handshake: runs automatically once a hello_ack shows the device is already
// paired. Combines the long-term PSK with a fresh ephemeral ECDH every reconnect for forward secrecy. ----
function handleAuthHello(ws, m) {
  const deviceId = ws._deviceId;
  const psk = deviceId ? trust.pskFor(deviceId) : null;
  if (!psk) { try { ws.send(JSON.stringify({ type: 'auth_reject', reason: 'unknown_device' })); } catch {} return; }
  let nonceDevice, pubDeviceEph;
  try {
    nonceDevice = Buffer.from(m.nonce, 'base64'); pubDeviceEph = Buffer.from(m.pub, 'base64');
    if (nonceDevice.length !== 16 || pubDeviceEph.length !== 32) throw new Error('bad length');
  } catch { try { ws.send(JSON.stringify({ type: 'auth_reject', reason: 'bad_request' })); } catch {} return; }
  const { privateKey, rawPublic } = pcrypto.genKeyPair();
  const nonceBridge = pcrypto.randomBytes(16);
  const ecdhShared = pcrypto.ecdh(privateKey, pubDeviceEph);
  const transcript = pcrypto.authTranscript(nonceDevice, nonceBridge, pubDeviceEph, rawPublic);
  const sessionKey = pcrypto.deriveSessionKey(psk, ecdhShared, transcript);
  const dirs = pcrypto.directionalKeys(sessionKey);
  if (ws._authPending?.timer) clearTimeout(ws._authPending.timer);
  ws._authPending = { sessionKey, dirs, transcript, timer: setTimeout(() => { ws._authPending = null; }, PAIR_TIMEOUT_MS) };
  try { ws.send(JSON.stringify({ type: 'auth_challenge', nonce: nonceBridge.toString('base64'), pub: rawPublic.toString('base64') })); } catch {}
}
function handleAuthVerify(ws, m) {
  const ap = ws._authPending;
  if (!ap) return;
  let mac;
  try { mac = Buffer.from(m.mac, 'base64'); } catch { return; }
  const expected = pcrypto.authTag(ap.sessionKey, 'device', ap.transcript);
  if (mac.length !== expected.length || !crypto.timingSafeEqual(mac, expected)) {
    try { ws.send(JSON.stringify({ type: 'auth_reject', reason: 'bad_mac' })); } catch {}
    ws.close(4004, 'bad-auth-mac');
    return;
  }
  clearTimeout(ap.timer);
  ws._sessionKeys = { send: ap.dirs.b2d, recv: ap.dirs.d2b };
  ws._counters = { send: 0, recv: 0 };
  ws._authenticated = true;
  ws._authPending = null;
  try { ws.send(JSON.stringify({ type: 'auth_verify', mac: pcrypto.authTag(ap.sessionKey, 'bridge', ap.transcript).toString('base64') })); } catch {}
  console.log(`[auth] device ${ws._deviceId} authenticated`);
  sendSnapshot(ws);   // (no greeting sound from the bridge -- the deck plays its own tone on auth success)
}

// ---------- websocket ----------
function sendSnapshot(ws) {
  sendSecure(ws, { type: 'macros', items: macros });
  sendSecure(ws, { type: 'sessions', list: sessionsList(), focus: focusedSid, host: SHORT_HOST, bridge_id: BRIDGE_ID });
  const s = sessions.get(focusedSid);
  if (s) {
    sendSecure(ws, statusMsg(s));
    sendSecure(ws, hudMsg(s));
    if (s.pendingAsk) sendSecure(ws, askMsg(s));
    sendSecure(ws, feedMsg(s));   // after the ask (see pushFocused)
  } else {
    sendSecure(ws, { type: 'status', sid: null, state: 'idle', text: 'idle', tool: null });
  }
}
const wss = new WebSocketServer({ server });
wss.on('connection', (ws, req) => {
  clients.add(ws);
  ws._deviceId = null; ws._deviceName = null; ws._authenticated = false;
  ws._pairing = null; ws._authPending = null;
  ws._sessionKeys = null; ws._counters = { send: 0, recv: 0 };
  console.log(`[ws] connection (${clients.size} total) from ${req.socket.remoteAddress} — awaiting hello/auth`);
  ws.on('message', (buf, isBinary) => {
    if (isBinary) {   // mic PCM frames — encrypted once authenticated, unusable (dropped) before that
      if (!ws._authenticated) return;
      if (buf.length < 24) return;   // 8-byte counter + >=16-byte tag
      const n = Number(buf.readBigUInt64BE(0));
      if (n <= ws._counters.recv) { ws.close(4002, 'replay'); return; }
      let pcm;
      try { pcm = pcrypto.gcmDecrypt(ws._sessionKeys.recv, n, buf.subarray(8)); }
      catch { ws.close(4003, 'bad-auth-tag'); return; }
      ws._counters.recv = n;
      if (ws._recording) ws._mic.push(pcm);
      return;
    }
    let m; try { m = JSON.parse(buf.toString()); } catch { return; }

    if (m.type === 'sec') {
      if (!ws._authenticated) return;
      if (typeof m.n !== 'number' || m.n <= ws._counters.recv) { ws.close(4002, 'replay'); return; }
      let inner;
      try { inner = JSON.parse(pcrypto.gcmDecrypt(ws._sessionKeys.recv, m.n, Buffer.from(m.ct, 'base64')).toString('utf8')); }
      catch { ws.close(4003, 'bad-auth-tag'); return; }
      ws._counters.recv = m.n;
      m = inner;   // fall through to the existing dispatch below, exactly as if it had arrived in the clear
    } else if (!HANDSHAKE_TYPES.has(m.type)) {
      return;   // unauthenticated socket, not a handshake/keepalive type -> drop silently
    }

    switch (m.type) {
      case 'hello':
        ws._deviceId = typeof m.device_id === 'string' ? m.device_id : null;
        ws._deviceName = typeof m.name === 'string' ? m.name : null;
        console.log(`[ws] hello from ${m.name} (${m.fw}) device_id=${ws._deviceId || '?'}`);
        try {
          ws.send(JSON.stringify({
            type: 'hello_ack', bridge_id: BRIDGE_ID, host: SHORT_HOST, fw: BRIDGE_FW,
            paired: ws._deviceId ? trust.isPaired(ws._deviceId) : false,
          }));
        } catch {}
        break;
      case 'pair_request': handlePairRequest(ws, m); break;
      case 'pair_response': handlePairResponse(ws, m); break;
      case 'pair_confirm': handlePairConfirm(ws, m); break;
      case 'pair_reject': handlePairReject(ws, m); break;
      case 'pair_ack': handlePairAck(ws, m); break;
      case 'auth_hello': handleAuthHello(ws, m); break;
      case 'auth_verify': handleAuthVerify(ws, m); break;
      case 'voice_start': ws._mic = []; ws._recording = true; ws._voiceId = m.id; console.log('[voice] start'); break;
      case 'voice_end': finishVoice(ws); break;
      case 'voice_commit': {
        const pv = ws._pendingVoice;
        if (pv && pv.id === m.id) {
          ws._pendingVoice = null;
          const s = sessions.get(pv.sid);
          if (s) setSessionStatus(s, 'working', 'voice: ' + pv.text.slice(0, 32));
          injectToCC(pv.text, VOICE_AUTOSUBMIT, pv.tmux);
          console.log(`[voice] committed: "${pv.text}"`);
        }
        break;
      }
      case 'voice_cancel': {
        const pv = ws._pendingVoice;
        if (pv && pv.id === m.id) ws._pendingVoice = null;
        console.log('[voice] cancelled');
        break;
      }
      case 'focus': if (m.sid && sessions.has(m.sid)) setFocus(m.sid); break;
      case 'answer': {
        const p = pendingAsks.get(m.id);
        if (p) {
          clearTimeout(p.timer); pendingAsks.delete(m.id);
          console.log('[ask] answered:', JSON.stringify(m.answers));
          p.resolve(m.answers);
          const s = sessions.get(p.sid);
          if (s && s.pendingAsk && s.pendingAsk.id === m.id) { s.pendingAsk = null; broadcastSessions(); }
        }
        break;
      }
      case 'macro': {
        const mc = macros.find((x) => x.id === m.id);
        if (mc) { const s = sessions.get(focusedSid); console.log(`[macro] ${mc.label} -> ${s ? s.title : DEFAULT_TMUX}`); injectToCC(mc.prompt); }
        break;
      }
      case 'perm': console.log('[perm]', m.id, m.decision); break;
      case 'ping':
        // An authenticated peer sent its ping inside the envelope and drops plaintext replies —
        // answer in kind. Unauthenticated peers (handshake keepalive) still get plaintext.
        if (ws._authenticated) sendSecure(ws, { type: 'pong' });
        else try { ws.send('{"type":"pong"}'); } catch {}
        break;
      default: break;
    }
  });
  ws.on('close', () => {
    clients.delete(ws);
    ws._pendingVoice = null;
    if (ws._pairing?.timer) clearTimeout(ws._pairing.timer);
    if (ws._authPending?.timer) clearTimeout(ws._authPending.timer);
    console.log(`[ws] disconnected (${clients.size} left)`);
    if (clients.size === 0) {  // no deck left to answer -> unblock pending asks so Claude falls back to its TUI picker
      for (const [id, p] of pendingAsks) {
        clearTimeout(p.timer); pendingAsks.delete(id);
        const s = sessions.get(p.sid);
        if (s && s.pendingAsk && s.pendingAsk.id === id) s.pendingAsk = null;
        p.resolve(null);
      }
    }
  });
  ws.on('error', () => {});
});

// ---------- mDNS advertising: the deck auto-discovers every bridge on the LAN (_claudeq._tcp) ----------
// Chain: dns-sd (macOS, native) -> avahi-publish-service (Linux w/ avahi-utils) -> bonjour-service
// (vendored pure-JS, works anywhere). spawn() never throws on a missing binary — it emits an async
// 'error' — so success is only logged from the 'spawn' event, and each ENOENT advances the chain.
// The old version logged "advertising" unconditionally and swallowed both failures, so a Linux box
// without avahi silently never advertised while claiming it did.
let mdnsProc = null;
let mdnsBonjour = null;
function startMdns() {
  const inst = `Claudeq @ ${os.hostname().replace(/\.local$/, '')}`;
  const announce = (via) => console.log(`  mDNS: advertising "${inst}" as _claudeq._tcp on :${PORT} (via ${via})`);
  const tryBonjour = async () => {
    try {
      const { Bonjour } = await import('bonjour-service');
      mdnsBonjour = new Bonjour();
      mdnsBonjour.publish({ name: inst, type: 'claudeq', port: PORT });
      announce('bonjour-service');
    } catch (e) {
      console.error(`  mDNS: advertising UNAVAILABLE (${e.message}) — decks must be pointed at this host manually (Bridge address on the deck's setup screen)`);
    }
  };
  const tryCmd = (cmd, args, via, onFail) => {
    const p = spawn(cmd, args, { stdio: 'ignore' });
    p.on('spawn', () => announce(via));
    p.on('error', () => { mdnsProc = null; onFail(); });
    return p;
  };
  mdnsProc = tryCmd('dns-sd', ['-R', inst, '_claudeq._tcp', '.', String(PORT)], 'dns-sd', () => {
    mdnsProc = tryCmd('avahi-publish-service', [inst, '_claudeq._tcp', String(PORT)], 'avahi', () => { tryBonjour(); });
  });
}
function stopMdns() {
  if (mdnsProc) { try { mdnsProc.kill(); } catch {} mdnsProc = null; }
  if (mdnsBonjour) { try { mdnsBonjour.unpublishAll(); mdnsBonjour.destroy(); } catch {} mdnsBonjour = null; }
}
process.on('exit', stopMdns);
process.on('SIGINT', () => { stopMdns(); process.exit(0); });
process.on('SIGTERM', () => { stopMdns(); process.exit(0); });

server.listen(PORT, () => {
  console.log(`Claudeq bridge on http://localhost:${PORT}  (ws + mock at /)`);
  console.log(`  macros: ${macros.map((m) => m.id).join(', ')}`);
  startMdns();
});
