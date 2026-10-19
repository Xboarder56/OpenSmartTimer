#pragma once

// IWssTransport over esp_transport (TLS + WebSocket) for the relay task.
//
// RelayClient runs in the relay task and calls open()/poll()/sendJson()/
// close() there. open() only records the request; the task performs the
// blocking connect with performConnect() while it does not hold the relay
// mutex, so owner API calls never wait behind a TLS handshake.
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "EspTls.h"
#include "IWssTransport.h"
#include "RelayEndpoint.h"
#include "RelayEnvelope.h"

namespace opensmarttimer { namespace relay {

class EspWssTransport : public IWssTransport {
public:
    static constexpr size_t kMaxFrameBytes = 4096;

    void setFrameHandler(FrameHandler cb, void* ctx) override { handler_ = cb; handler_ctx_ = ctx; }
    bool open(const WssOpenParams& params) override;
    void poll() override;
    bool peerKeyDigest(uint8_t out[32]) const override {
        if (state_ != WssState::Open || !policy_.leaf_seen) return false;
        memcpy(out, policy_.leaf_digest, sizeof(policy_.leaf_digest));
        return true;
    }
    bool sendJson(const char* json_utf8, size_t len) override;
    void close(uint16_t code, const char* reason) override;
    WssState state() override { return state_; }
    WssError lastError() override { return error_; }
    uint32_t bytesSent() override { return bytes_sent_; }
    uint32_t bytesReceived() override { return bytes_received_; }

    // Relay-task control, outside the relay mutex.
    bool connectPending() const { return connect_pending_; }
    void performConnect();
    // Waits up to `ms` for inbound data; sleeps when not connected.
    void waitReadable(uint32_t ms);

    // While false (no Wi-Fi, or an OTA/restart/network transition owns the
    // device) open() fails and the task closes any live connection.
    void setAllowed(bool allowed) { allowed_ = allowed; }
    bool allowed() const { return allowed_; }
    const char* lastErrorName() const;

private:
    void teardown();
    void fail(WssError error);

    FrameHandler handler_ = nullptr;
    void* handler_ctx_ = nullptr;
    esp_transport_handle_t ssl_ = nullptr;
    esp_transport_handle_t ws_ = nullptr;
    RelayEndpoint endpoint_{};
    TlsPinPolicy policy_{};
    volatile bool connect_pending_ = false;
    std::atomic<bool> allowed_{false};
    WssState state_ = WssState::Idle;
    WssError error_ = WssError::None;
    uint32_t bytes_sent_ = 0;
    uint32_t bytes_received_ = 0;
    char frame_[kMaxFrameBytes + 1];
};

}}  // namespace opensmarttimer::relay
