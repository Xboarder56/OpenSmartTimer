// Actual WebCrypto endpoint. Server doubles use independent Node crypto.
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {webcrypto, createECDH, createPrivateKey, sign, createHmac, hkdfSync,
  createHash, randomBytes, createCipheriv} = require('node:crypto');
Object.defineProperty(globalThis, 'crypto', {value: webcrypto});
const secure = require('../spa/e2e.js');
const ORDER = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n;
const b64 = data => Buffer.from(data).toString('base64url');
const raw = text => Buffer.from(text, 'base64url');
const be64 = value => {const bytes=Buffer.alloc(8);bytes.writeBigUInt64BE(BigInt(value));return bytes;};
function identity() {
  const curve=createECDH('prime256v1');curve.generateKeys();const point=curve.getPublicKey();
  const key=createPrivateKey({format:'jwk',key:{kty:'EC',crv:'P-256',
    d:b64(curve.getPrivateKey()),x:b64(point.subarray(1,33)),y:b64(point.subarray(33))}});
  return {key,vk:b64(curve.getPublicKey(null,'compressed'))};
}
function signature(key,message) {
  const sig=sign('sha256',message,{key,dsaEncoding:'ieee-p1363'});
  const s=BigInt('0x'+sig.subarray(32).toString('hex'));
  return b64(Buffer.concat([sig.subarray(0,32),Buffer.from((s>ORDER/2n?ORDER-s:s).toString(16).padStart(64,'0'),'hex')]));
}
function token(payload) {return 'ost-cap.v1.'+b64(JSON.stringify(payload))+'.'+b64(randomBytes(16));}
function openedBytes(frame,payload,changes={}) {
  const p={...payload,...changes},f={...frame,...changes};
  return Buffer.concat([Buffer.from('OST-RELAY-E2E-OPEN-v1'+p.tid+'\0'+p.boot+'\0'),
    raw(f.fp_hash),raw(f.client_id),raw(f.client_key)]);
}
function acceptedBytes(opened,frame) {
  return Buffer.concat([Buffer.from('OST-RELAY-E2E-ACCEPT-v1'),opened,raw(frame.server_id),raw(frame.server_key)]);
}
async function fixture() {
  const signer=identity(),fp=randomBytes(16);
  const payload={aud:'ost-relay/1',tid:'timer-test-a',boot:'aabbccdd',vk:signer.vk,fp:b64(fp),
    sc:['view','adjust'],mint_up_ms:1,exp_up_ms:60000};
  const wire=token(payload),opened=await secure.open(wire),server=createECDH('prime256v1');server.generateKeys();
  const auth=Buffer.from(hkdfSync('sha256',fp,Buffer.from('OST-RELAY-E2E-AUTH-v1'),Buffer.alloc(0),32));
  const accept={type:'e2e_accept',upstream_seq:1,fp_hash:opened.frame.fp_hash,client_id:opened.frame.client_id,
    server_id:b64(randomBytes(16)),server_key:b64(server.getPublicKey())};
  const base=openedBytes(opened.frame,payload),joined=acceptedBytes(base,accept);
  const mac=message=>b64(createHmac('sha256',auth).update(message).digest());
  accept.tag=mac(joined);accept.sig=signature(signer.key,joined);
  const shared=server.computeSecret(raw(opened.frame.client_key));
  const keys=Buffer.from(hkdfSync('sha256',shared,auth,
    Buffer.concat([Buffer.from('OST-RELAY-E2E-KEYS-v1'),createHash('sha256').update(joined).digest()]),64));
  function reply(body,seq,request='status-proof') {
    const aad=Buffer.concat([Buffer.from('OST-RELAY-E2E-DATA-v1'),Buffer.from([2]),raw(accept.fp_hash),
      raw(accept.client_id),raw(accept.server_id),be64(seq),Buffer.from(request)]);
    const cipher=createCipheriv('aes-256-gcm',keys.subarray(32),Buffer.concat([Buffer.alloc(4),be64(seq)]));
    cipher.setAAD(aad);const data=Buffer.concat([cipher.update(JSON.stringify(body)),cipher.final(),cipher.getAuthTag()]);
    return {type:'e2e',upstream_seq:seq+1,fp_hash:accept.fp_hash,client_id:accept.client_id,
      server_id:accept.server_id,seq,request_id:request,ciphertext:b64(data)};
  }
  return {payload,wire,opened,accept,signer,base,joined,mac,reply};
}

test('identity is verified once; routine replies use AEAD and keys remain nonextractable', async () => {
  const f=await fixture(),verify=webcrypto.subtle.verify.bind(webcrypto.subtle);let identityChecks=0;
  webcrypto.subtle.verify=(algorithm,...args)=>{if(algorithm.name==='ECDSA')identityChecks++;return verify(algorithm,...args);};
  try {
    const channel=await f.opened.accept(f.accept);
    assert.equal(channel.tx.extractable,false);assert.equal(channel.rx.extractable,false);
    for(let i=1;i<=8;i++) {
      const body={type:'status',upstream_seq:i,body_json:JSON.stringify({boot_id:f.payload.boot})};
      assert.deepEqual(await channel.decrypt(f.reply(body,i)),body);
    }
    assert.equal(identityChecks,1);
    channel.close();await assert.rejects(channel.encrypt({type:'sync'},'closed'));
    assert.equal(await channel.decrypt(f.reply({},9)),null);
  } finally {webcrypto.subtle.verify=verify;}
});

