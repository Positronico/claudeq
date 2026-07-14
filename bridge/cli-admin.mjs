#!/usr/bin/env node
// claudeq pair / claudeq devices ... — a thin CLI over the bridge's loopback-only /admin/* endpoints
// (see bridge/bridge.mjs's "pairing / authentication" section and docs/PROTOCOL.md's "Pairing" section).
// Kept as a small Node script (not inline bash+jq) since Node is already this project's one hard
// dependency — no new tool to install just to pair a device.
import readline from 'node:readline/promises';
import { stdin, stdout } from 'node:process';

const PORT = process.env.CCDECK_PORT || '8787';
const BASE = `http://127.0.0.1:${PORT}`;

async function api(path, opts = {}) {
  const res = await fetch(BASE + path, {
    method: opts.method || 'GET',
    headers: opts.body ? { 'content-type': 'application/json' } : undefined,
    body: opts.body ? JSON.stringify(opts.body) : undefined,
  });
  if (!res.ok && res.status !== 404) throw new Error(`${path}: HTTP ${res.status}`);
  return res.json();
}

async function ask(rl, prompt) {
  return (await rl.question(prompt)).trim();
}

async function cmdPair() {
  const pending = await api('/admin/devices/pending');
  if (!pending.length) {
    console.log('No unpaired devices are currently connected.');
    console.log('On the deck: Settings → Paired Bridges → Pair new bridge, then run this again.');
    return;
  }
  let target = pending[0];
  if (pending.length > 1) {
    console.log('Multiple unpaired devices are connected:');
    pending.forEach((d, i) => console.log(`  ${i + 1}) ${d.name || d.deviceId} (${d.deviceId})`));
    const rl = readline.createInterface({ input: stdin, output: stdout });
    const idx = parseInt(await ask(rl, 'Pick one: '), 10);
    rl.close();
    target = pending[idx - 1];
    if (!target) { console.error('Invalid choice.'); process.exitCode = 1; return; }
  }
  console.log(`Pairing with ${target.name || target.deviceId}...`);
  let code = target.pairing?.code;
  if (!code) {
    process.stdout.write('Waiting for the device to respond... ');
    const r = await api('/admin/pair/start', { method: 'POST', body: { deviceId: target.deviceId } });
    if (!r.ok) { console.log(`failed (${r.reason || 'unknown'}).`); process.exitCode = 1; return; }
    code = r.code;
    console.log('ok.');
  }
  console.log(`\nConfirm this code matches the device screen: ${code}\n`);
  const rl = readline.createInterface({ input: stdin, output: stdout });
  const answer = (await ask(rl, 'Codes match? [y/N] ')).toLowerCase();
  rl.close();
  if (answer !== 'y' && answer !== 'yes') {
    await api('/admin/pair/reject', { method: 'POST', body: { deviceId: target.deviceId, reason: 'user' } });
    console.log('Pairing cancelled.');
    return;
  }
  process.stdout.write('Waiting for the device to confirm... ');
  const r = await api('/admin/pair/confirm', { method: 'POST', body: { deviceId: target.deviceId } });
  console.log(r.paired ? 'paired.' : 'not confirmed (timed out, or rejected on the device).');
  if (!r.paired) process.exitCode = 1;
}

async function cmdDevicesList() {
  const devices = await api('/admin/devices');
  if (!devices.length) { console.log('No paired devices.'); return; }
  const pad = (s, n) => String(s).padEnd(n);
  console.log(pad('DEVICE ID', 38) + pad('LABEL', 20) + pad('STATUS', 14) + 'PAIRED AT');
  for (const d of devices) {
    const status = d.authenticated ? 'connected' : (d.connected ? 'connecting' : 'offline');
    console.log(pad(d.deviceId, 38) + pad(d.label, 20) + pad(status, 14) + new Date(d.pairedAt).toLocaleString());
  }
}

async function cmdDevicesAction(action, id) {
  if (!id) { console.error(`usage: claudeq devices ${action} <device-id>`); process.exitCode = 1; return; }
  const r = await api(`/admin/devices/${encodeURIComponent(id)}/${action}`, { method: 'POST', body: {} });
  if (action === 'disconnect') console.log(r.disconnected ? 'Disconnected.' : 'Device was not connected.');
  else console.log(r.existed ? 'Forgotten — the device must be re-paired.' : 'No such paired device.');
}

const [, , cmd, sub, ...rest] = process.argv;
try {
  if (cmd === 'pair') {
    await cmdPair();
  } else if (cmd === 'devices') {
    if (!sub || sub === 'list') await cmdDevicesList();
    else if (sub === 'disconnect' || sub === 'forget') await cmdDevicesAction(sub, rest[0]);
    else { console.error(`unknown: claudeq devices ${sub}`); process.exitCode = 1; }
  } else {
    console.error(`unknown command: ${cmd}`);
    process.exitCode = 1;
  }
} catch (e) {
  console.error(`claudeq: ${e.message}`);
  console.error('Is the bridge running? Try: claudeq restart-bridge');
  process.exitCode = 1;
}
