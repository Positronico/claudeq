// Persistent bridge identity + paired-device trust store, backed by bridge/trust.json (same
// load-once/save-on-write pattern as macros.json). One entry per paired device, keyed by the
// device's persistent device_id; each entry holds the long-term PSK derived during pairing
// (see crypto.mjs's derivePsk) plus a display label and pairing timestamp.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomUUID } from 'node:crypto';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const TRUST_FILE = path.join(__dirname, 'trust.json');

function load() {
  try {
    const raw = JSON.parse(fs.readFileSync(TRUST_FILE, 'utf8'));
    if (!raw.devices || typeof raw.devices !== 'object') raw.devices = {};
    return raw;
  } catch {
    return { version: 1, bridgeId: null, devices: {} };
  }
}

const state = load();
function save() { fs.writeFileSync(TRUST_FILE, JSON.stringify(state, null, 2)); }

// BRIDGE_ID must survive process restarts for pairing to remain valid — previously this was
// randomUUID() every run (bridge.mjs originally), which silently orphaned every paired device
// on the next `claudeq restart-bridge`. CCDECK_BRIDGE_ID still overrides, for multi-instance testing.
if (!state.bridgeId) { state.bridgeId = randomUUID(); save(); }
export const BRIDGE_ID = process.env.CCDECK_BRIDGE_ID || state.bridgeId;

export function isPaired(deviceId) {
  return Object.prototype.hasOwnProperty.call(state.devices, deviceId);
}

export function getDevice(deviceId) {
  return state.devices[deviceId] || null;
}

export function pskFor(deviceId) {
  const d = state.devices[deviceId];
  return d ? Buffer.from(d.psk, 'base64') : null;
}

export function listDevices() {
  return Object.entries(state.devices).map(([deviceId, d]) => ({ deviceId, ...d }));
}

export function addDevice(deviceId, label, psk) {
  state.devices[deviceId] = { label: label || deviceId, psk: psk.toString('base64'), pairedAt: Date.now() };
  save();
}

export function forgetDevice(deviceId) {
  const existed = isPaired(deviceId);
  if (existed) { delete state.devices[deviceId]; save(); }
  return existed;
}
