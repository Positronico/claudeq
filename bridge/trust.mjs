// Persistent bridge identity + paired-device trust store, backed by bridge/trust.json (same
// load-once/save-on-write pattern as macros.json). One entry per paired device, keyed by the
// device's persistent device_id; each entry holds the long-term PSK derived during pairing
// (see crypto.mjs's derivePsk) plus a display label and pairing timestamp.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomUUID } from 'node:crypto';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Mutable state lives OUTSIDE the install dir: under Homebrew, __dirname is the versioned keg, which
// is wiped on every `brew upgrade` — trust.json there means every paired deck is silently orphaned on
// upgrade (new bridgeId, empty device table). ~/.claudeq is the project's per-user state dir.
export const STATE_DIR = process.env.CLAUDEQ_STATE_DIR || path.join(os.homedir(), '.claudeq');
const TRUST_FILE = path.join(STATE_DIR, 'trust.json');
const LEGACY_TRUST_FILE = path.join(__dirname, 'trust.json');

function load() {
  // one-time migration from the pre-2.1.5 install-dir location
  if (!fs.existsSync(TRUST_FILE) && fs.existsSync(LEGACY_TRUST_FILE)) {
    try {
      fs.mkdirSync(STATE_DIR, { recursive: true });
      fs.copyFileSync(LEGACY_TRUST_FILE, TRUST_FILE);
      fs.chmodSync(TRUST_FILE, 0o600);
      console.log(`[trust] migrated ${LEGACY_TRUST_FILE} -> ${TRUST_FILE}`);
    } catch (e) { console.error(`[trust] migration failed: ${e.message}`); }
  }
  try {
    const raw = JSON.parse(fs.readFileSync(TRUST_FILE, 'utf8'));
    if (!raw.devices || typeof raw.devices !== 'object') raw.devices = {};
    return raw;
  } catch {
    return { version: 1, bridgeId: null, devices: {} };
  }
}

const state = load();
function save() {
  // atomic write (tmp + rename): a crash mid-write must never corrupt the store — load()'s catch would
  // silently drop every paired device and mint a fresh bridgeId. 0600: the file holds per-device PSKs.
  fs.mkdirSync(STATE_DIR, { recursive: true });
  const tmp = TRUST_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(state, null, 2), { mode: 0o600 });
  fs.renameSync(tmp, TRUST_FILE);
}

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
