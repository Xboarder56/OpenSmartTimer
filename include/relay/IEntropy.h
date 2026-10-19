#pragma once

// Cryptographic RNG source. Two calls: fill() fills a buffer, ok()
// reports whether the source has been through its bringup entropy
// gathering (RelayClient refuses to generate capability_key or
// enrollment_secret unless ok() is true).

#include <stdint.h>
#include <stddef.h>

namespace opensmarttimer { namespace relay {

class IEntropy {
public:
    virtual ~IEntropy() = default;
    virtual bool ok() = 0;                     // >=128 bits gathered
    virtual void fill(uint8_t* out, size_t len) = 0;
};

}  // namespace relay
}  // namespace opensmarttimer
