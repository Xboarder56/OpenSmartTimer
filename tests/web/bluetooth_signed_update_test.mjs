import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {File} from 'node:buffer';
import test from 'node:test';
import vm from 'node:vm';
import {MGMT_OPCODES} from '../../bluetooth-app/ble-management.mjs';

const source = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
function fixture({armed = true, accept = true} = {}) {
  const nodes = new Map();
  const get = id => {
    if (!nodes.has(id)) nodes.set(id, {checked: false, classList: {add() {}, remove() {}}});
    return nodes.get(id);
  };
  const requests = [], chunks = [];
  const context = vm.createContext({$: get, Uint8Array, DataView, TextEncoder, TextDecoder,
    MGMT_OPCODES, MGMT_FIRMWARE_CHUNK: 472, app: {server: {connected: true}},
    t: s => s, managementMessage: () => {}, renderUpdateStatus: () => {},
    confirmManagement: async () => accept, runManagement: async (_, work) => work(),
    readManagement: async opcode => { assert.equal(opcode, MGMT_OPCODES.status); return {ota_armed: armed, ota_unverified_allowed: true}; },
    manageJson: async (opcode, body) => {requests.push({opcode, body}); return {status: 202, json: {chunk_bytes: 472}};},
    managementExchange: async (opcode, body) => {
      if (opcode === MGMT_OPCODES.otaChunk) chunks.push(new Uint8Array(body));
      else assert.equal(opcode, MGMT_OPCODES.otaFinish);
      return {status: 200, body: new TextEncoder().encode('{"updated":true}')};
    },
    managementError: (_, code) => `error ${code}`,
  });
  for (const name of ['crc32Hex', 'installFirmwareOverBluetooth']) {
    const pattern = new RegExp(`^(?:async )?function ${name}\\([^\\n]*\\) \\{[\\s\\S]*?^\\}`, 'm');
    vm.runInContext(source.match(pattern)[0], context);
  }
  return {context, requests, chunks};
}

test('actual Bluetooth installer forwards signed descriptor, then sends exact chunk offsets and image', async () => {
  const f = fixture();
  const image = Uint8Array.from({length: 1000}, (_, i) => i === 0 ? 0xe9 : i & 255);
  const file = new File([image], 'signed-fixture.bin');
  const metadata = {image_size: image.length, image_crc32: f.context.crc32Hex(image),
    release_version: '1.0.0-rc.1', image_sha256: '1'.repeat(64), release_signature: '2'.repeat(128)};
  await f.context.installFirmwareOverBluetooth(file, metadata);
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0].opcode, MGMT_OPCODES.otaBegin);
  assert.deepEqual({...f.requests[0].body}, metadata);
  let offset = 0;
  for (const chunk of f.chunks) {
    assert.equal(new DataView(chunk.buffer).getUint32(0, true), offset);
    assert.deepEqual(chunk.slice(4), image.slice(offset, offset + chunk.length - 4));
    offset += chunk.length - 4;
  }
  assert.equal(offset, image.length);
});

test('unarmed or cancelled signed Bluetooth installation sends no OTA begin or chunks', async () => {
  const file = new File([Uint8Array.from({length: 24}, (_, i) => i ? 0 : 0xe9)], 'signed-fixture.bin');
  for (const options of [{armed: false}, {accept: false}]) {
    const f = fixture(options);
    if (options.armed === false) await assert.rejects(f.context.installFirmwareOverBluetooth(file, {release_signature: '2'.repeat(128)}), /Briefly press Reset/);
    else await f.context.installFirmwareOverBluetooth(file, {release_signature: '2'.repeat(128)});
    assert.equal(f.requests.length, 0); assert.equal(f.chunks.length, 0);
  }
});
