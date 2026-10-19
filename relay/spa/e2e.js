// Endpoint crypto. This file belongs on the independently hosted trusted page.
'use strict';
const OSTSecure = (() => {
  const utf8 = new TextEncoder();
  const label = (s) => utf8.encode(s);
  const cat = (...parts) => {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  };
  const enc = (bytes) => btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  const dec = (value, length) => {
    if (typeof value !== 'string' || !/^[A-Za-z0-9_-]+$/.test(value)) throw Error('base64url');
    const raw = Uint8Array.from(atob(value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4-value.length%4)%4)), c => c.charCodeAt(0));
    if (enc(raw) !== value || (length !== undefined && raw.length !== length)) throw Error('base64url');
    return raw;
  };
  const u64 = (seq) => {
    const raw = new Uint8Array(8);
    new DataView(raw.buffer).setBigUint64(0, BigInt(seq));
    return raw;
  };
  const hkdf = async (ikm, salt, info, size) => {
    const key = await crypto.subtle.importKey('raw', ikm, 'HKDF', false, ['deriveBits']);
    return new Uint8Array(await crypto.subtle.deriveBits({name:'HKDF', hash:'SHA-256', salt, info}, key, size*8));
  };
  const macKey = raw => crypto.subtle.importKey('raw', raw, {name:'HMAC', hash:'SHA-256'}, false, ['sign', 'verify']);
  // The timer signs with ECDSA P-256 (SHA-256, RFC 6979, low-S r || s). Its
  // verify key travels as a 33-byte SEC1 compressed point to keep share links
  // short; WebCrypto raw import needs the 65-byte uncompressed form.
  const P256_P = (1n << 256n) - (1n << 224n) + (1n << 192n) + (1n << 96n) - 1n;
  const P256_B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604bn;
  const P256_HALF_N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n >> 1n;
  function bytesToBigInt(bytes) {
    return bytes.reduce((value, byte) => (value << 8n) | BigInt(byte), 0n);
  }
  function modPow(base, exponent, modulus) {
    let result = 1n;
    base %= modulus;
    for (; exponent > 0n; exponent >>= 1n) {
      if (exponent & 1n) result = (result * base) % modulus;
      base = (base * base) % modulus;
    }
    return result;
  }
  function decompressP256(compressed) {
    if (compressed.length !== 33 || (compressed[0] !== 2 && compressed[0] !== 3)) throw new Error('bad_key');
    const x = bytesToBigInt(compressed.subarray(1));
    if (x >= P256_P) throw new Error('bad_key');
    const ySquared = (((x * x) % P256_P) * x - 3n * x + P256_B) % P256_P;
    const rhs = (ySquared + P256_P) % P256_P;
    let y = modPow(rhs, (P256_P + 1n) >> 2n, P256_P);
    if ((y * y) % P256_P !== rhs) throw new Error('bad_key');
    if (Number(y & 1n) !== (compressed[0] & 1)) y = P256_P - y;
    const out = new Uint8Array(65);
    out[0] = 4;
    out.set(compressed.subarray(1), 1);
    for (let index = 64; index >= 33; --index, y >>= 8n) out[index] = Number(y & 0xffn);
    return out;
  }
  class Channel {
    constructor(hash, client, server, rx, tx) {
      Object.assign(this, {hash, client, server, rx, tx, rxSeq:0, txSeq:0});
    }
    aad(direction, seq, request) {
      if (typeof request !== 'string' || !/^[!-~]{0,64}$/.test(request)) throw Error('request_id');
      return cat(label('OST-RELAY-E2E-DATA-v1'), new Uint8Array([direction]), this.hash, this.client, this.server, u64(seq), label(request));
    }
    async encrypt(body, request) {
      const seq = ++this.txSeq;
      if (!this.tx || !Number.isSafeInteger(seq)) throw Error('channel_closed');
      if (body.type === 'command') body = {...body, downstream_seq:seq};
      const plain = label(JSON.stringify(body));
      if (plain.length > 1800) throw Error('too_large');
      const ciphertext = await crypto.subtle.encrypt({name:'AES-GCM', iv:cat(new Uint8Array(4), u64(seq)), additionalData:this.aad(1,seq,request), tagLength:128}, this.tx, plain);
      plain.fill(0);
      return {type:'e2e', fp_hash:enc(this.hash), client_id:enc(this.client), server_id:enc(this.server), seq, request_id:request, ciphertext:enc(new Uint8Array(ciphertext))};
    }
    async decrypt(frame) {
      if (!this.rx || frame.type !== 'e2e' || frame.fp_hash !== enc(this.hash) || frame.client_id !== enc(this.client) || frame.server_id !== enc(this.server) || !Number.isSafeInteger(frame.seq) || frame.seq <= this.rxSeq) return null;
      const cipher = dec(frame.ciphertext);
      if (cipher.length < 16 || cipher.length > 1816) throw Error('too_large');
      const plain = new Uint8Array(await crypto.subtle.decrypt({name:'AES-GCM', iv:cat(new Uint8Array(4),u64(frame.seq)), additionalData:this.aad(2,frame.seq,frame.request_id), tagLength:128}, this.rx, cipher));
      this.rxSeq = frame.seq;
      try { return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(plain)); }
      finally { plain.fill(0); }
    }
    close() { this.rx = this.tx = null; }
  }
  async function open(token) {
    if (!crypto.subtle) throw Error('verification_unavailable');
    const parts = token.split('.');
    if (parts.length !== 4 || parts[0] !== 'ost-cap' || parts[1] !== 'v1') throw Error('invalid_token');
    const p = JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(dec(parts[2])));
    if (p.aud !== 'ost-relay/1' || !/^[A-Za-z0-9_-]{1,24}$/.test(p.tid) || !/^[0-9a-f]{1,16}$/.test(p.boot)) throw Error('invalid_token');
    // Import the identity anchor from the locally retained capability, once.
    const verifyKey = await crypto.subtle.importKey('raw', decompressP256(dec(p.vk,33)),
      {name:'ECDSA', namedCurve:'P-256'}, false, ['verify']);
    const fp = dec(p.fp,16);
    const hash = new Uint8Array(await crypto.subtle.digest('SHA-256',fp)).slice(0,16);
    const auth = await hkdf(fp,label('OST-RELAY-E2E-AUTH-v1'),new Uint8Array(),32);
    fp.fill(0);
    const mac = await macKey(auth);
    const client = crypto.getRandomValues(new Uint8Array(16));
    // Private ECDH keys and derived traffic keys are nonextractable.
    let pair = await crypto.subtle.generateKey({name:'ECDH',namedCurve:'P-256'},false,['deriveBits']);
    const peer = new Uint8Array(await crypto.subtle.exportKey('raw',pair.publicKey));
    const transcript = cat(label('OST-RELAY-E2E-OPEN-v1'),label(p.tid),new Uint8Array(1),label(p.boot),new Uint8Array(1),hash,client,peer);
    const tag = new Uint8Array(await crypto.subtle.sign('HMAC',mac,transcript));
    return {
      payload:p,
      frame:{fp_hash:enc(hash),client_id:enc(client),boot_id:p.boot,client_key:enc(peer),tag:enc(tag)},
      async accept(frame) {
        if (!pair || frame.type !== 'e2e_accept' || frame.fp_hash !== enc(hash) || frame.client_id !== enc(client)) throw Error('bad_accept');
        const server = dec(frame.server_id,16), pub = dec(frame.server_key,65);
        const joined = cat(label('OST-RELAY-E2E-ACCEPT-v1'),transcript,server,pub);
        const signature = dec(frame.sig,64);
        if (bytesToBigInt(signature.subarray(32)) > P256_HALF_N ||
            !await crypto.subtle.verify({name:'ECDSA', hash:'SHA-256'},verifyKey,signature,joined)) throw Error('bad_accept');
        if (!await crypto.subtle.verify('HMAC',mac,dec(frame.tag,32),joined)) throw Error('bad_accept');
        const serverKey = await crypto.subtle.importKey('raw',pub,{name:'ECDH',namedCurve:'P-256'},false,[]);
        const shared = new Uint8Array(await crypto.subtle.deriveBits({name:'ECDH',public:serverKey},pair.privateKey,256));
        pair = null;
        const digest = new Uint8Array(await crypto.subtle.digest('SHA-256',joined));
        const keys = await hkdf(shared,auth,cat(label('OST-RELAY-E2E-KEYS-v1'),digest),64);
        shared.fill(0); auth.fill(0);
        const tx = await crypto.subtle.importKey('raw',keys.slice(0,32),'AES-GCM',false,['encrypt']);
        const rx = await crypto.subtle.importKey('raw',keys.slice(32),'AES-GCM',false,['decrypt']);
        keys.fill(0);
        return new Channel(hash,client,server,rx,tx);
      },
    };
  }
  return {open,enc,dec};
})();
if (typeof module !== 'undefined') module.exports = OSTSecure;
