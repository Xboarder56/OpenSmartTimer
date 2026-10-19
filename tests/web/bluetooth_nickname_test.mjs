import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
const functionSource = source.slice(source.indexOf('async function applyPairingNickname()'),
  source.indexOf('async function finishPairing()'));
function fixture({name = '我的手机', present = true, flags = 2, version = 1, failWrite = false, mismatch = false} = {}) {
  let reads = 0;
  const writes = [];
  const context = vm.createContext({TextEncoder, TextDecoder,
    app: {chars: present ? {pairingNickname: 'nickname'} : {}},
    $: () => ({value: name}), bytesOf: value => value,
    readGatt: async () => {
      reads++;
      return Uint8Array.from([version, flags, ...new TextEncoder().encode(
        reads === 1 ? 'Device 1' : mismatch ? 'Different' : name.trim())]);
    },
    writeGatt: async (characteristic, bytes) => {
      writes.push(new TextDecoder().decode(bytes));
      if (failWrite) throw Error('write rejected');
    },
  });
  vm.runInContext(functionSource, context);
  return {run: () => context.applyPairingNickname(), writes, reads: () => reads};
}

test('headless nickname uses encrypted extension, UTF-8 and verified readback', async () => {
  const f = fixture(); await f.run();
  assert.deepEqual(f.writes, ['我的手机']); assert.equal(f.reads(), 2);
  const max = fixture({name: '中'.repeat(8)}); await max.run(); assert.equal(max.writes.length, 1);
});
test('optional extension and owner-selected nickname leave names unchanged', async () => {
  const old = fixture({present: false}); await old.run(); assert.equal(old.reads(), 0);
  const owner = fixture({flags: 3}); await owner.run(); assert.equal(owner.writes.length, 0);
});
test('invalid nickname or closed/malformed pairing window never writes', async () => {
  for (const options of [{name: ''}, {name: '中'.repeat(9)}, {name: 'bad\0name'},
    {name: 'bad\u007fname'}, {flags: 0}, {version: 2}]) {
    const f = fixture(options); await assert.rejects(f.run()); assert.equal(f.writes.length, 0);
  }
});
test('nickname write rejection and mismatched readback prevent pairing commit', async () => {
  for (const options of [{failWrite: true}, {mismatch: true}])
    await assert.rejects(fixture(options).run());
  const pairing = source.slice(source.indexOf('async function finishPairing()'), source.indexOf('function buildCommand'));
  assert.ok(pairing.indexOf('await applyPairingNickname()') < pairing.indexOf("credentialWrite('pending', secret)"));
});
