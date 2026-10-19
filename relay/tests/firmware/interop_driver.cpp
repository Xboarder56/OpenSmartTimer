// Test-only JSON-lines bridge into the actual C++ protocol classes. No sockets,
// hardware, real credentials, or ESP adapters. All keys/identity are synthetic.
#include "RelayClient.h"
#include "IWssTransport.h"
#include "Crypto.h"
#include "RelayJson.h"
#include "Fakes.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
namespace opensmarttimer { namespace relay { unsigned testOnlySigningCalls(); void testOnlyFailSigning(bool value); }}
using namespace opensmarttimer::relay;
using namespace opensmarttimer::relay::test;
namespace {
EnrollmentCredentials credentials{};
class PipeTransport : public IWssTransport {
public:
    FrameHandler callback=nullptr; void* context=nullptr;
    WssState current=WssState::Closed;
    std::vector<std::string> sent;
    std::deque<std::string> received;
    void setFrameHandler(FrameHandler h, void* c) override {callback=h;context=c;}
    bool open(const WssOpenParams& p) override {assert(std::string(p.subprotocol)=="ost-relay.v1");current=WssState::Open;return true;}
    void poll() override {while(!received.empty()){auto f=received.front();received.pop_front();callback(context,f.data(),f.size());}}
    // No TLS on this pipe: the all-zero key is the binding a development relay expects.
    bool peerKeyDigest(uint8_t out[32]) const override {if(current!=WssState::Open)return false;std::memset(out,0,32);return true;}
    char hello_tag[24]={};
    bool sendJson(const char* b,size_t n) override {
        if(current!=WssState::Open)return false;
        char type[24];
        if(jsonCopyString(b,n,"type",type,sizeof(type))>0&&!std::strcmp(type,"hello"))
            assert(jsonCopyString(b,n,"hello_tag",hello_tag,sizeof(hello_tag))==22);
        sent.emplace_back(b,n);return true;
    }
    void close(uint16_t,const char*) override {current=WssState::Closed;}
    WssState state() override {return current;}
    WssError lastError() override {return WssError::None;}
    uint32_t bytesSent() override {return 0;}
    uint32_t bytesReceived() override {return 0;}
};
class TestTimer : public ITimerControl {
public:
    FakeClock& clock;
    int applies=0; uint32_t remaining=60000;
    explicit TestTimer(FakeClock& c):clock(c){}
    void snapshot(StatusSnapshot* s) override {
        *s={};s->state=StatusSnapshot::State::Running;s->remaining_ms=remaining;
        s->session_duration_ms=60000;s->max_duration_ms=86400000;s->uptime_ms=clock.uptime_ms();s->state_revision=applies;
        std::strcpy(s->boot_id,"aabbccdd");std::strcpy(s->session_id,"session-1");
        std::strcpy(s->build_id,"0123456789abcdef01234567");std::strcpy(s->device_label,"Timer \"A\" \\ café");
    }
    MutationOutcome apply(const MutationRequest& r) override {
        if(std::strcmp(r.if_boot_id,"aabbccdd"))return {ResultCode::StaleBoot,false,false};
        if(std::strcmp(r.if_session_id,"session-1"))return {ResultCode::StaleSession,false,false};
        if(r.op!=TimerOp::Adjust)return {ResultCode::UnsupportedOpcode,false,false};
        remaining=uint32_t(int64_t(remaining)+r.value_a);++applies;
        return {ResultCode::Applied,true,false};
    }
};
}
namespace opensmarttimer { namespace relay {
bool RelayClient::readEnrollmentCredentialsFromNvs(){cred_=credentials;return true;}
bool RelayClient::writeEnrollmentCredentialsToNvs(){credentials=cred_;return true;}
bool RelayClient::eraseEnrollmentCredentialsFromNvs(){return true;}
bool RelayClient::readEnabledPreferenceFromNvs(){return true;}
bool RelayClient::setEnabledPreferencePersisted(bool){return true;}
}}
int main() {
    std::strcpy(credentials.timer_id,"timer_test_a");std::strcpy(credentials.wss_endpoint,"wss://relay.test/api/v1/timer");
    std::memset(credentials.enrollment_secret,0x37,32);
    FakeNvsStore nvs;FakeClock clock;FakeEntropy entropy;FakeIdentity identity;
    clock.set(1000);CapabilityToken tokens({&nvs,&clock,&entropy,&identity});assert(tokens.begin());
    char wire[1024];size_t wire_size=0;
    assert(tokens.mint(kPartnerScopeMask,3600000,"interop",wire,sizeof(wire),&wire_size)==MintResult::Ok);
    TokenRecord* record=nullptr;assert(tokens.verify(wire,wire_size,Scope::View,&record)==VerifyResult::Ok);
    char hash[24],public_key[48];assert(CapabilityToken::fingerprintHash(record->fp,hash));
    assert(b64urlEncode(tokens.signKeyPublic(),33,public_key,sizeof(public_key))==44);
    TestTimer timer(clock);PipeTransport transport;RelayClient relay({&timer,&tokens,&clock,&transport});assert(relay.begin());
    char nonce[24]={};
    for(std::string line;std::getline(std::cin,line);) {
        if(line.size()>16384)return 2;
        char action[32];assert(jsonCopyString(line.data(),line.size(),"action",action,sizeof(action))>0);
        bool accepted=true;int verified=-1;
        if(!std::strcmp(action,"connect")){relay.loop();relay.loop();}
        else if(!std::strcmp(action,"reconnect")){
            transport.current=WssState::Closed;relay.loop();relay.loop();clock.advance(1000);relay.loop();relay.loop();
        } else if(!std::strcmp(action,"frame")){
            JsonView frame{};assert(jsonFindKey(line.data(),line.size(),"frame",&frame)&&frame.is_object);
            jsonCopyString(frame.data,frame.len,"server_nonce",nonce,sizeof(nonce));
            transport.received.emplace_back(frame.data,frame.len);relay.loop();
        } else if(!std::strcmp(action,"ack")){
            // Stand in for the relay: acknowledge the newest hello as only a
            // holder of this timer's enrollment secret can.
            uint8_t message[48],mac[32];std::memcpy(message,"OST-RELAY-ACK-v1",16);
            assert(b64urlDecode(nonce,22,message+16,16)==16&&b64urlDecode(transport.hello_tag,22,message+32,16)==16);
            assert(hmacSha256(credentials.enrollment_secret,32,message,sizeof(message),mac));
            char encoded[24];assert(b64urlEncode(mac,16,encoded,sizeof(encoded))==22);
            transport.received.push_back(std::string("{\"type\":\"hello_ack\",\"ack_tag\":\"")+encoded+"\"}");relay.loop();
        } else if(!std::strcmp(action,"mint"))accepted=relay.sharingAvailable()&&relay.announceMintedToken(wire,wire_size);
        else if(!std::strcmp(action,"revoke")){
            accepted=tokens.revokeByHash(hash)==RevokeResult::Ok&&relay.announceRevokedToken(hash);
        } else if(!std::strcmp(action,"fail_signing")) {
            bool value=false; assert(jsonGetBool(line.data(),line.size(),"value",&value));
            testOnlyFailSigning(value);
        } else if(!std::strcmp(action,"view_only")) {
            record->scope_mask=1;
        } else if(!std::strcmp(action,"extend")) {
            int64_t ms;assert(jsonGetInt(line.data(),line.size(),"ms",&ms)&&ms>0&&ms<=UINT32_MAX);
            accepted=tokens.extendByHash(hash,uint32_t(ms),false,nullptr)==RevokeResult::Ok;
        } else if(!std::strcmp(action,"tick")){
            int64_t ms;assert(jsonGetInt(line.data(),line.size(),"ms",&ms)&&ms>=0&&ms<=UINT32_MAX);
            clock.advance(uint32_t(ms));relay.loop();
        } else if(!std::strcmp(action,"status"))relay.onStatusChanged();
        else if(!std::strcmp(action,"event"))relay.onEventEmitted(7);
        else if(!std::strcmp(action,"verify_token")){
            char input[2048];int n=jsonCopyString(line.data(),line.size(),"token",input,sizeof(input));assert(n>0);
            verified=int(tokens.verify(input,size_t(n),Scope::Adjust,nullptr));
        } else if(!std::strcmp(action,"verify_signature")){
            char text[8192],key[48],signature[87];uint8_t message[4096],pub[33],sig[64];
            int n=jsonCopyString(line.data(),line.size(),"message",text,sizeof(text));assert(n>=0);
            int m=b64urlDecode(text,size_t(n),message,sizeof(message));assert(m>=0);
            assert(jsonCopyString(line.data(),line.size(),"key",key,sizeof(key))==44);
            assert(jsonCopyString(line.data(),line.size(),"signature",signature,sizeof(signature))==86);
            assert(b64urlDecode(key,44,pub,33)==33&&b64urlDecode(signature,86,sig,64)==64);
            verified=ecdsaP256Verify(pub,message,size_t(m),sig)?1:0;
        } else if(std::strcmp(action,"inspect"))return 3;
        char output[65536];JsonWriter jw(output,sizeof(output));jw.beginObject();
        jw.key("frames");jw.beginArray();for(const auto& f:transport.sent)jw.valueRaw(f.data(),f.size());jw.endArray();
        jw.key("wire");jw.valueString(wire);jw.key("fp_hash");jw.valueString(hash);jw.key("public_key");jw.valueString(public_key);
        jw.key("accepted");jw.valueBool(accepted);jw.key("verified");jw.valueInt(verified);
        jw.key("live");jw.valueBool(relay.diagnostics().state==SessionState::Live);
        jw.key("signing_calls");jw.valueUint(testOnlySigningCalls());
        jw.key("applies");jw.valueInt(timer.applies);jw.key("uptime_ms");jw.valueUint(clock.uptime_ms());
        jw.key("can_share");jw.valueBool(relay.sharingAvailable());jw.key("grant_state");jw.valueString(RelayClient::grantStateName(relay.grantState(hash)));
        jw.endObject();assert(jw.finalize());transport.sent.clear();std::cout<<output<<std::endl;
    }
}
