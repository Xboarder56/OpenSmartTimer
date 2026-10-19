#pragma once

#include <stdint.h>
#include <string.h>

namespace opensmarttimer { namespace relay {

// Loop-owned physical approval of one immutable, locally submitted setup.
// Relay responses never enter this record. No pre-armed authorization window.
class RelaySetupApproval {
public:
    static constexpr uint32_t kWindowMs = 60000;
    struct Request {
        char relay_origin[80]{};
        char partner_origin[96]{};
        char enrollment_code[129]{};
        uint8_t pin[32]{};
    };
    enum class Stage { Pending, Replay, Busy };

    Stage stage(const Request& request, uint32_t now) {
        expire(now);
        if (active_) return equal(request, request_) ? Stage::Replay : Stage::Busy;
        request_ = request;
        deadline_ = now + kWindowMs;
        if (++id_ == 0) ++id_;
        active_ = true;
        return Stage::Pending;
    }
    bool pending(uint32_t now) const {
        return active_ && static_cast<int32_t>(deadline_ - now) > 0;
    }
    uint32_t remaining(uint32_t now) const { return pending(now) ? deadline_ - now : 0; }
    uint32_t id() const { return id_; }
    const Request& request() const { return request_; }
    void expire(uint32_t now) { if (active_ && !pending(now)) clear(); }
    bool cancel(uint32_t id) { if (!active_ || id != id_) return false; clear(); return true; }
    bool approve(uint32_t now, bool timer_open, bool transitions_clear, Request* out) {
        expire(now);
        if (!pending(now) || !timer_open || !transitions_clear || !out) {
            if (!timer_open || !transitions_clear) clear();
            return false;
        }
        *out = request_;
        clear();
        return true;
    }
    void clear() {
        volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&request_);
        for (size_t i = 0; i < sizeof(request_); ++i) bytes[i] = 0;
        active_ = false;
        deadline_ = 0;
    }
private:
    static bool equal(const Request& a, const Request& b) {
        return !strcmp(a.relay_origin, b.relay_origin)
            && !strcmp(a.partner_origin, b.partner_origin)
            && !strcmp(a.enrollment_code, b.enrollment_code)
            && !memcmp(a.pin, b.pin, sizeof(a.pin));
    }
    Request request_{};
    uint32_t deadline_ = 0;
    uint32_t id_ = 0;
    bool active_ = false;
};

}}  // namespace opensmarttimer::relay
