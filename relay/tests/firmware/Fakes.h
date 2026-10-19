#pragma once

// Small test doubles for the four dependency interfaces
// CapabilityToken uses. All in-memory; no side effects.

#include "INvsStore.h"
#include "IClock.h"
#include "IEntropy.h"
#include "ITimerIdentity.h"

#include <map>
#include <string>
#include <vector>
#include <cstring>

namespace opensmarttimer { namespace relay { namespace test {

class FakeNvsStore : public INvsStore {
public:
    bool writes_ok = true;
    bool erases_ok = true;
    // Real ESP-IDF NVS caps namespace names at 15 chars (nvs.h).
    // Emulate that here so the F7 finding stays regression-locked.
    static bool nsNameOk(const char* ns) {
        return std::strlen(ns) > 0 && std::strlen(ns) <= 15;
    }

    bool readBlob(const char* ns, const char* key,
                  uint8_t* out, size_t* inout_len) override {
        if (!nsNameOk(ns)) return false;
        auto it = store_.find(std::string(ns) + "/" + key);
        if (it == store_.end()) return false;
        size_t want = it->second.size();
        if (want > *inout_len) return false;
        std::memcpy(out, it->second.data(), want);
        *inout_len = want;
        return true;
    }
    bool writeBlob(const char* ns, const char* key,
                   const uint8_t* data, size_t len) override {
        if (!nsNameOk(ns) || !writes_ok) return false;
        store_[std::string(ns) + "/" + key].assign(data, data + len);
        return true;
    }
    bool eraseKey(const char* ns, const char* key) override {
        if (!erases_ok) return false;
        store_.erase(std::string(ns) + "/" + key);
        return true;
    }
    bool eraseNamespace(const char* ns) override {
        if (!erases_ok) return false;
        std::string prefix = std::string(ns) + "/";
        for (auto it = store_.begin(); it != store_.end(); ) {
            if (it->first.rfind(prefix, 0) == 0) it = store_.erase(it);
            else ++it;
        }
        return true;
    }
    size_t bytesStored() const {
        size_t total = 0;
        for (const auto& kv : store_) total += kv.second.size();
        return total;
    }
private:
    std::map<std::string, std::vector<uint8_t>> store_;
};

class FakeClock : public IClock {
public:
    // A real clock moves between reads; step_per_read models that so code
    // that samples the clock twice around a nested call is exercised.
    uint32_t uptime_ms() override { now_ += step_per_read; return now_; }
    void advance(uint32_t ms) { now_ += ms; }
    void set(uint32_t ms) { now_ = ms; }
    uint32_t step_per_read = 0;
private:
    uint32_t now_ = 0;
};

class FakeEntropy : public IEntropy {
public:
    bool ok() override { return true; }
    void fill(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)((next_++) & 0xff);
    }
    void resetSeed(uint8_t seed = 0) { next_ = seed; }
private:
    uint32_t next_ = 0;
};

class FakeIdentity : public ITimerIdentity {
public:
    const char* timer_id() override        { return timer_id_; }
    const char* current_boot_id() override  { return boot_id_; }
    void setTimerId(const char* s)          { std::strncpy(timer_id_, s, sizeof(timer_id_) - 1); timer_id_[sizeof(timer_id_)-1] = '\0'; }
    void setBootId(const char* s)           { std::strncpy(boot_id_, s, sizeof(boot_id_) - 1); boot_id_[sizeof(boot_id_)-1] = '\0'; }
private:
    char timer_id_[24] = "timer_test_a";
    char boot_id_[24]  = "aabbccdd";
};

}}}  // namespace test / relay / opensmarttimer
