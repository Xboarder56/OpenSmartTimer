#pragma once
// Endpoint double for legacy timer-behavior tests. The transport still carries
// real ECDH/AES-GCM; only the test's assertions see the decrypted envelopes.
#include "RelaySecure.h"
#include "RelayJson.h"
#include "Crypto.h"
#include <cassert>
#include <string>
#include <cstring>
using namespace opensmarttimer::relay;
struct SecureEndpoint {
    uint8_t hash[16]{},client[16]{},server[16]{},priv[32]{},pub[65]{},auth[32]{},rx[32]{},tx[32]{},opened[288]{};
    char fp[24]{},cid[24]{},sid[24]{};
    size_t opened_len=0;
    uint64_t tx_seq=0;
    std::string field(const std::string& body,const char* key) {
        char out[4096];int n=jsonCopyString(body.data(),body.size(),key,out,sizeof(out));assert(n>=0);return {out,size_t(n)};
    }
    std::string open(const uint8_t fingerprint[16]) {
        uint8_t digest[32],tag[32];
        assert(sha256(fingerprint,16,digest));memcpy(hash,digest,16);
        assert(secureRandom(client,16)&&ecdhGenerate(priv,pub));
        const char* salt="OST-RELAY-E2E-AUTH-v1";
        assert(hkdfSha256(fingerprint,16,(const uint8_t*)salt,strlen(salt),nullptr,0,auth,32));
        opened_len=secureOpenTranscript("timer_test_a","aabbccdd",hash,client,pub,opened);
        assert(hmacSha256(auth,32,opened,opened_len,tag));
        char peer[88],encoded[44],wire[512];
        assert(b64urlEncode(hash,16,fp,sizeof(fp))>0&&b64urlEncode(client,16,cid,sizeof(cid))>0);
        assert(b64urlEncode(pub,65,peer,sizeof(peer))>0&&b64urlEncode(tag,32,encoded,sizeof(encoded))>0);
        JsonWriter j(wire,sizeof(wire));j.beginObject();
        j.key("type");j.valueString("e2e_open");j.key("fp_hash");j.valueString(fp);
        j.key("client_id");j.valueString(cid);j.key("boot_id");j.valueString("aabbccdd");
        j.key("client_key");j.valueString(peer);j.key("tag");j.valueString(encoded);
        j.endObject();assert(j.finalize());return wire;
    }
    void accept(const std::string& body, const uint8_t signing_public_key[33]) {
        uint8_t peer[65],tag[32],expected[32],shared[32],keys[64],digest[32],info[64];
        const auto peer64=field(body,"server_key"),tag64=field(body,"tag"),sid64=field(body,"server_id");
        assert(b64urlDecode(peer64.data(),peer64.size(),peer,65)==65);
        assert(b64urlDecode(tag64.data(),tag64.size(),tag,32)==32);
        assert(b64urlDecode(sid64.data(),sid64.size(),server,16)==16);strcpy(sid,sid64.c_str());
        uint8_t accepted[kSecureAcceptTranscriptBytes],signature[64];
        const size_t accept_len=secureAcceptTranscript(opened,opened_len,server,peer,accepted);
        const auto sig64=field(body,"sig");
        assert(accept_len && b64urlDecode(sig64.data(),sig64.size(),signature,64)==64);
        assert(ecdsaP256Verify(signing_public_key,accepted,accept_len,signature));
        assert(hmacSha256(auth,32,accepted,accept_len,expected)&&!constantTimeCompare(expected,tag,32));
        assert(ecdhShared(priv,peer,shared));secureErase(priv,32);
        assert(sha256(accepted,accept_len,digest));
        const char* label="OST-RELAY-E2E-KEYS-v1";size_t n=strlen(label);
        memcpy(info,label,n);memcpy(info+n,digest,32);
        assert(hkdfSha256(shared,32,auth,32,info,n+32,keys,64));
        memcpy(tx,keys,32);memcpy(rx,keys+32,32);secureErase(keys,64);secureErase(shared,32);secureErase(auth,32);
    }
    std::string encrypt(std::string plain,const std::string& request) {
        if(request.size()>64)return "{}";
        uint64_t seq=++tx_seq;
        JsonView value{};
        if(jsonFindKey(plain.data(),plain.size(),"downstream_seq",&value)) {
            size_t at=size_t(value.data-plain.data());plain.replace(at,value.len,std::to_string(seq));
        }
        uint8_t nonce[12],aad[160],cipher[1816];char encoded[2424],wire[3000];
        secureNonce(seq,nonce);size_t al=secureAAD(1,hash,client,server,seq,request.c_str(),aad);
        assert(plain.size()<=1800&&gcmEncrypt(tx,nonce,aad,al,(const uint8_t*)plain.data(),plain.size(),cipher));
        assert(b64urlEncode(cipher,plain.size()+16,encoded,sizeof(encoded))>0);
        JsonWriter j(wire,sizeof(wire));j.beginObject();
        j.key("type");j.valueString("e2e");j.key("fp_hash");j.valueString(fp);j.key("client_id");j.valueString(cid);
        j.key("server_id");j.valueString(sid);j.key("seq");j.valueUint(seq);
        j.key("request_id");j.valueString(request.c_str());j.key("ciphertext");j.valueString(encoded);
        j.endObject();assert(j.finalize());return wire;
    }
    std::string decrypt(const std::string& wire) {
        int64_t seq;assert(jsonGetInt(wire.data(),wire.size(),"seq",&seq));
        auto encoded=field(wire,"ciphertext"),request=field(wire,"request_id");
        uint8_t cipher[1816],plain[1800],aad[160],nonce[12];
        int n=b64urlDecode(encoded.data(),encoded.size(),cipher,sizeof(cipher));assert(n>=16);
        secureNonce(uint64_t(seq),nonce);size_t al=secureAAD(2,hash,client,server,uint64_t(seq),request.c_str(),aad);
        assert(gcmDecrypt(rx,nonce,aad,al,cipher,size_t(n),plain));return {(char*)plain,size_t(n-16)};
    }
};
