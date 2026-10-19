#pragma once

// One-shot HTTPS enrollment (POST /api/v1/enroll) for the relay task. The
// owner-supplied SPKI pin authenticates the enrollment server; the relay must
// return that same pin for the WebSocket.
#include <stdint.h>

#include "RelayClient.h"

namespace opensmarttimer { namespace relay {

enum class EnrollOutcome : uint8_t {
    Ok, InvalidOrigin, TlsFailed, PinMismatch, Timeout, HttpError, Malformed, HeapExhausted,
};

EnrollOutcome enrollOverHttps(const char* origin, const char* enrollment_code,
                              const uint8_t pin[kSpkiPinBytes],
                              const char* hardware_model, const char* chip_family,
                              const char* build_id, EnrollmentCredentials* out, int* http_status,
                              const char* device_name = nullptr);

const char* enrollOutcomeName(EnrollOutcome outcome);

}}  // namespace opensmarttimer::relay
