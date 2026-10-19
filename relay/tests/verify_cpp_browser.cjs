// Parser interop for C++ responses already authenticated/decrypted by Python.
// verify_secure_cpp.cjs separately exercises actual WebCrypto authentication.
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
(async () => {
  const fixture = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  const context = vm.createContext({setInterval:()=>1,clearInterval(){},TextEncoder,TextDecoder,Uint8Array,DataView,
    atob:value=>Buffer.from(value,'base64').toString('binary'),document:{addEventListener(){}},
    window:{addEventListener(){}}}); // Parser-only check: browser boot handlers are not run.
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../spa/app.js'),'utf8'),context);
  const api = vm.runInContext('({state,parseResponseFrame,validSnapshot,tokenPayloadJson})',context);
  const payload = api.tokenPayloadJson(fixture.wire);
  assert.equal(payload.vk,fixture.public_key); api.state.bootId=payload.boot;
  for(const frame of fixture.envelopes) {
    const body=api.parseResponseFrame(frame,frame.type);
    assert(body,`C++ ${frame.type} parses in the actual SPA`);
    if(frame.type==='status')assert(api.validSnapshot(body));
    if(frame.type==='result') {
      assert(api.validSnapshot(body.status));assert.equal(body.op_id,fixture.intent.op_id);
      assert.equal(body.mutation.operation_id,fixture.intent.op_id);
      assert.equal(body.status.boot_id,fixture.intent.if_boot_id);
    }
    assert.equal(api.parseResponseFrame({...frame,upstream_seq:0},frame.type),null);
    assert.equal(api.parseResponseFrame({...frame,body_json:'[]'},frame.type),null);
    assert.equal(api.parseResponseFrame({...frame,type:frame.type==='status'?'event':'status'},frame.type),null);
    assert.equal(api.parseResponseFrame({...frame,sig:'A'.repeat(86)},frame.type),null);
  }
  console.log(`Actual SPA parsed ${fixture.envelopes.length} authenticated C++ response bodies and rejected malformed/legacy shapes`);
})().catch(error=>{console.error(error);process.exitCode=1;});
