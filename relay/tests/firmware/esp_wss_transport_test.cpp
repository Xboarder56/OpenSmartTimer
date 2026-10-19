// The real EspWssTransport against a host stand-in for ESP-IDF 5.5's
// transport_ws, reproducing the SDK behaviors the adapter must survive:
//  - reads may return part of a frame (TLS record boundaries);
//  - PING/PONG/CLOSE are handled inside the SDK, the read returns 0 and the
//    frame's payload length stays set;
//  - esp_transport_ws_send_raw masks the caller's buffer IN PLACE.
// Built and run by relay/tests/check.sh with -DESP_PLATFORM and esp_stubs/.
#include "EspWssTransport.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "esp_timer.h"
#include "esp_transport_ws.h"
#include "freertos/task.h"

using namespace opensmarttimer::relay;

namespace {
struct Frame { int opcode; bool fin; std::string payload; };
struct Sent { int opcode; std::string payload; };

struct FakeSdk {
    std::deque<Frame> inbound;
    std::vector<Sent> sent;
    size_t chunk = 7;              // bytes per read, like small TLS records
    int opcode = 0;
    bool fin = true;
    int payload_len = 0;
    std::string current;
    size_t offset = 0;             // consumed bytes of `current`
    int64_t now_us = 0;
    int pongs = 0;
    int close_echoes = 0;
} sdk;

esp_transport_handle_t handle(int n) { return reinterpret_cast<esp_transport_handle_t>(static_cast<uintptr_t>(n)); }
}  // namespace

namespace opensmarttimer { namespace relay {
esp_transport_handle_t newPinnedTlsTransport(TlsPinPolicy* policy) {
    // The WebSocket requires no pin; the real verifier records the leaf key.
    assert(policy->pin == nullptr);
    policy->leaf_seen = true;
    policy->pin_matched = false;
    for (int i = 0; i < 32; ++i) policy->leaf_digest[i] = uint8_t(0x40 + i);
    return handle(1);
}
}}  // namespace opensmarttimer::relay

extern "C" {
int64_t esp_timer_get_time(void) { return sdk.now_us += 1000; }
void vTaskDelay(TickType_t) {}
int esp_transport_connect(esp_transport_handle_t, const char*, int, int) { return 0; }
int esp_transport_close(esp_transport_handle_t) { return 0; }
int esp_transport_destroy(esp_transport_handle_t) { return 0; }
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t) { return handle(2); }
int esp_transport_ws_set_path(esp_transport_handle_t, const char*) { return 0; }
int esp_transport_ws_set_subprotocol(esp_transport_handle_t, const char*) { return 0; }
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t) { return 101; }
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t) { return sdk.fin; }
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t) {
    return static_cast<ws_transport_opcodes_t>(sdk.opcode);
}
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t) { return sdk.payload_len; }

int esp_transport_poll_read(esp_transport_handle_t, int) {
    return (sdk.offset < sdk.current.size() || !sdk.inbound.empty()) ? 1 : 0;
}

int esp_transport_read(esp_transport_handle_t, char* buffer, int len, int) {
    if (sdk.offset >= sdk.current.size()) {   // start the next frame
        if (sdk.inbound.empty()) return 0;     // timeout
        Frame f = sdk.inbound.front();
        sdk.inbound.pop_front();
        sdk.opcode = f.opcode;
        sdk.fin = f.fin;
        sdk.payload_len = int(f.payload.size());
        if (f.opcode & 0x08) {                 // control: handled inside the SDK
            if (f.opcode == WS_TRANSPORT_OPCODES_PING) {
                sdk.sent.push_back({WS_TRANSPORT_OPCODES_PONG, f.payload});
                ++sdk.pongs;
            } else if (f.opcode == WS_TRANSPORT_OPCODES_CLOSE) {
                ++sdk.close_echoes;
            }
            sdk.current.clear();
            sdk.offset = 0;
            return 0;                          // payload_len stays set
        }
        sdk.current = f.payload;
        sdk.offset = 0;
    }
    const size_t n = std::min({size_t(len), sdk.chunk, sdk.current.size() - sdk.offset});
    memcpy(buffer, sdk.current.data() + sdk.offset, n);
    sdk.offset += n;
    return int(n);
}

