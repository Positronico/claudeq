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
import { randomUUID } from 'node:crypto';
import { WebSocketServer } from 'ws';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.env.CCDECK_PORT || '8787', 10);
const SHORT_HOST = (process.env.CCDECK_HOST || os.hostname() || 'host').replace(/\.local$/, '').split('.')[0].slice(0, 16);  // for the deck to label chips on title clashes; override with CCDECK_HOST
// Stable per-process id so the deck can tell that this bridge — reached via the LAN and via the tailnet
// at two different IPs — is ONE machine, and show its sessions once. Override with CCDECK_BRIDGE_ID.
const BRIDGE_ID = process.env.CCDECK_BRIDGE_ID || randomUUID();
const DEFAULT_TMUX = process.env.CCDECK_TMUX_TARGET || 'ccdeck';   // fallback for legacy `cc`
const ASK_TIMEOUT_MS = parseInt(process.env.CCDECK_ASK_TIMEOUT_MS || '280000', 10);
const PRUNE_GRACE_MS = parseInt(process.env.CCDECK_PRUNE_GRACE_MS || '20000', 10);
const MACROS_FILE = path.join(__dirname, 'macros.json');
const MOCK_FILE = path.join(__dirname, 'mock-device.html');
const SOUNDS_DIR = path.join(__dirname, 'sounds');
const SOUND_SR = 16000;
const SOUND_EXTS = ['wav', 'mp3', 'm4a', 'ogg', 'flac', 'aiff', 'aif'];
const soundCache = {}; // name -> { mtime, pcm }

// Find sounds/<name>.<ext>, convert to 16k mono s16le PCM via ffmpeg, cache by mtime.
// Drop any audio file at bridge/sounds/alert.* or done.* to customize — picked up automatically.
function loadSound(name) {
  let file = null;
  for (const e of SOUND_EXTS) { const p = path.join(SOUNDS_DIR, `${name}.${e}`); if (fs.existsSync(p)) { file = p; break; } }
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
  for (const ws of clients) { try { ws.send(pcm, { binary: true }); } catch {} }
}

// ---------- state ----------
const clients = new Set();
const sessions = new Map();          // sid -> session object
const pendingAsks = new Map();        // ask id -> { resolve, timer, sid }
let focusedSid = null;
let sessionSeq = 0;
let macros = loadMacros();

function newSession(sid) {
  return {
    sid, seq: ++sessionSeq, tmux: DEFAULT_TMUX, title: 'session', cwd: null,
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
  if (isNew) { if (!focusedSid) focusedSid = sid; broadcastSessions(); }
  return s;
}

function loadMacros() {
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
  const s = JSON.stringify(obj);
  for (const ws of clients) { try { ws.send(s); } catch {} }
}
function needsAttn(s) { return !!s.pendingAsk || s.attn; }
function sessionsList() {
  return [...sessions.values()].sort((a, b) => a.seq - b.seq)
    .map((s) => ({ sid: s.sid, title: s.title, needs: needsAttn(s) }));
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
function clip(v, n = 52) { const s = String(v || '').replace(/\s+/g, ' ').trim(); return s.length > n ? s.slice(0, n - 1) + '…' : s; }
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
      if (text) return { text, id: o.uuid || m.id || String(i) };  // id = this turn's message identity
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
    if (s) { s.lastReplyId = r.id; pushFeed(s, 'reply', '', clip(r.text, 200)); console.log(`[reply] ${r.text.length} chars -> ${s.title}`); }
  } else if (tries > 0) {
    setTimeout(() => pushReplyWhenReady(sid, transcriptPath, tries - 1, prevId), 200);
  }
}
// Append a line to a session's feed; stream it to the deck if that session is focused.
function pushFeed(s, kind, label, detail = '') {
  const line = { ts: Date.now(), kind, label, detail };
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
  s.state = state; s.text = text ?? state; s.tool = tool ?? s.tool;
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
const WHISPER_MODEL = process.env.CCDECK_WHISPER_MODEL || path.join(__dirname, '..', 'tools', 'whisper-models', 'ggml-base.en.bin');
const VOICE_AUTOSUBMIT = process.env.CCDECK_VOICE_AUTOSUBMIT !== '0';   // type+Enter by default; set 0 to type only
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
  const r = spawnSync('/bin/sh', ['-c', `"${WHISPER_BIN}" -m "${WHISPER_MODEL}" -f "${wavPath}" -nt -np 2>/dev/null`], { maxBuffer: 16 * 1024 * 1024 });
  if (r.status !== 0) { console.error('[voice] whisper failed:', (r.stderr || '').toString().slice(0, 200)); return ''; }
  return (r.stdout || '').toString().replace(/\s+/g, ' ').trim();
}
function finishVoice(ws) {
  const chunks = ws._mic || []; ws._mic = []; ws._recording = false;
  const pcm = Buffer.concat(chunks);
  console.log(`[voice] ${pcm.length} bytes (~${(pcm.length / 2 / 16000).toFixed(1)}s)`);
  if (pcm.length < 16000 * 2 * 0.3) { console.log('[voice] too short, ignored'); return; }
  const tmp = path.join(os.tmpdir(), `ccdeck-voice-${process.pid}-${++voiceSeq}.wav`);
  try {
    fs.writeFileSync(tmp, pcmToWav(pcm));
    const text = transcribeWav(tmp);
    fs.unlink(tmp, () => {});
    const s = sessions.get(focusedSid);
    console.log(`[voice] -> ${s ? s.title : '?'}: "${text}"`);
    if (text) { if (s) setSessionStatus(s, 'working', 'voice: ' + text.slice(0, 32)); injectToCC(text, VOICE_AUTOSUBMIT); }
  } catch (e) { console.error('[voice] error', e.message); }
}

