#pragma once

// ESP32 bindings for the relay's clock, entropy, storage and identity
// interfaces. Used only from the relay task.
#include <stddef.h>
#include <stdint.h>

#include "IClock.h"
#include "IEntropy.h"
#include "INvsStore.h"
#include "ITimerIdentity.h"

namespace opensmarttimer { namespace relay {

class EspClock : public IClock {
public:
    uint32_t uptime_ms() override;
};

// esp_fill_random is a true RNG while Wi-Fi or Bluetooth RF is running; the
// relay only runs with Wi-Fi up.
class EspEntropy : public IEntropy {
public:
    bool ok() override { return true; }
    void fill(uint8_t* out, size_t len) override;
};

class EspNvs : public INvsStore {
public:
    void setWriteGuard(bool (*guard)(void*), void* context) { guard_ = guard; context_ = context; }
    bool readBlob(const char* ns, const char* key, uint8_t* out, size_t* inout_len) override;
    bool writeBlob(const char* ns, const char* key, const uint8_t* data, size_t len) override;
    bool eraseKey(const char* ns, const char* key) override;
    bool eraseNamespace(const char* ns) override;
private:
    bool writable() const { return !guard_ || guard_(context_); }
    bool (*guard_)(void*) = nullptr;
    void* context_ = nullptr;
};

class RelayIdentity : public ITimerIdentity {
public:
    void setBootId(uint32_t boot_id);
    void setTimerId(const char* timer_id);
    const char* timer_id() override { return timer_id_; }
    const char* current_boot_id() override { return boot_id_; }
private:
    char timer_id_[24] = {};
    char boot_id_[12] = {};
};

}}  // namespace opensmarttimer::relay
