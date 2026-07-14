// Claudeq pairing/auth crypto primitives — X25519 ECDH, HMAC-SHA256, AES-256-GCM, using only
// node:crypto (no npm deps). This is the reference implementation: the firmware (vendored x25519.c +
// mbedtls) and bridge/mock-device.html (WebCrypto SubtleCrypto) must derive byte-identical values from
// the same inputs. Every derivation here is a single HMAC-SHA256 call (no HKDF component available on
// the firmware side) — see docs/PROTOCOL.md's "Pairing" / "Authentication & encryption" sections for the
// full message flow this supports.
import crypto from 'node:crypto';

// Node's X25519 key objects only import/export DER (SPKI/PKCS8, RFC 8410) — not raw 32-byte keys, which
// is what goes over the wire (and what the firmware/mock use). These prefixes are fixed-length and
// constant for X25519 (OID 1.3.101.110), so raw <-> DER is just prefix concat/strip.
const SPKI_PREFIX = Buffer.from('302a300506032b656e032100', 'hex');
const PKCS8_PREFIX = Buffer.from('302e020100300506032b656e04220420', 'hex');

export function genKeyPair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('x25519');
  return { publicKey, privateKey, rawPublic: rawPub(publicKey) };
}

export function rawPub(pubKeyObject) {
  return pubKeyObject.export({ type: 'spki', format: 'der' }).subarray(SPKI_PREFIX.length);
}

export function pubKeyFromRaw(raw32) {
  if (raw32.length !== 32) throw new Error('x25519 public key must be 32 bytes');
  return crypto.createPublicKey({ key: Buffer.concat([SPKI_PREFIX, raw32]), format: 'der', type: 'spki' });
}

export function ecdh(privKeyObject, peerRawPub32) {
  return crypto.diffieHellman({ privateKey: privKeyObject, publicKey: pubKeyFromRaw(peerRawPub32) });
}

export function hmac(key, ...parts) {
  const h = crypto.createHmac('sha256', key);
  for (const p of parts) h.update(p);
  return h.digest();
}

// Canonical ordering so both peers hash the two ephemeral pairing pubkeys in the same order regardless
// of who sent pair_request vs pair_response. Byte-lexicographic compare — matches C's memcmp exactly.
export function sortPubs(a, b) {
  return Buffer.compare(a, b) <= 0 ? [a, b] : [b, a];
}

export function sas6(shared, pubA, pubB) {
  const [x, y] = sortPubs(pubA, pubB);
  const mac = hmac(shared, x, y);
  return String(mac.readUIntBE(0, 3) % 1000000).padStart(6, '0');
}

export function derivePsk(shared) {
  return hmac(shared, Buffer.from('claudeq-psk-v1', 'utf8'));
}

// Per-connection auth transcript: always device-nonce, then bridge-nonce, then the two fresh per-session
// ephemeral pubkeys in canonical (sorted) order. Fixed field order (not sorted) for the nonces since their
// roles are unambiguous (device always sends auth_hello first).
export function authTranscript(nonceDevice, nonceBridge, pubDeviceEph, pubBridgeEph) {
  const [x, y] = sortPubs(pubDeviceEph, pubBridgeEph);
  return Buffer.concat([nonceDevice, nonceBridge, x, y]);
}

export function deriveSessionKey(psk, ecdhShared, transcript) {
  return hmac(psk, ecdhShared, transcript, Buffer.from('claudeq-session-v1', 'utf8'));
}

export function directionalKeys(sessionKey) {
  return {
    d2b: hmac(sessionKey, Buffer.from('d2b', 'utf8')),
    b2d: hmac(sessionKey, Buffer.from('b2d', 'utf8')),
  };
}

export function authTag(sessionKey, role, transcript) {
  return hmac(sessionKey, Buffer.from(role, 'utf8'), transcript);
}

function counterIv(counter) {
  const iv = Buffer.alloc(12); // first 4 bytes stay zero; counter space never approaches 2^64
  iv.writeBigUInt64BE(BigInt(counter), 4);
  return iv;
}

export function gcmEncrypt(key, counter, plaintext) {
  const c = crypto.createCipheriv('aes-256-gcm', key, counterIv(counter));
  const ct = Buffer.concat([c.update(plaintext), c.final()]);
  return Buffer.concat([ct, c.getAuthTag()]);
}

