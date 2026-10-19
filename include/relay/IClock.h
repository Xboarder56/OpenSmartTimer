#pragma once

// Monotonic uptime clock abstraction. The relay implementation never
// asks for wall-clock time — expiry is always measured against
// uptime_ms(). Provide any monotonic source (millis() on Arduino,
// esp_timer_get_time() / 1000 on ESP-IDF, std::chrono for host tests).
//
// Wraparound: uptime_ms() returns uint32_t. 32-bit ms wraps at 49.7d.
// RelayClient handles wraparound internally by treating uptimes as
// deltas from the last observed value; nevertheless, callers should
// prefer a 64-bit source when the platform offers one and downcast at
// the boundary.

#include <stdint.h>

namespace opensmarttimer { namespace relay {

class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t uptime_ms() = 0;
};

}  // namespace relay
}  // namespace opensmarttimer
