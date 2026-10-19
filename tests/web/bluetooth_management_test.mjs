import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { webcrypto } from 'node:crypto';
import {
  MGMT_OPCODES, MGMT_MAX_SEGMENT, managementCrypt, managementEncryptionKey, managementFragments,
  managementFrame, managementIv, parseManagementReady, parseManagementSegment,
} from '../../bluetooth-app/ble-management.mjs';

// The same vectors the firmware codec is checked against.
const vectors = JSON.parse(readFileSync(new URL('../vectors/ost_ble_mgmt_v1.json', import.meta.url), 'utf8'));
const hex = (text) => Uint8Array.from(text.match(/../g).map((pair) => Number.parseInt(pair, 16)));
const toHex = (bytes) => Buffer.from(bytes).toString('hex');
const subtle = webcrypto.subtle;
const key = hex(vectors.session_key_hex);

test('request frame, encrypted secret and fragments match the published vectors', async () => {
  const wifi = vectors.wifi_configure;
  assert.equal(toHex(await managementEncryptionKey(subtle, key)), vectors.encryption_key_hex);
  assert.equal(toHex(managementIv(wifi.counter, wifi.request_id)), wifi.iv_hex);
  const password = new TextEncoder().encode(wifi.password);
  const ciphertext = await managementCrypt(subtle, key, wifi.counter, wifi.request_id, password);
  assert.equal(toHex(ciphertext), wifi.secret_ciphertext_hex);
  assert.deepEqual(await managementCrypt(subtle, key, wifi.counter, wifi.request_id, ciphertext), password);
  const frame = await managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.wifiConfigure, requestId: wifi.request_id, bootId: vectors.boot_id,
    counter: wifi.counter, body: new TextEncoder().encode(wifi.body), secret: password,
  });
  assert.equal(toHex(frame), wifi.frame_hex);
  assert.deepEqual(managementFragments(frame, 20).map(toHex), wifi.fragments_mtu23_hex);
  assert.deepEqual(managementFragments(frame, 244).map(toHex), wifi.fragments_mtu247_hex);
});

test('long responses verify per segment and reject tampering or the wrong counter', async () => {
  const status = vectors.wifi_status;
  const read = await managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.wifiStatus, requestId: status.request_id, bootId: vectors.boot_id,
    counter: status.counter, body: new TextEncoder().encode('{}'),
  });
  assert.equal(toHex(read), status.frame_hex);
  const ready = parseManagementReady(hex(status.ready_hex));
  assert.deepEqual(ready, { opcode: 0x40, requestId: status.request_id, status: 200, totalLength: status.body.length });
  const first = await parseManagementSegment(subtle, key, status.counter, hex(status.segment0_hex));
  assert.equal(first.offset, 0);
  assert.equal(first.body.length, MGMT_MAX_SEGMENT);
  const continueBody = new TextEncoder().encode(JSON.stringify({ request_id: status.request_id, offset: first.body.length }));
  const continueFrame = await managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.continueResponse, requestId: 0x1236, bootId: vectors.boot_id,
    counter: status.continue_counter, body: continueBody,
  });
  assert.equal(toHex(continueFrame), status.continue_frame_hex);
  const second = await parseManagementSegment(subtle, key, status.continue_counter, hex(status.segment1_hex));
  assert.equal(new TextDecoder().decode(new Uint8Array([...first.body, ...second.body])), status.body);
  await assert.rejects(parseManagementSegment(subtle, key, status.counter, hex(status.segment1_hex)), /verification/);
  const tampered = hex(status.segment0_hex);
  tampered[20] ^= 1;
  await assert.rejects(parseManagementSegment(subtle, key, status.counter, tampered), /verification/);
});

test('secrets are allowed only for password operations and requests stay within one frame', async () => {
  await assert.rejects(managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.deviceNameSet, requestId: 1, bootId: 1, counter: 1,
    body: new Uint8Array(), secret: new Uint8Array([1]),
  }), /malformed/);
  await assert.rejects(managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.otaChunk, requestId: 1, bootId: 1, counter: 1, body: new Uint8Array(4 + 480),
  }), /too large/);
  const chunk = await managementFrame(subtle, key, {
    opcode: MGMT_OPCODES.otaChunk, requestId: 1, bootId: 1, counter: 1, body: new Uint8Array(4 + 472),
  });
  assert.ok(chunk.length <= 512);
});
