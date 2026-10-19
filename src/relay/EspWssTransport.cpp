#if defined(ESP_PLATFORM)
#include "EspWssTransport.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace opensmarttimer { namespace relay {

namespace {
constexpr int kConnectTimeoutMs = 10000;
constexpr int kReadTimeoutMs = 1000;
constexpr int kWriteTimeoutMs = 2000;
constexpr int kSwitchingProtocols = 101;
}  // namespace

bool EspWssTransport::open(const WssOpenParams& params) {
    teardown();
    error_ = WssError::None;
    if (!allowed_ || !params.url || !parseWssUrl(params.url, &endpoint_)) {
        state_ = WssState::Failed;
        return false;
    }
    state_ = WssState::Connecting;
    connect_pending_ = true;
    return true;
}

void EspWssTransport::performConnect() {
    if (!connect_pending_) return;
    connect_pending_ = false;
    if (!allowed_) { fail(WssError::LocalTimeout); return; }
    // No pin: the leaf key is recorded, and RelayClient authenticates the relay.
    policy_ = TlsPinPolicy{nullptr, false, false, {}};
    ssl_ = newPinnedTlsTransport(&policy_);
    ws_ = ssl_ ? esp_transport_ws_init(ssl_) : nullptr;
    if (!ws_) { fail(WssError::HeapExhausted); return; }
    esp_transport_ws_set_path(ws_, endpoint_.path);
    esp_transport_ws_set_subprotocol(ws_, kWebSocketSubprotocol);
    if (esp_transport_connect(ws_, endpoint_.host, endpoint_.port, kConnectTimeoutMs) < 0) {
        fail(!policy_.leaf_seen ? WssError::TlsHandshake : WssError::WebsocketHandshake);
        return;
    }
    if (!allowed_) { fail(WssError::LocalTimeout); return; }
    if (!policy_.leaf_seen) { fail(WssError::TlsHandshake); return; }
    if (esp_transport_ws_get_upgrade_request_status(ws_) != kSwitchingProtocols) {
        fail(WssError::WebsocketHandshake);
        return;
    }
    state_ = WssState::Open;
}

void EspWssTransport::waitReadable(uint32_t ms) {
    if (state_ != WssState::Open || !ws_) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    esp_transport_poll_read(ws_, int(ms));
}

void EspWssTransport::poll() {
    if (state_ != WssState::Open || !ws_) return;
    if (!allowed_) { fail(WssError::LocalTimeout); return; }
    // Drain complete frames that are already available, without blocking.
    for (int frames = 0; frames < 4 && esp_transport_poll_read(ws_, 0) > 0; ++frames) {
        int got = esp_transport_read(ws_, frame_, kMaxFrameBytes, kReadTimeoutMs);
        if (got < 0) { fail(WssError::PeerClosed); return; }
        const ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(ws_);
        // The SDK answers PING and CLOSE itself and returns 0 while keeping
        // payload_len set. Never read those bytes again; a CLOSE ends the session.
        if (opcode == WS_TRANSPORT_OPCODES_CLOSE) { fail(WssError::PeerClosed); return; }
        if (got == 0) continue;
        const int total = esp_transport_ws_get_read_payload_len(ws_);
        if (total < 0 || size_t(total) > kMaxFrameBytes) { fail(WssError::ProtocolError); return; }
        // Protocol messages are complete text frames. Reject fragmentation
        // explicitly rather than executing a valid-looking first fragment.
        if (opcode != WS_TRANSPORT_OPCODES_TEXT || !esp_transport_ws_get_fin_flag(ws_)) {
            fail(WssError::ProtocolError); return;
        }
        const int64_t deadline = esp_timer_get_time() + int64_t(kReadTimeoutMs) * 1000;
        while (got < total) {
            const int remaining = int((deadline - esp_timer_get_time()) / 1000);
            if (!allowed_ || remaining <= 0) { fail(WssError::LocalTimeout); return; }
            const int more = esp_transport_read(ws_, frame_ + got, size_t(total - got), remaining);
            if (more <= 0) { fail(WssError::ProtocolError); return; }
            got += more;
        }
        if (!allowed_) { fail(WssError::LocalTimeout); return; }
        bytes_received_ += uint32_t(got);
        frame_[got] = '\0';
        if (handler_) handler_(handler_ctx_, frame_, size_t(got));
        if (state_ != WssState::Open) return;  // the handler may have closed
    }
}

bool EspWssTransport::sendJson(const char* json, size_t len) {
    if (!allowed_ || state_ != WssState::Open || !ws_ || !json || !len || len > kMaxFrameBytes) return false;
    // ESP-IDF masks outbound data IN PLACE despite accepting const char*.
    // Reuse the receive scratch: callbacks parse their input before sending,
    // and never retain it after sendJson(). No extra 4 KiB heap allocation.
    memmove(frame_, json, len);
    const int sent = esp_transport_ws_send_raw(
        ws_, static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN),
        frame_, int(len), kWriteTimeoutMs);
    if (sent != int(len)) { fail(WssError::PeerClosed); return false; }
    bytes_sent_ += uint32_t(len);
    return true;
}

void EspWssTransport::close(uint16_t code, const char* reason) {
    connect_pending_ = false;
    if (ws_ && state_ == WssState::Open) {
        char payload[125] = {char(code >> 8), char(code)};
        const size_t n = reason ? strnlen(reason, sizeof(payload) - 2) : 0;
        if (n) memcpy(payload + 2, reason, n);
        esp_transport_ws_send_raw(ws_, static_cast<ws_transport_opcodes_t>(
            WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN), payload, int(n + 2), 100);
    }
    teardown();
    if (state_ != WssState::Failed) state_ = WssState::Closed;
}

void EspWssTransport::fail(WssError error) {
    error_ = error;
    teardown();
    state_ = WssState::Failed;
}

void EspWssTransport::teardown() {
    if (ws_) {
        esp_transport_close(ws_);
        esp_transport_destroy(ws_);
        ws_ = nullptr;
    }
    if (ssl_) {
        esp_transport_destroy(ssl_);
        ssl_ = nullptr;
    }
}

const char* EspWssTransport::lastErrorName() const {
    switch (error_) {
        case WssError::None: return "none";
        case WssError::Dns: return "dns";
        case WssError::TcpConnect: return "tcp_connect";
        case WssError::TlsHandshake: return "tls_handshake";
        case WssError::WebsocketHandshake: return "websocket_handshake";
        case WssError::PeerClosed: return "peer_closed";
        case WssError::ProtocolError: return "protocol_error";
        case WssError::LocalTimeout: return "not_allowed";
        case WssError::HeapExhausted: return "heap_exhausted";
    }
    return "unknown";
}

}}  // namespace opensmarttimer::relay
#endif