int esp_transport_ws_send_raw(esp_transport_handle_t, ws_transport_opcodes_t opcode, const char* b,
                              int len, int) {
    // Same in-place masking the SDK performs; a read-only buffer faults here.
    char* buffer = const_cast<char*>(b);
    for (int i = 0; i < len; ++i) buffer[i] ^= 0x5a;
    std::string payload(buffer, size_t(len));
    for (char& c : payload) c ^= 0x5a;
    for (int i = 0; i < len; ++i) buffer[i] ^= 0x5a;
    sdk.sent.push_back({int(opcode) & 0x0f, payload});
    return len;
}
}  // extern "C"

namespace {
std::vector<std::string> received;
void onFrame(void*, const char* json, size_t len) { received.emplace_back(json, len); }


void openTransport(EspWssTransport& t) {
    sdk = FakeSdk{};
    received.clear();
    t.setFrameHandler(onFrame, nullptr);
    t.setAllowed(true);
    WssOpenParams params{"wss://relay.test/api/v1/timer", "ost-relay.v1"};
    uint8_t key[32];
    assert(t.open(params));
    assert(!t.peerKeyDigest(key));   // no TLS session yet
    t.performConnect();
    assert(t.state() == WssState::Open);
    // The key the server presented is available for the hello to bind to.
    assert(t.peerKeyDigest(key) && key[0] == 0x40 && key[31] == 0x5f);
}

void drain(EspWssTransport& t) {
    for (int i = 0; i < 10 && t.state() == WssState::Open && esp_transport_poll_read(nullptr, 0); ++i) t.poll();
}
}  // namespace

int main() {
    const std::string challenge = "{\"type\":\"challenge\",\"server_nonce\":\"AAECAwQFBgcICQoLDA0ODw\"}";
    const std::string ack = "{\"type\":\"hello_ack\",\"session_id\":\"s1\"}";

    // 1. A text frame arriving in 7-byte reads is reassembled exactly.
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, true, challenge});
        drain(t);
        assert(received.size() == 1 && received[0] == challenge);
        assert(t.state() == WssState::Open);
    }
    // 2. Real WebSocket PINGs (with payload) between text frames: answered by
    //    the SDK, never re-read by the adapter, and the next frame is intact.
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, true, challenge});
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_PING, true, "keepalive-0123456789"});
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_PING, true, ""});
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_PONG, true, "unsolicited"});
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, true, ack});
        drain(t);
        assert(t.state() == WssState::Open);
        assert(received.size() == 2 && received[0] == challenge && received[1] == ack);
        assert(sdk.pongs == 2 && t.lastError() == WssError::None);
    }
    // 3. A server CLOSE ends the session even though the SDK read returns 0.
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_CLOSE, true, std::string("\x03\xe8", 2)});
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, true, ack});
        drain(t);
        assert(t.state() == WssState::Failed && t.lastError() == WssError::PeerClosed);
        assert(received.empty());
    }
    // 4. Fragmented, binary and oversize frames are protocol errors, never
    //    delivered as partial commands.
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, false, "{\"type\":\"comm"});
        drain(t);
        assert(t.state() == WssState::Failed && t.lastError() == WssError::ProtocolError && received.empty());
    }
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_BINARY, true, "\x01\x02"});
        drain(t);
        assert(t.lastError() == WssError::ProtocolError && received.empty());
    }
    {
        EspWssTransport t; openTransport(t);
        sdk.inbound.push_back({WS_TRANSPORT_OPCODES_TEXT, true, std::string(EspWssTransport::kMaxFrameBytes + 1, 'x')});
        drain(t);
        assert(t.lastError() == WssError::ProtocolError && received.empty());
    }
    // 5. Sending a string literal (read-only memory, like RelayClient's static
    //    pong) must not reach the SDK's in-place masking; the caller's bytes
    //    are unchanged and the peer receives them exactly.
    {
        EspWssTransport t; openTransport(t);
        static const char pong[] = "{\"type\":\"pong\"}";
        assert(t.sendJson(pong, sizeof(pong) - 1));
        assert(!sdk.sent.empty() && sdk.sent.back().opcode == WS_TRANSPORT_OPCODES_TEXT);
        assert(sdk.sent.back().payload == pong && std::string(pong) == "{\"type\":\"pong\"}");
        t.setAllowed(false);
        assert(!t.sendJson(pong, sizeof(pong) - 1));
    }
    std::puts("EspWssTransport SDK-semantics tests passed (split frames, PING/PONG/CLOSE, "
              "fragments, oversize, read-only send buffers)");
    return 0;
}
