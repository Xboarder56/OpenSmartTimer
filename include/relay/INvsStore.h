#pragma once

// Small key/value blob store. Namespaces: relay.tokens, relay.capkey and
// relay.signkey (CapabilityToken) and relay.cfg (credentials and the enable
// flag, RelayRuntime). Results are never stored: the result ring is RAM only.
// On the timer this is EspNvs over ESP-IDF NVS.
//
// Semantics:
//
//   * writeBlob() MUST be atomic across power loss: the next readBlob()
//     sees either the old or the new bytes, never a torn mix. ESP-IDF's
//     nvs_set_blob + nvs_commit provides this; Arduino/EEPROM wrappers do
//     not and must not be used behind this interface.
//
//   * writeBlob() succeeds when the underlying storage confirms
//     durability. Returning true before a real sync will let the relay
//     acknowledge tokens the timer might not actually have on next boot.
//     The test double in tests/Fakes.h is honest about this because
//     it never persists.
//
//   * readBlob() MUST return false (not zero-length success) when the
//     key has never been written. Callers use that to distinguish
//     "no capability_key yet" from "capability_key is 32 zeros".
//
//   * On a torn-write recovery (both slots present but neither
//     verifiable by CRC), readBlob() MUST return false so the caller
//     regenerates from scratch rather than trusting garbage.

#include <stdint.h>
#include <stddef.h>

namespace opensmarttimer { namespace relay {

class INvsStore {
public:
    virtual ~INvsStore() = default;

    // Returns true if the key exists and the read succeeded. `out` is
    // filled up to `*inout_len`; on success `*inout_len` is set to the
    // number of bytes actually read.
    virtual bool readBlob(const char* ns, const char* key,
                          uint8_t* out, size_t* inout_len) = 0;

    // Durably store. Returns true after the flash-level commit.
    virtual bool writeBlob(const char* ns, const char* key,
                           const uint8_t* data, size_t len) = 0;

    // Erase a single key. Returns true even if it did not exist.
    virtual bool eraseKey(const char* ns, const char* key) = 0;

    // Erase every key in a namespace (used by wipe paths).
    virtual bool eraseNamespace(const char* ns) = 0;
};

}  // namespace relay
}  // namespace opensmarttimer