// ---------- http ----------
function readBody(req) {
  return new Promise((resolve) => {
    let b = ''; req.on('data', (c) => (b += c)); req.on('end', () => resolve(b));
  });
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, `http://localhost:${PORT}`);

  if (req.method === 'GET' && (u.pathname === '/' || u.pathname === '/mock' || u.pathname === '/mock-device.html')) {
    fs.readFile(MOCK_FILE, (e, data) => {
      if (e) { res.writeHead(404); res.end('mock not found'); return; }
      res.writeHead(200, { 'content-type': 'text/html' }); res.end(data);
    });
    return;
  }
  if (req.method === 'GET' && u.pathname === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: true, clients: clients.size, sessions: sessionsList(), focus: focusedSid })); return;
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

  res.writeHead(404); res.end('not found');
});

function handleEvent(kind, ev, meta) {
  const s = touchSession(meta);
  if (!s) return;                       // no session id -> can't attribute; ignore
  switch (kind) {
    case 'SessionStart':
      if (ev.model) s.hud.model = ev.model;
      s.attn = false; setSessionStatus(s, 'idle', 'ready');
      pushFeed(s, 'start', 'session', shortFile(s.cwd)); break;
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
      broadcast({ type: 'alert', sid: s.sid, level: 'attn', text: ev.message || 'Claude needs you', sound: 'chirp' });
      sendSound('alert'); broadcastSessions(); break;
    case 'Stop':
      if (s.promptStartedAt) s.hud.elapsedMs = Date.now() - s.promptStartedAt;
      s.attn = false; setSessionStatus(s, 'done', 'done');
      pushFeed(s, 'done', 'done', s.hud.elapsedMs ? Math.round(s.hud.elapsedMs / 1000) + 's' : '');
      if (ev.transcript_path) pushReplyWhenReady(s.sid, ev.transcript_path, 12, s.lastReplyId || '');
      broadcast({ type: 'alert', sid: s.sid, level: 'info', text: 'done', sound: 'soft' });
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
  let changed = false;
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

// ---------- websocket ----------
function sendSnapshot(ws) {
  ws.send(JSON.stringify({ type: 'macros', items: macros }));
  ws.send(JSON.stringify({ type: 'sessions', list: sessionsList(), focus: focusedSid, host: SHORT_HOST, bridge_id: BRIDGE_ID }));
  const s = sessions.get(focusedSid);
  if (s) {
    ws.send(JSON.stringify(statusMsg(s)));
    ws.send(JSON.stringify(hudMsg(s)));
    if (s.pendingAsk) ws.send(JSON.stringify(askMsg(s)));
    ws.send(JSON.stringify(feedMsg(s)));   // after the ask (see pushFocused)
  } else {
    ws.send(JSON.stringify({ type: 'status', sid: null, state: 'idle', text: 'idle', tool: null }));
  }
}
const wss = new WebSocketServer({ server });
wss.on('connection', (ws, req) => {
  clients.add(ws);
  console.log(`[ws] deck connected (${clients.size} total) from ${req.socket.remoteAddress}`);
  sendSnapshot(ws);
  ws.on('message', (buf, isBinary) => {
    if (isBinary) { if (ws._recording) ws._mic.push(buf); return; }   // mic PCM frames
    let m; try { m = JSON.parse(buf.toString()); } catch { return; }
    switch (m.type) {
      case 'hello': console.log(`[ws] hello from ${m.name} (${m.fw})`); sendSound('done'); break;
      case 'voice_start': ws._mic = []; ws._recording = true; console.log('[voice] start'); break;
      case 'voice_end': finishVoice(ws); break;
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
      case 'ping': try { ws.send('{"type":"pong"}'); } catch {} break;
      default: break;
    }
  });
  ws.on('close', () => {
    clients.delete(ws);
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
let mdnsProc = null;
function startMdns() {
  const inst = `Claudeq @ ${os.hostname().replace(/\.local$/, '')}`;
  const tryCmd = (cmd, args) => { try { const p = spawn(cmd, args, { stdio: 'ignore' }); return p; } catch { return null; } };
  // macOS (Bonjour) first; fall back to Linux (Avahi) if dns-sd is missing
  mdnsProc = tryCmd('dns-sd', ['-R', inst, '_claudeq._tcp', '.', String(PORT)]);
  if (mdnsProc) {
    mdnsProc.on('error', () => { mdnsProc = tryCmd('avahi-publish-service', [inst, '_claudeq._tcp', String(PORT)]); if (mdnsProc) mdnsProc.on('error', () => {}); });
    console.log(`  mDNS: advertising "${inst}" as _claudeq._tcp on :${PORT}`);
  }
}
function stopMdns() { if (mdnsProc) { try { mdnsProc.kill(); } catch {} mdnsProc = null; } }
process.on('exit', stopMdns);
process.on('SIGINT', () => { stopMdns(); process.exit(0); });
process.on('SIGTERM', () => { stopMdns(); process.exit(0); });

server.listen(PORT, () => {
  console.log(`Claudeq bridge on http://localhost:${PORT}  (ws + mock at /)`);
  console.log(`  macros: ${macros.map((m) => m.id).join(', ')}`);
  startMdns();
});
