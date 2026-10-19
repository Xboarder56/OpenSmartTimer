#include "RelayClient.h"
#include "RelayJson.h"
#include "Crypto.h"
#include "IClock.h"
#include "IWssTransport.h"
#include <memory>
#include <new>
#include <string.h>
namespace opensmarttimer { namespace relay {
namespace {
constexpr size_t kPlainMax=1800;
bool decode(const char* body,size_t len,const char* key,uint8_t* out,size_t expected) {
    const char* value; size_t n;
    return jsonGetString(body,len,key,&value,&n) && b64urlDecode(value,n,out,expected)==int(expected);
}
}
void RelayClient::sweepSecure() {
    const uint32_t now=deps_.clock->uptime_ms();
    for (auto& s:secure_sessions_) if(s.occupied &&
        (!deps_.tokens->liveByHash(s.fp_hash) || now<s.last_active ||
         now-s.last_active>(s.confirmed?120000u:10000u))) secureErase(&s,sizeof(s));
}
bool RelayClient::sendSecureAccept(SecureSession& s) {
    char pub[88],tag[44],sig[87],wire[640];
    if(b64urlEncode(s.server_key,65,pub,sizeof(pub))!=87 || b64urlEncode(s.accept_tag,32,tag,sizeof(tag))!=43
        || b64urlEncode(s.accept_sig,64,sig,sizeof(sig))!=86) return false;
    JsonWriter j(wire,sizeof(wire)); j.beginObject();
    j.key("type");j.valueString("e2e_accept");j.key("upstream_seq");j.valueUint(++last_upstream_seq_);
    j.key("fp_hash");j.valueString(s.fp_hash);j.key("client_id");j.valueString(s.client_id);
    j.key("server_id");j.valueString(s.server_id);j.key("server_key");j.valueString(pub);j.key("tag");j.valueString(tag);
    j.key("sig");j.valueString(sig);
    j.endObject();size_t n=j.finalize();
    if (!n || !deps_.transport->sendJson(wire,n-1)) { failTransport("secure_accept_send_failed");return false; }
    return true;
}
void RelayClient::handleSecureOpen(const char* body,size_t len) {
    char fp[24],cid[24],boot[24];uint8_t client[16],peer[65],hash[16],tag[32];
    if(jsonCopyString(body,len,"fp_hash",fp,sizeof(fp))!=22 || jsonCopyString(body,len,"client_id",cid,sizeof(cid))!=22
        || jsonCopyString(body,len,"boot_id",boot,sizeof(boot))<=0
        || !decode(body,len,"client_id",client,16) || !decode(body,len,"fp_hash",hash,16)
        || !decode(body,len,"client_key",peer,65) || !decode(body,len,"tag",tag,32)) return;
    TokenRecord* record=deps_.tokens->liveByHash(fp);if(!record) return;
    StatusSnapshot snap{};deps_.timer->snapshot(&snap);if(strcmp(boot,snap.boot_id)) return;
    static const uint8_t salt[]="OST-RELAY-E2E-AUTH-v1";
    uint8_t auth[32]={},transcript[288]={},expected[32]={};
    size_t n=secureOpenTranscript(cred_.timer_id,boot,hash,client,peer,transcript);
    if(!n || !hkdfSha256(record->fp,16,salt,sizeof(salt)-1,nullptr,0,auth,32)
        || !hmacSha256(auth,32,transcript,n,expected) || constantTimeCompare(tag,expected,32)) {
        secureErase(auth,32);return;
    }
    for(auto& s:secure_sessions_) if(s.occupied && !strcmp(s.client_id,cid)) {
        if(!strcmp(s.fp_hash,fp) && !memcmp(s.client_key,peer,65)) sendSecureAccept(s);
        secureErase(auth,32);return;
    }
    const uint32_t now=deps_.clock->uptime_ms();
    if(secure_open_seen_ && now-last_secure_open_<1000) { secureErase(auth,32);return; }
    last_secure_open_=now;secure_open_seen_=true;
    SecureSession* slot=nullptr;size_t pending=0;
    for(auto& s:secure_sessions_) { if(!s.occupied && !slot) slot=&s; if(s.occupied && !s.confirmed) ++pending; }
    if(!slot || pending>=2) { secureErase(auth,32);return; }
    SecureSession s{};uint8_t priv[32]={},shared[32]={},keys[64]={},digest[32]={},info[64]={};
    uint8_t accepted[kSecureAcceptTranscriptBytes]={};
    bool ok=secureRandom(s.server,16) && ecdhGenerate(priv,s.server_key) && ecdhShared(priv,peer,shared);
    secureErase(priv,32);
    const size_t accept_len=secureAcceptTranscript(transcript,n,s.server,s.server_key,accepted);
    static const char key_label[]="OST-RELAY-E2E-KEYS-v1";
    memcpy(info,key_label,sizeof(key_label)-1);
    ok=ok && accept_len && hmacSha256(auth,32,accepted,accept_len,s.accept_tag)
        && sha256(accepted,accept_len,digest)
        && ecdsaP256Sign(deps_.tokens->signKeyPrivate(),accepted,accept_len,s.accept_sig);
    memcpy(info+sizeof(key_label)-1,digest,32);
    ok=ok && hkdfSha256(shared,32,auth,32,info,sizeof(key_label)-1+32,keys,64);
    if(ok) {
        strcpy(s.fp_hash,fp);strcpy(s.client_id,cid);memcpy(s.fp_raw_hash,hash,16);memcpy(s.client,client,16);
        memcpy(s.client_key,peer,65);memcpy(s.rx_key,keys,32);memcpy(s.tx_key,keys+32,32);
        ok=b64urlEncode(s.server,16,s.server_id,sizeof(s.server_id))==22;
        s.occupied=true;s.last_active=now;
        if(ok) { *slot=s; if(!sendSecureAccept(*slot)) secureErase(slot,sizeof(*slot)); }
    }
    secureErase(&s,sizeof(s));secureErase(auth,32);secureErase(shared,32);secureErase(keys,64);
    secureErase(accepted,sizeof(accepted));
}
bool RelayClient::sendSecure(SecureSession& s,const char* plain,size_t len,const char* request) {
    if(len>kPlainMax || s.tx_seq>=9007199254740991ULL) { secureErase(&s,sizeof(s));return false; }
    uint64_t seq=++s.tx_seq;uint8_t aad[160],nonce[12];secureNonce(seq,nonce);
    size_t al=secureAAD(2,s.fp_raw_hash,s.client,s.server,seq,request,aad);
    const size_t encoded_size=((len+16+2)/3)*4+1,wire_size=encoded_size+320;
    std::unique_ptr<uint8_t[]> data(new(std::nothrow) uint8_t[len+16]);
    std::unique_ptr<char[]> encoded(new(std::nothrow) char[encoded_size]);
    std::unique_ptr<char[]> wire(new(std::nothrow) char[wire_size]);
    if(!al || !data || !encoded || !wire || !gcmEncrypt(s.tx_key,nonce,aad,al,(const uint8_t*)plain,len,data.get())
        || b64urlEncode(data.get(),len+16,encoded.get(),encoded_size)<0) {
        secureErase(&s,sizeof(s));return false;
    }
    JsonWriter j(wire.get(),wire_size);j.beginObject();
    j.key("type");j.valueString("e2e");j.key("upstream_seq");j.valueUint(++last_upstream_seq_);
    j.key("fp_hash");j.valueString(s.fp_hash);j.key("client_id");j.valueString(s.client_id);
    j.key("server_id");j.valueString(s.server_id);j.key("seq");j.valueUint(seq);
    j.key("request_id");j.valueString(request);j.key("ciphertext");j.valueString(encoded.get());
    j.endObject();size_t n=j.finalize();
    if (!n || !deps_.transport->sendJson(wire.get(),n-1)) { failTransport("secure_send_failed");return false; }
    return true;
}
void RelayClient::handleSecureData(const char* body,size_t len) {
    char cid[24],sid[24],fp[24],request[65];int64_t seq=0;
    if(jsonCopyString(body,len,"client_id",cid,sizeof(cid))!=22 || jsonCopyString(body,len,"server_id",sid,sizeof(sid))!=22
        || jsonCopyString(body,len,"fp_hash",fp,sizeof(fp))!=22 || jsonCopyString(body,len,"request_id",request,sizeof(request))<0
        || !jsonGetInt(body,len,"seq",&seq) || seq<=0 || uint64_t(seq)>9007199254740991ULL) return;
    for(const char* p=request;*p;++p) if(*p<0x21 || *p>0x7e) return;
    SecureSession* s=nullptr;
    for(auto& c:secure_sessions_) if(c.occupied && !strcmp(c.client_id,cid) && !strcmp(c.server_id,sid) && !strcmp(c.fp_hash,fp)) s=&c;
    if(!s || uint64_t(seq)<=s->rx_seq || !deps_.tokens->liveByHash(fp)) return;
    const char* encoded;size_t el;uint8_t cipher[kPlainMax+16],plain[kPlainMax+1],aad[160],nonce[12];
    if(!jsonGetString(body,len,"ciphertext",&encoded,&el)) return;
    int n=b64urlDecode(encoded,el,cipher,sizeof(cipher));if(n<16) return;
    size_t al=secureAAD(1,s->fp_raw_hash,s->client,s->server,uint64_t(seq),request,aad);secureNonce(uint64_t(seq),nonce);
    if(!al || !gcmDecrypt(s->rx_key,nonce,aad,al,cipher,size_t(n),plain)) return;
    s->rx_seq=uint64_t(seq);s->last_active=deps_.clock->uptime_ms();n-=16;plain[n]=0;
    const char* token;size_t tl;TokenRecord* authorized=nullptr;char hash[24],type[24];
    bool valid=jsonGetString((const char*)plain,n,"cap_token",&token,&tl)
        && deps_.tokens->verify(token,tl,Scope::View,&authorized)==VerifyResult::Ok
        && CapabilityToken::fingerprintHash(authorized->fp,hash) && !strcmp(hash,fp)
        && jsonCopyString((const char*)plain,n,"type",type,sizeof(type))>0;
    if(valid) {
        s->confirmed=true;active_secure_=s;
        if(!strcmp(type,"sync")) {
            sendSecureStatus(cid,request);
        } else if(!strcmp(type,"lookup")) {
            for(auto& r:result_ring_) if(r.occupied && !strcmp(r.op_id,request)) {
                strcpy(r.secure_client_id,cid);sendResultUpstream(r);break;
            }
        } else if(!strcmp(type,"command")) {
            int64_t dseq=0;char opid[65];
            if(jsonGetInt((const char*)plain,n,"downstream_seq",&dseq) && dseq==seq
                && jsonCopyString((const char*)plain,n,"op_id",opid,sizeof(opid))>0 && !strcmp(opid,request))
                handleDownstreamCommand(plain,size_t(n));
        }
        active_secure_=nullptr;
    }
    secureErase(plain,sizeof(plain));
}
}}
