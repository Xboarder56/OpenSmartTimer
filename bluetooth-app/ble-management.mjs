// ost-ble/1 management extension (docs/BLUETOOTH_MANAGEMENT.md). Requests carry
// the same JSON as the HTTP API and replies carry the same JSON body, produced
// by the same firmware handler. Pure functions: the caller supplies WebCrypto.

export const MGMT_VERSION = 1;
export const MGMT_MAX_FRAME = 512;
export const MGMT_HEADER = 15;
export const MGMT_MAC = 16;
export const MGMT_MAX_SECRET = 64;
export const MGMT_RESPONSE_HEADER = 12;
export const MGMT_MAX_SEGMENT = MGMT_MAX_FRAME - MGMT_RESPONSE_HEADER - MGMT_MAC;
export const MGMT_FIRMWARE_CHUNK = 472;
export const MGMT_ATT_BUSY = 0x80;
export const MGMT_ATT_FRAGMENT = 0x81;

const encoder = new TextEncoder();
const REQUEST_LABEL = encoder.encode('OST-MGMT-v1');
const RESPONSE_LABEL = encoder.encode('OST-MGMT-RSP-v1');
const ENCRYPTION_LABEL = encoder.encode('OST-MGMT-ENC-v1');

export const MGMT_OPCODES = Object.freeze({
  continueResponse: 0x3f,
  wifiStatus: 0x40, accessPoint: 0x41, deviceName: 0x42, bluetooth: 0x43,
  diagnostics: 0x44, events: 0x45, capabilities: 0x46, status: 0x47,
  wifiConfigure: 0x50, wifiForget: 0x51, accessPointSet: 0x52, deviceNameSet: 0x53,
  restart: 0x54, appLabel: 0x55, appRevoke: 0x56, pairingStart: 0x57,
  pairingCancel: 0x58, pairingRevoke: 0x59, bluetoothOff: 0x5a,
  wifiSuspend: 0x5b, wifiResume: 0x5c,
  wifiScanStart: 0x60, wifiScanResults: 0x61,
  otaBegin: 0x70, otaChunk: 0x71, otaFinish: 0x72, otaAbort: 0x73,
});

const SECRET_OPCODES = new Set([MGMT_OPCODES.wifiConfigure, MGMT_OPCODES.accessPointSet]);
export const carriesSecret = (opcode) => SECRET_OPCODES.has(opcode);

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((size, part) => size + part.length, 0));
  let offset = 0;
  for (const part of parts) { out.set(part, offset); offset += part.length; }
  return out;
}

async function hmac(subtle, keyBytes, ...parts) {
  const key = await subtle.importKey('raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return new Uint8Array(await subtle.sign('HMAC', key, concat(...parts)));
}

export async function managementEncryptionKey(subtle, sessionKey) {
  return (await hmac(subtle, sessionKey, ENCRYPTION_LABEL)).slice(0, 16);
}

export function managementIv(counter, requestId) {
  const iv = new Uint8Array(16);
  const view = new DataView(iv.buffer);
  view.setUint32(0, counter, true);
  view.setUint16(4, requestId, true);
  return iv;
}

// AES-128-CTR; encryption and decryption are the same operation.
export async function managementCrypt(subtle, sessionKey, counter, requestId, data) {
  const raw = await managementEncryptionKey(subtle, sessionKey);
  const key = await subtle.importKey('raw', raw, { name: 'AES-CTR' }, false, ['encrypt']);
  return new Uint8Array(await subtle.encrypt(
    { name: 'AES-CTR', counter: managementIv(counter, requestId), length: 64 }, key, data));
}

export async function managementFrame(subtle, sessionKey, { opcode, requestId, bootId, counter, body, secret = new Uint8Array() }) {
  if (!Number.isInteger(requestId) || requestId < 1 || requestId > 0xffff || !counter ||
      secret.length > MGMT_MAX_SECRET || (secret.length && !carriesSecret(opcode)) ||
      MGMT_HEADER + body.length + secret.length + MGMT_MAC > MGMT_MAX_FRAME) {
    throw new Error('The Bluetooth settings request is too large or malformed');
  }
  const ciphertext = secret.length ? await managementCrypt(subtle, sessionKey, counter, requestId, secret) : new Uint8Array();
  const header = new Uint8Array(MGMT_HEADER);
  const view = new DataView(header.buffer);
  header[0] = MGMT_VERSION;
  header[1] = opcode;
  view.setUint16(2, requestId, true);
  view.setUint32(4, bootId >>> 0, true);
  view.setUint32(8, counter >>> 0, true);
  view.setUint16(12, body.length, true);
  header[14] = ciphertext.length;
  const signed = concat(header, body, ciphertext);
  return concat(signed, (await hmac(subtle, sessionKey, REQUEST_LABEL, signed)).slice(0, MGMT_MAC));
}

// Writes of at most `maxWrite` bytes: index, last-flag, then frame bytes.
export function managementFragments(frame, maxWrite) {
  const payload = Math.max(1, maxWrite - 2);
  const pieces = [];
  for (let offset = 0; offset < frame.length; offset += payload) pieces.push(frame.slice(offset, offset + payload));
  return pieces.map((piece, index) => concat(Uint8Array.of(index, index === pieces.length - 1 ? 1 : 0), piece));
}

export function parseManagementReady(bytes) {
  if (bytes.length !== 8 || bytes[0] !== MGMT_VERSION) throw new Error('Invalid Bluetooth settings notification');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return { opcode: bytes[1], requestId: view.getUint16(2, true), status: view.getUint16(4, true), totalLength: view.getUint16(6, true) };
}

// Verifies the timer's MAC, bound to the counter of the request being answered.
export async function parseManagementSegment(subtle, sessionKey, counter, bytes) {
  if (bytes.length < MGMT_RESPONSE_HEADER + MGMT_MAC || bytes[0] !== MGMT_VERSION) throw new Error('Invalid Bluetooth settings response');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(10, true);
  if (bytes.length !== MGMT_RESPONSE_HEADER + length + MGMT_MAC) throw new Error('Invalid Bluetooth settings response length');
  const counterBytes = new Uint8Array(4);
  new DataView(counterBytes.buffer).setUint32(0, counter >>> 0, true);
  const signed = bytes.slice(0, MGMT_RESPONSE_HEADER + length);
  const expected = (await hmac(subtle, sessionKey, RESPONSE_LABEL, counterBytes, signed)).slice(0, MGMT_MAC);
  const actual = bytes.slice(MGMT_RESPONSE_HEADER + length);
  let difference = 0;
  for (let index = 0; index < MGMT_MAC; index += 1) difference |= expected[index] ^ actual[index];
  if (difference !== 0) throw new Error('The Bluetooth settings response failed verification');
  return {
    opcode: bytes[1], requestId: view.getUint16(2, true), status: view.getUint16(4, true),
    totalLength: view.getUint16(6, true), offset: view.getUint16(8, true),
    body: bytes.slice(MGMT_RESPONSE_HEADER, MGMT_RESPONSE_HEADER + length),
  };
}
