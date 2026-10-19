// Exercises the actual isolated RelayClient using a queued fake transport.
// Uses real OpenSSL ECDSA P-256; clock, transport and timer remain host doubles.
#include "RelayClient.h"
#include "IWssTransport.h"
#include "Crypto.h"
#include "RelayJson.h"
#include "Fakes.h"
#include "SecureEndpoint.h"
#include <cassert>
#include <openssl/evp.h>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace opensmarttimer::relay;
using namespace opensmarttimer::relay::test;
namespace {
bool stored_enabled = true;
bool storage_ok = true;
EnrollmentCredentials stored_credentials{};

class Transport : public IWssTransport {
public:
    FrameHandler handler = nullptr; void* context = nullptr;
    WssState current = WssState::Closed;
    bool sends_ok = true;
    bool auto_open = true;
    uint16_t close_code = 0;
    std::vector<std::string> sent;
    std::deque<std::string> incoming;
    SecureEndpoint* endpoint=nullptr;
    void setFrameHandler(FrameHandler h, void* c) override {handler=h;context=c;}
    bool open(const WssOpenParams& p) override {
        assert(std::string(p.subprotocol) == "ost-relay.v1");
        current=auto_open?WssState::Open:WssState::Connecting; return true;
    }
    void poll() override {
        while(!incoming.empty()) {
            auto frame=incoming.front();incoming.pop_front();handler(context,frame.data(),frame.size());
        }
    }
    bool sendJson(const char* b,size_t n) override {
        if (!sends_ok || current!=WssState::Open) return false;
        std::string wire(b,n);
        char type[24];assert(jsonCopyString(b,n,"type",type,sizeof(type))>0);
        sent.push_back(endpoint && !strcmp(type,"e2e") ? endpoint->decrypt(wire) : wire);
        return true;
    }
    void close(uint16_t code,const char*) override {close_code=code;current=WssState::Closed;}
    // The key the fake server "presented"; the client binds it into its hello.
    uint8_t tls_key[32];
    bool tls_key_known = true;
    Transport() { for (int i=0;i<32;++i) tls_key[i]=uint8_t(0xa0+i); }
    bool peerKeyDigest(uint8_t out[32]) const override {
        if (!tls_key_known || current!=WssState::Open) return false;
        memcpy(out,tls_key,32); return true;
    }
    WssState state() override {return current;}
    WssError lastError() override {return WssError::None;}
    uint32_t bytesSent() override {return 0;}
    uint32_t bytesReceived() override {return 0;}
};
class Timer : public ITimerControl {
public:
    FakeClock* clock;
    int applies=0;
    bool adapter_duplicate=false;
    std::string last_fp, denied_fp;
    char last_event[32]={};bool last_event_via_link=false;bool limited=false;
    void snapshot(StatusSnapshot* s) override {
        *s=StatusSnapshot{};
        s->state=StatusSnapshot::State::Running; s->remaining_ms=60000;
        s->session_duration_ms=60000; s->max_duration_ms=86400000;
        s->uptime_ms=clock->uptime_ms(); s->state_revision=applies;
        strcpy(s->boot_id,"aabbccdd");strcpy(s->session_id,"session-1");
        strcpy(s->build_id,"0123456789abcdef01234567");
        strcpy(s->device_label,"Timer \"A\"");
        strcpy(s->last_event,last_event);s->last_event_via_link=last_event_via_link;
        s->hold_limited=limited;s->hold_limit_remaining_ms=limited?30000:0;
    }
    MutationOutcome apply(const MutationRequest& r) override {
        assert(r.if_boot_id && std::string(r.if_boot_id)=="aabbccdd");
        assert(r.token_fp_hash && std::strlen(r.token_fp_hash)==22);
        last_fp=r.token_fp_hash;
        // Like RelayRuntime's loop: a link revoked after verification is
        // refused before mutation; other links are not.
        if(last_fp==denied_fp) return {ResultCode::Unauthorized,false,false};
        ++applies;return {ResultCode::Applied,!adapter_duplicate,adapter_duplicate};
    }
};
std::string field(const std::string& json,const char* name) {
    char out[4096];int n=jsonCopyString(json.data(),json.size(),name,out,sizeof(out));
    assert(n>=0);return std::string(out,size_t(n));
}
int64_t integer(const std::string& json,const char* name) {
    int64_t n;assert(jsonGetInt(json.data(),json.size(),name,&n));return n;
}
std::string resultBody(const std::string& frame) {
    assert(field(frame,"type")=="result");return field(frame,"body_json");
}
struct Fixture {
    FakeNvsStore nvs;FakeClock clock;FakeEntropy entropy;FakeIdentity identity;
    CapabilityToken tokens{{&nvs,&clock,&entropy,&identity}};
    Timer timer;Transport transport;
    RelayClient relay{{&timer,&tokens,&clock,&transport}};
    std::string wire;
    SecureEndpoint endpoint;
    Fixture() {
        stored_enabled=true;storage_ok=true;stored_credentials={};
        strcpy(stored_credentials.timer_id,"timer_test_a");
        strcpy(stored_credentials.wss_endpoint,"wss://relay.test/api/v1/timer");
        memset(stored_credentials.enrollment_secret,0x37,32);
        timer.clock=&clock;clock.set(1000);
        assert(tokens.begin());
        char buffer[1024];size_t n;
        assert(tokens.mint(0x7f,3600000,"all scopes",buffer,sizeof(buffer),&n)==MintResult::Ok);
        wire.assign(buffer,n);assert(relay.begin());
    }
    void receive(const std::string& frame) {
        auto actual=frame;
        if(field(frame,"type")=="command") actual=endpoint.encrypt(frame,field(frame,"op_id"));
        transport.incoming.push_back(actual);relay.loop();
    }
    // What the relay answers once it has verified the newest hello: only a
    // holder of the enrollment secret can compute this tag.
    std::string ack(const std::string& nonce_b64, const uint8_t* secret=nullptr) {
        std::string tag_b64;
        for (auto it=transport.sent.rbegin();it!=transport.sent.rend();++it)
            if (field(*it,"type")=="hello") { tag_b64=field(*it,"hello_tag"); break; }
        assert(tag_b64.size()==22 && nonce_b64.size()==22);
        uint8_t message[16+16+16]; memcpy(message,"OST-RELAY-ACK-v1",16);
        assert(b64urlDecode(nonce_b64.c_str(),22,message+16,16)==16);
        assert(b64urlDecode(tag_b64.c_str(),22,message+32,16)==16);
        uint8_t mac[32];
        assert(hmacSha256(secret?secret:stored_credentials.enrollment_secret,32,message,sizeof(message),mac));
        char encoded[24];b64urlEncode(mac,16,encoded,sizeof(encoded));
        return std::string("{\"type\":\"hello_ack\",\"ack_tag\":\"")+encoded+"\"}";
    }
    void connect() {
        relay.loop(); relay.loop();
        assert(relay.diagnostics().state==SessionState::AwaitingChallenge);
        assert(transport.sent.empty());
        receive("{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}");
        assert(relay.diagnostics().state==SessionState::HelloSent);
        receive(ack("AAECAwQFBgcICQoLDA0ODw"));
        assert(relay.diagnostics().state==SessionState::Live);
        TokenRecord records[1];assert(tokens.listLive(records,1)==1);
        receive(endpoint.open(records[0].fp));
        assert(field(transport.sent.back(),"type")=="e2e_accept");
        endpoint.accept(transport.sent.back(),tokens.signKeyPublic());transport.sent.pop_back();transport.endpoint=&endpoint;
        receive(endpoint.encrypt("{\"type\":\"sync\",\"cap_token\":\""+wire+"\"}","test-sync"));
    }
    std::string command(const std::string& id,int seq=1,int delta=1000,int64_t expiry=-1) {
        if(expiry<0) expiry=int64_t(clock.uptime_ms())+5000;
        char buffer[2048];JsonWriter jw(buffer,sizeof(buffer));
        jw.beginObject();jw.key("type");jw.valueString("command");
        jw.key("downstream_seq");jw.valueInt(seq);jw.key("op");jw.valueString("adjust");
        jw.key("op_id");jw.valueString(id.c_str());jw.key("cap_token");jw.valueString(wire.c_str());
        jw.key("if_boot_id");jw.valueString("aabbccdd");jw.key("if_session_id");jw.valueString("session-1");
        jw.key("expiry_uptime_ms");jw.valueInt(expiry);
        jw.key("params");jw.beginObject();jw.key("delta_ms");jw.valueInt(delta);jw.endObject();
        jw.endObject();assert(jw.finalize());return buffer;
    }
    void reconnect() {
        transport.current=WssState::Closed;relay.loop();relay.loop();clock.advance(1000);
        relay.loop();relay.loop();
        receive("{\"type\":\"challenge\",\"server_nonce\":\"AQECAwQFBgcICQoLDA0ODw\"}");
        receive(ack("AQECAwQFBgcICQoLDA0ODw"));
        assert(relay.diagnostics().state==SessionState::Live);
    }
};
void test_identity_handshake_and_encrypted_responses() {
    Fixture f; f.connect();
    assert(f.transport.sent.size()==2);
    const auto& hello=f.transport.sent[0];assert(field(hello,"type")=="hello");
    std::vector<uint8_t> bytes;
    auto append=[&](const std::string& s) {bytes.insert(bytes.end(),s.begin(),s.end());};
    append("OST-RELAY-HELLO-v2");for(int i=0;i<16;++i)bytes.push_back(uint8_t(i));
    append("timer_test_a");append("aabbccdd");
    for(uint64_t n:{uint64_t(1000),uint64_t(0),uint64_t(0)})
        for(int i=7;i>=0;--i)bytes.push_back(uint8_t(n>>(8*i)));
    // The hello ends with the key of the TLS session it was sent on.
    bytes.insert(bytes.end(),f.transport.tls_key,f.transport.tls_key+32);
    uint8_t tag[32];assert(hmacSha256(stored_credentials.enrollment_secret,32,bytes.data(),bytes.size(),tag));
    char encoded[24];b64urlEncode(tag,16,encoded,sizeof(encoded));
    assert(field(hello,"hello_tag")==encoded);
    auto frame=f.transport.sent[1];assert(field(frame,"type")=="status");
    auto body=field(frame,"body_json");assert(field(body,"device_name")=="Timer \"A\"");
    // Transport decrypts only after checking the identity-signed handshake.
    // Routine status/event/result responses carry no asymmetric signature.
    JsonView signature{};assert(!jsonFindKey(frame.data(),frame.size(),"sig",&signature));
    f.clock.advance(4999);f.relay.loop();assert(f.transport.sent.size()==2);
    f.clock.advance(1);f.relay.loop();assert(f.transport.sent.size()==3);
    assert(integer(field(f.transport.sent.back(),"body_json"),"uptime_ms")==6000);
    f.relay.onEventEmitted(7);assert(field(field(f.transport.sent.back(),"body_json"),"boot_id")=="aabbccdd");
    assert(field(field(f.transport.sent.back(),"body_json"),"type")=="snapshot_changed");
    // With a known timer event the partner sees its real name and source.
    strcpy(f.timer.last_event,"started");f.timer.last_event_via_link=true;
    f.relay.onEventEmitted(8);
    const auto event=field(f.transport.sent.back(),"body_json");
    assert(field(event,"type")=="started");
    bool via=false;assert(jsonGetBool(event.data(),event.size(),"via_share_link",&via)&&via);
    f.timer.limited=true;f.relay.onStatusChanged();
    assert(integer(field(f.transport.sent.back(),"body_json"),"share_link_limit_ms")==30000);
}
void test_replay_retry_and_correlation() {
    Fixture f;f.connect();std::string id(64,'a');
    f.receive(f.command(id));assert(f.timer.applies==1);
    auto original=f.transport.sent.back();auto result=resultBody(original);
    assert(field(result,"op_id")==id);assert(integer(result,"in_reply_to_downstream_seq")==2);
    assert(f.relay.diagnostics().result_ring_used==1);
    size_t before=f.transport.sent.size();f.reconnect();
    assert(f.transport.sent[before+1]==original); // hello, exact result, fresh status
    f.receive(f.command(id,2));assert(f.timer.applies==1);
    result=resultBody(f.transport.sent.back());assert(integer(result,"in_reply_to_downstream_seq")==3);
    JsonView mutation;assert(jsonFindKey(result.data(),result.size(),"mutation",&mutation));
    bool duplicate=false;assert(jsonGetBool(mutation.data,mutation.len,"duplicate",&duplicate)&&duplicate);
    f.receive(f.command(id,3,2000));assert(f.timer.applies==1);
    assert(field(resultBody(f.transport.sent.back()),"code")=="operation_conflict");
    f.receive("{\"type\":\"ack\",\"acked_upstream_seq\":-1}");assert(f.relay.diagnostics().result_ring_used==1);
    f.receive("{\"type\":\"ack\",\"acked_upstream_seq\":999999}");assert(f.relay.diagnostics().result_ring_used==1);
    f.receive("{\"type\":\"ack\",\"acked_upstream_seq\":"+std::to_string(f.relay.diagnostics().last_upstream_seq)+"}");
    assert(f.relay.diagnostics().result_ring_used==0);
    f.receive(f.command(id,4));assert(f.timer.applies==1); // retained acknowledged cache
}
void test_commands_carry_their_link_for_per_link_revocation() {
    Fixture f;f.connect();
    TokenRecord live[4];assert(f.tokens.listLive(live,4)==1);
    char hash[24];assert(CapabilityToken::fingerprintHash(live[0].fp,hash));
    f.receive(f.command(std::string(64,'c')));
    assert(f.timer.applies==1&&f.timer.last_fp==hash);
    f.timer.denied_fp=hash;
    f.clock.advance(5000);
    f.receive(f.command(std::string(64,'d'),2));
    assert(f.timer.applies==1);
    std::string last;
    for(auto it=f.transport.sent.rbegin();it!=f.transport.sent.rend();++it)
        if(field(*it,"type")=="result"){last=resultBody(*it);break;}
    assert(field(last,"op_id")==std::string(64,'d')&&field(last,"code")=="unauthorized");
}
// One live link. An acknowledged revocation frees its grant slot, so a
// renewed link does not wait up to 24 h for the old one to expire.
void test_link_cap_and_acknowledged_revocation_frees_a_slot() {
    Fixture f;
    char buffer[1024];size_t n;
    assert(f.tokens.mint(0x7f,3600000,"second",buffer,sizeof(buffer),&n)==MintResult::TableFull);
    f.connect();
    assert(f.relay.announceMintedToken(f.wire.data(),f.wire.size()));
    assert(!f.relay.canQueueGrant());
    auto lastOfType=[&](const char* type){
        for(auto it=f.transport.sent.rbegin();it!=f.transport.sent.rend();++it)
            if(field(*it,"type")==type) return *it;
        assert(false);return std::string();
    };
    const auto mint=lastOfType("token_mint");const auto hash=field(mint,"fp_hash");
    assert(f.tokens.revokeByHash(hash.c_str())==RevokeResult::Ok);
    assert(f.relay.announceRevokedToken(hash.c_str()));
    assert(!f.relay.canQueueGrant());  // still being delivered
    const auto revoke=lastOfType("token_revoke");
    f.receive("{\"type\":\"token_ack\",\"action\":\"token_revoke\",\"fp_hash\":\""+hash
        +"\",\"boot_id\":\"aabbccdd\",\"upstream_seq\":"+std::to_string(integer(revoke,"upstream_seq"))
        +",\"status\":\"revoked\"}");
    assert(f.relay.grantState(hash.c_str())==GrantState::Revoked&&f.relay.canQueueGrant());
    assert(f.tokens.mint(0x7f,3600000,"renewed",buffer,sizeof(buffer),&n)==MintResult::Ok);
    assert(f.relay.announceMintedToken(buffer,n)&&!f.relay.canQueueGrant());
}
// Extend: the same link is re-announced with a later expiry, and every
// authenticated status carries it so an open partner page follows without a new URL.
void test_extend_reannounces_and_status_carries_expiry() {
    Fixture f;
    assert(f.relay.announceMintedToken(f.wire.data(),f.wire.size()));
    f.connect();
    auto lastOfType=[&](const char* type){
        for(auto it=f.transport.sent.rbegin();it!=f.transport.sent.rend();++it)
            if(field(*it,"type")==type) return *it;
        assert(false);return std::string();
    };
    const auto mint=lastOfType("token_mint");const auto hash=field(mint,"fp_hash");
    const int64_t first=integer(mint,"exp_up_ms");
    f.relay.onStatusChanged();
    assert(integer(field(lastOfType("status"),"body_json"),"share_link_expires_up_ms")==first);
    assert(f.relay.announceExtendedToken(hash.c_str(),uint32_t(first+60000)));
    const auto again=lastOfType("token_mint");
    assert(field(again,"fp_hash")==hash&&field(again,"token_digest")==field(mint,"token_digest"));
    assert(integer(again,"exp_up_ms")==first+60000);
    assert(f.relay.grantState(hash.c_str())==GrantState::Pending);
    f.relay.onStatusChanged();
    assert(integer(field(lastOfType("status"),"body_json"),"share_link_expires_up_ms")==first+60000);
    assert(f.tokens.revokeByHash(hash.c_str())==RevokeResult::Ok);
    assert(f.relay.announceRevokedToken(hash.c_str()));
    assert(!f.relay.announceExtendedToken(hash.c_str(),uint32_t(first+120000)));  // never revives
}
void test_capacity_and_rejected_inputs() {
    Fixture f;f.connect();
    f.receive(f.command(std::string(65,'b')));assert(f.timer.applies==0);
    f.receive(f.command("far-future",1,1000,int64_t(UINT32_MAX)+1001));assert(f.timer.applies==0);
    f.receive(f.command("far-future",2,1000,f.clock.uptime_ms()+10001));assert(f.timer.applies==0);
    f.receive(f.command("expired",3,1000,f.clock.uptime_ms()));assert(f.timer.applies==0);
    for(int i=0;i<8;++i) {
        f.clock.advance(250);f.receive(f.command("operation-"+std::to_string(i),i+4));
    }
    assert(f.timer.applies==8 && f.relay.diagnostics().result_ring_used==8);
    f.clock.advance(250);f.receive(f.command("overflow",20));assert(f.timer.applies==8);
    assert(field(resultBody(f.transport.sent.back()),"code")=="throttled");
    f.receive(f.command("operation-0",21));assert(f.timer.applies==8);
    assert(field(resultBody(f.transport.sent.back()),"code")=="applied");
}
void test_deadlines_and_disabled_persistence() {
    {Fixture f;f.relay.loop();f.relay.loop();f.clock.advance(5000);f.relay.loop();
     assert(f.relay.diagnostics().state==SessionState::Failed);}
    {Fixture f;f.relay.loop();f.relay.loop();
     f.receive("{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}");
     f.clock.advance(5000);f.relay.loop();assert(f.relay.diagnostics().state==SessionState::Failed);}
    {Fixture f;f.transport.auto_open=false;f.relay.loop();f.clock.advance(10000);f.relay.loop();
     assert(f.relay.diagnostics().state==SessionState::Failed);}
    {Fixture f;f.connect();f.clock.advance(90000);f.relay.loop();
     assert(f.relay.diagnostics().state==SessionState::Failed);}
    {Fixture f;f.relay.loop();f.relay.loop();f.clock.advance(5000);
     f.receive("{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}");
     assert(f.transport.sent.empty());}
    {Fixture f;f.connect();f.clock.set(UINT32_MAX-10);f.receive("{\"type\":\"ping\"}");
     f.clock.advance(20);f.relay.loop();assert(f.relay.diagnostics().state==SessionState::Disabled);}
    {Fixture f;stored_enabled=false;assert(f.relay.begin());assert(f.relay.diagnostics().state==SessionState::Disabled);}
    {Fixture f;f.connect();storage_ok=false;assert(!f.relay.clearEnrollmentCredentials());
     assert(f.relay.diagnostics().state==SessionState::Disabled && f.transport.current==WssState::Closed);}
}
void test_grant_sync_ack_retry_revoke() {
    Fixture f;
    assert(f.relay.announceMintedToken(f.wire.data(), f.wire.size()));
    f.connect();
    // The queued registration precedes the first live status frame.
    const auto mint = f.transport.sent[f.transport.sent.size() - 2];
    assert(field(f.transport.sent.back(), "type") == "status");
    const auto hash = field(mint, "fp_hash");
    const auto expiry = integer(mint, "exp_up_ms");
    auto ack = [&](const std::string& frame, const char* status) {
        return std::string("{\"type\":\"token_ack\",\"action\":\"") + field(frame,"type")
            + "\",\"fp_hash\":\"" + hash + "\",\"boot_id\":\"aabbccdd\",\"upstream_seq\":"
            + std::to_string(integer(frame,"upstream_seq")) + ",\"status\":\"" + status + "\"}";
    };
    assert(f.relay.grantState(hash.c_str()) == GrantState::Pending);
    f.receive("{\"type\":\"ack\",\"acked_upstream_seq\":" + std::to_string(integer(mint,"upstream_seq")) + "}");
    assert(f.relay.grantState(hash.c_str()) == GrantState::Pending);
    f.clock.advance(2000); f.relay.loop();
    const auto retry = f.transport.sent.back();
    assert(integer(retry, "exp_up_ms") == expiry);
    f.receive(ack(mint,"ready")); // Stale ACK cannot confirm a newer send.
    assert(f.relay.grantState(hash.c_str()) == GrantState::Pending);
    f.receive(ack(retry,"ready"));
    assert(f.relay.grantState(hash.c_str()) == GrantState::Ready);
    assert(f.tokens.revokeByHash(hash.c_str()) == RevokeResult::Ok);
    assert(f.relay.announceRevokedToken(hash.c_str()));
    const auto revoke = f.transport.sent.back();
    assert(field(revoke,"type") == "token_revoke" && field(revoke,"token_digest") == field(mint,"token_digest"));
    f.receive(ack(retry,"ready"));
    assert(f.relay.grantState(hash.c_str()) == GrantState::Revoking);
    f.receive(ack(revoke,"revoked"));
    assert(f.relay.grantState(hash.c_str()) == GrantState::Revoked);
    // A same-boot reconnect reannounces the tombstone, never the mint.
    f.transport.current = WssState::Closed; f.relay.loop(); f.relay.loop();
    f.clock.advance(1000); f.relay.loop(); f.relay.loop();
    f.receive("{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}");
    f.receive(f.ack("AAECAwQFBgcICQoLDA0ODw"));
    // The tombstone goes before the first status, so a revoked partner
    // session never receives fresh timer state after a reconnect.
    assert(field(f.transport.sent.back(),"type") == "token_revoke");
    assert(f.relay.grantState(hash.c_str()) == GrantState::Revoking);
    f.clock.set(uint32_t(expiry));
    assert(f.relay.grantState(hash.c_str()) == GrantState::Expired);
    f.relay.forgetGrants();
    assert(f.relay.grantState(hash.c_str()) == GrantState::Unknown);
}
void test_aead_failure_and_sharing_availability() {
    Fixture f; assert(!f.relay.sharingAvailable());
    f.connect(); assert(f.relay.sharingAvailable());
    size_t before=f.transport.sent.size();
    assert(EVP_set_default_properties(nullptr,"provider=missing")==1);
    f.relay.onEventEmitted(9);
    assert(EVP_set_default_properties(nullptr,"")==1);
    assert(f.transport.sent.size()==before && f.relay.diagnostics().state==SessionState::Live);
    // Failed AEAD erased the channel, so restoring the provider cannot revive it.
    f.receive(f.command("after-crypto-failure"));assert(f.timer.applies==0);
    Fixture g;g.connect();g.transport.current=WssState::Closed;
    assert(!g.relay.sharingAvailable()); // No wait for another loop tick.
}
void test_mint_announcement_and_send_failure() {
    Fixture f;assert(f.relay.announceMintedToken(f.wire.data(),f.wire.size()));f.connect();
    assert(f.relay.announceMintedToken(f.wire.data(),f.wire.size()));
    // A grant queued while offline is sent before the first live status.
    size_t mint_at=f.transport.sent.size(),status_at=f.transport.sent.size();
    for(size_t i=0;i<f.transport.sent.size();++i){
        auto type=field(f.transport.sent[i],"type");
        if(type=="token_mint"&&mint_at==f.transport.sent.size())mint_at=i;
        if(type=="status"&&status_at==f.transport.sent.size())status_at=i;
    }
    assert(mint_at<status_at);
    auto frame=f.transport.sent[mint_at];assert(field(frame,"type")=="token_mint");
    uint8_t digest[32];assert(sha256((const uint8_t*)f.wire.data(),f.wire.size(),digest));
    char encoded[48];b64urlEncode(digest,32,encoded,sizeof(encoded));assert(field(frame,"token_digest")==encoded);
    f.transport.sends_ok=false;f.receive(f.command("lost-result"));assert(f.timer.applies==1);
    assert(f.relay.diagnostics().state==SessionState::Backoff || f.relay.diagnostics().state==SessionState::Failed);
    assert(f.relay.diagnostics().result_ring_used==1);
}
}
namespace opensmarttimer { namespace relay {
bool RelayClient::readEnrollmentCredentialsFromNvs() {cred_=stored_credentials;return true;}
bool RelayClient::writeEnrollmentCredentialsToNvs() {if(storage_ok)stored_credentials=cred_;return storage_ok;}
bool RelayClient::eraseEnrollmentCredentialsFromNvs() {return storage_ok;}
bool RelayClient::readEnabledPreferenceFromNvs() {return stored_enabled;}
bool RelayClient::setEnabledPreferencePersisted(bool value) {if(storage_ok)stored_enabled=value;return storage_ok;}
}}
// Regression (V2 hardware, 2026-09-28): loop() sampled the clock before
// poll(); a frame handled inside poll() recorded a later state-entry time and
// the unsigned delta wrapped, so every session died right after hello.
void test_session_survives_a_moving_clock() {
    Fixture f;
    f.clock.step_per_read = 1;
    f.connect();
    for (int i = 0; i < 20; ++i) {
        f.receive("{\"type\":\"ping\"}");
        assert(f.relay.diagnostics().state == SessionState::Live);
    }
    assert(f.relay.sharingAvailable());
    assert(std::string(f.relay.diagnostics().last_failure).empty());
}