// Throws on tag mismatch (tampered/replayed/wrong-key ciphertext) — callers must try/catch and treat any
// throw as a hard failure (close the connection), never a silent drop.
export function gcmDecrypt(key, counter, ctAndTag) {
  if (ctAndTag.length < 16) throw new Error('ciphertext too short');
  const tag = ctAndTag.subarray(ctAndTag.length - 16);
  const ct = ctAndTag.subarray(0, ctAndTag.length - 16);
  const d = crypto.createDecipheriv('aes-256-gcm', key, counterIv(counter));
  d.setAuthTag(tag);
  return Buffer.concat([d.update(ct), d.final()]);
}

export function randomBytes(n) {
  return crypto.randomBytes(n);
}

// Self-test: `node bridge/crypto.mjs` — round-trips every primitive above. Run before relying on this
// module from bridge.mjs; if this ever fails after a Node upgrade, the DER prefixes are the first thing
// to suspect (they're RFC-fixed but Node's key-object export could reject/reshape unexpected input).
if (import.meta.url === `file://${process.argv[1]}`) {
  const assert = (cond, msg) => { if (!cond) throw new Error('crypto self-test FAILED: ' + msg); };

  // X25519 raw <-> DER round trip + ECDH agreement, mirroring the pairing ceremony's two ephemeral parties.
  const device = genKeyPair();
  const bridge = genKeyPair();
  const sharedD = ecdh(device.privateKey, bridge.rawPublic);
  const sharedB = ecdh(bridge.privateKey, device.rawPublic);
  assert(sharedD.equals(sharedB), 'ECDH shared secrets differ between the two sides');
  assert(device.rawPublic.length === 32 && bridge.rawPublic.length === 32, 'raw pubkeys must be 32 bytes');

  // SAS must match regardless of which side's pubkey is passed first (canonical sort).
  const sasA = sas6(sharedD, device.rawPublic, bridge.rawPublic);
  const sasB = sas6(sharedB, bridge.rawPublic, device.rawPublic);
  assert(sasA === sasB, `SAS mismatch: ${sasA} vs ${sasB}`);
  assert(/^\d{6}$/.test(sasA), `SAS not 6 digits: ${sasA}`);

  // PSK -> session key -> directional keys, from both sides independently, must all agree.
  const pskD = derivePsk(sharedD), pskB = derivePsk(sharedB);
  assert(pskD.equals(pskB), 'PSK mismatch');
  const eph1 = genKeyPair(), eph2 = genKeyPair();
  const ecdhSession = ecdh(eph1.privateKey, eph2.rawPublic);
  const nonceD = randomBytes(16), nonceB = randomBytes(16);
  const transcript = authTranscript(nonceD, nonceB, eph1.rawPublic, eph2.rawPublic);
  const sk1 = deriveSessionKey(pskD, ecdhSession, transcript);
  const sk2 = deriveSessionKey(pskB, ecdh(eph2.privateKey, eph1.rawPublic), transcript);
  assert(sk1.equals(sk2), 'session key mismatch');
  const dirs1 = directionalKeys(sk1), dirs2 = directionalKeys(sk2);
  assert(dirs1.d2b.equals(dirs2.d2b) && dirs1.b2d.equals(dirs2.b2d), 'directional key mismatch');

  const tagDevice = authTag(sk1, 'device', transcript);
  const tagBridge = authTag(sk2, 'bridge', transcript);
  assert(!tagDevice.equals(tagBridge), 'device/bridge auth tags must differ (different role string)');
  assert(authTag(sk2, 'device', transcript).equals(tagDevice), 'auth tag not reproducible cross-side');

  // AES-256-GCM round trip + tamper detection.
  const pt = Buffer.from(JSON.stringify({ type: 'sessions', list: [] }), 'utf8');
  const ct = gcmEncrypt(dirs1.d2b, 1, pt);
  const rt = gcmDecrypt(dirs2.d2b, 1, ct);
  assert(rt.equals(pt), 'GCM round-trip mismatch');
  let tamperedOk = false;
  try {
    const tampered = Buffer.from(ct); tampered[0] ^= 0xff;
    gcmDecrypt(dirs2.d2b, 1, tampered);
    tamperedOk = true;
  } catch { /* expected */ }
  assert(!tamperedOk, 'tampered ciphertext must fail to decrypt');
  let wrongCounterOk = false;
  try { gcmDecrypt(dirs2.d2b, 2, ct); wrongCounterOk = true; } catch { /* expected */ }
  assert(!wrongCounterOk, 'decrypting with the wrong counter/IV must fail');

  console.log('crypto.mjs self-test: all checks passed');
  console.log('  sample SAS:', sasA);
}
