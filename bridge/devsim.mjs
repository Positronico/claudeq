#!/usr/bin/env node
// Headless device simulator: stands in for the ESP32 to test the bridge end-to-end.
// Connects, logs everything it receives, and auto-answers any `ask` with option index $PICK.
import WebSocket from 'ws';
const URL = process.env.URL || 'ws://127.0.0.1:8787';
const PICK = process.env.PICK ?? '1'; // index (e.g. "1") or exact label
let ws;
function connect() {
  ws = new WebSocket(URL);
  ws.on('open', () => { console.log('[sim] connected ->', URL); ws.send(JSON.stringify({ type: 'hello', name: 'devsim', fw: 'sim-0.1', caps: ['decide','macros','hud'] })); });
  ws.on('message', (d) => {
    let m; try { m = JSON.parse(d.toString()); } catch { return; }
    console.log('[sim] recv:', JSON.stringify(m));
    if (m.type === 'ask') {
      const q = m.questions[0];
      let idx = parseInt(PICK, 10);
      if (Number.isNaN(idx)) idx = q.options.findIndex((o) => o.label === PICK);
      if (idx < 0 || idx >= q.options.length) idx = 0;
      const answers = {}; answers[q.question] = q.options[idx].label;
      setTimeout(() => { console.log('[sim] answering:', JSON.stringify(answers)); ws.send(JSON.stringify({ type: 'answer', id: m.id, answers })); }, 700);
    }
  });
  ws.on('close', () => { console.log('[sim] closed; reconnect in 1s'); setTimeout(connect, 1000); });
  ws.on('error', (e) => console.error('[sim] error:', e.message));
}
connect();