// The transport accepts any certificate, so the relay must prove itself before
// the timer sends anything but its hello.
void test_relay_must_prove_itself_before_the_session_is_live() {
    const std::string challenge="{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}";
    const std::string nonce="AAECAwQFBgcICQoLDA0ODw";
    auto to_hello=[&](Fixture& f) {
        f.relay.loop();f.relay.loop();f.receive(challenge);
        assert(f.relay.diagnostics().state==SessionState::HelloSent);
        assert(f.transport.sent.size()==1 && field(f.transport.sent[0],"type")=="hello");
    };
    auto refused=[&](Fixture& f) {
        assert(f.relay.diagnostics().state!=SessionState::Live);
        assert(std::string(f.relay.diagnostics().last_failure)=="relay_auth_failed");
        // Nothing followed the hello: no status, link registration or revocation.
        assert(f.transport.sent.size()==1 && f.transport.close_code==1011);
    };
    { Fixture f; to_hello(f); f.receive("{\"type\":\"hello_ack\"}"); refused(f); }
    { Fixture f; to_hello(f); f.receive("{\"type\":\"hello_ack\",\"ack_tag\":\"AAAAAAAAAAAAAAAAAAAAAA\"}"); refused(f); }
    { Fixture f; to_hello(f); f.receive("{\"type\":\"hello_ack\",\"ack_tag\":\"short\"}"); refused(f); }
    // A relay that does not hold this timer's enrollment secret.
    { Fixture f; to_hello(f); uint8_t other[32]; memset(other,0x38,32); f.receive(f.ack(nonce,other)); refused(f); }
    // An acknowledgement computed for another challenge.
    { Fixture f; to_hello(f); f.receive(f.ack("AQECAwQFBgcICQoLDA0ODw")); refused(f); }
    // The genuine acknowledgement.
    { Fixture f; to_hello(f); f.receive(f.ack(nonce));
      assert(f.relay.diagnostics().state==SessionState::Live); }
    // The hello names the key the server presented: a different key gives a different tag.
    { Fixture a; to_hello(a); Fixture b; b.transport.tls_key[0]^=1; to_hello(b);
      assert(field(a.transport.sent[0],"hello_tag")!=field(b.transport.sent[0],"hello_tag")); }
    // Without a TLS key there is nothing to bind to, so no hello is sent at all.
    { Fixture f; f.transport.tls_key_known=false; f.relay.loop();f.relay.loop();f.receive(challenge);
      assert(f.transport.sent.empty() && f.relay.diagnostics().state!=SessionState::HelloSent);
      assert(std::string(f.relay.diagnostics().last_failure)=="bad_challenge"); }
}

int main() {
    test_relay_must_prove_itself_before_the_session_is_live();
    test_identity_handshake_and_encrypted_responses();test_replay_retry_and_correlation();
    test_commands_carry_their_link_for_per_link_revocation();
    test_link_cap_and_acknowledged_revocation_frees_a_slot();
    test_extend_reannounces_and_status_carries_expiry();
    test_capacity_and_rejected_inputs();test_deadlines_and_disabled_persistence();
    test_aead_failure_and_sharing_availability();
    test_mint_announcement_and_send_failure();test_grant_sync_ack_retry_revoke();
    test_session_survives_a_moving_clock();
    std::printf("all relay_client tests passed (real OpenSSL ECDSA P-256; sizeof client=%zu, record=%zu)\n",sizeof(RelayClient),sizeof(ResultRecord));
}