test('missing identity proof and a mathematically valid high-S twin fail closed', async () => {
  const f=await fixture(),unsigned={...f.accept};delete unsigned.sig;
  await assert.rejects(f.opened.accept(unsigned));
  const sig=raw(f.accept.sig),s=BigInt('0x'+sig.subarray(32).toString('hex'));
  const high=Buffer.concat([sig.subarray(0,32),Buffer.from((ORDER-s).toString(16).padStart(64,'0'),'hex')]);
  await assert.rejects(f.opened.accept({...f.accept,sig:b64(high)}));
  assert(await f.opened.accept(f.accept),'bad accepts must not consume the legitimate exchange');
});

test('a share-secret holder can calculate a valid MAC but cannot impersonate timer identity', async () => {
  const f=await fixture(),attacker=identity(),server=createECDH('prime256v1');server.generateKeys();
  const forged={...f.accept,server_key:b64(server.getPublicKey()),server_id:b64(randomBytes(16))};
  const joined=acceptedBytes(f.base,forged);forged.tag=f.mac(joined);forged.sig=signature(attacker.key,joined);
  // The returned key is attacker-controlled; verification must use the capability anchor.
  await assert.rejects(f.opened.accept({...forged,vk:attacker.vk}));
  await assert.rejects(f.opened.accept({...forged,sig:f.accept.sig}));
  assert(await f.opened.accept(f.accept));
});

test('the identity signature binds protocol, timer, boot, grant, IDs and both ECDH keys', async () => {
  const f=await fixture(),other=createECDH('prime256v1');other.generateKeys();
  for(const changes of [{tid:'other-timer'},{boot:'deadbeef'},{fp_hash:b64(randomBytes(16))},
    {client_id:b64(randomBytes(16))},{client_key:b64(other.getPublicKey())}]) {
    const wrong=acceptedBytes(openedBytes(f.opened.frame,f.payload,changes),f.accept);
    await assert.rejects(f.opened.accept({...f.accept,sig:signature(f.signer.key,wrong)}));
  }
  for(const changes of [{server_id:b64(randomBytes(16))},{server_key:b64(other.getPublicKey())}]) {
    const changed={...f.accept,...changes};changed.tag=f.mac(acceptedBytes(f.base,changed));
    await assert.rejects(f.opened.accept(changed));
  }
  await assert.rejects(f.opened.accept({...f.accept,sig:signature(f.signer.key,f.joined.subarray('OST-RELAY-E2E-ACCEPT-v1'.length))}));
  assert(await f.opened.accept(f.accept));
});

test('another exchange or another capability identity cannot reuse an accepted handshake', async () => {
  const f=await fixture(),other=await secure.open(f.wire);
  await assert.rejects(other.accept({...f.accept,client_id:other.frame.client_id}));
  const otherIdentity=identity(),swapped=await secure.open(token({...f.payload,vk:otherIdentity.vk}));
  await assert.rejects(swapped.accept({...f.accept,client_id:swapped.frame.client_id}));
});

test('identity anchors must be valid compressed P-256 points', async () => {
  const f=await fixture(),key=raw(f.payload.vk);
  for(const bad of [Buffer.alloc(33),Buffer.concat([Buffer.from([4]),key.subarray(1)]),
    Buffer.concat([key.subarray(0,1),Buffer.alloc(32,255)]),key.subarray(0,32)]) {
    await assert.rejects(secure.open(token({...f.payload,vk:b64(bad)})));
  }
});

test('missing WebCrypto prevents channel setup', async () => {
  const vm=require('node:vm'),fs=require('node:fs'),path=require('node:path');
  const context=vm.createContext({crypto:{},TextEncoder,TextDecoder,Uint8Array,DataView});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../spa/e2e.js'),'utf8'),context);
  await assert.rejects(vm.runInContext('OSTSecure.open("unused")',context),/verification_unavailable/);
});

test('reply tampering, replay, plaintext and cross-channel substitution cannot authenticate', async () => {
  const f=await fixture(),channel=await f.opened.accept(f.accept);
  const body={type:'status',upstream_seq:1,body_json:'{"boot_id":"aabbccdd"}'},frame=f.reply(body,1);
  for(const changes of [{request_id:'other-proof'},{seq:2},{ciphertext:b64(Buffer.alloc(raw(frame.ciphertext).length))}]) {
    await assert.rejects(channel.decrypt({...frame,...changes}));
  }
  for(const changes of [{client_id:b64(randomBytes(16))},{server_id:b64(randomBytes(16))},{fp_hash:b64(randomBytes(16))}]) {
    assert.equal(await channel.decrypt({...frame,...changes}),null);
  }
  assert.equal(await channel.decrypt(body),null);
  assert.deepEqual(await channel.decrypt(frame),body);
  assert.equal(await channel.decrypt(frame),null);
});
