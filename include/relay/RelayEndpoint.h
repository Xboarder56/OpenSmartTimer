#pragma once

// Portable URL and enrollment-message helpers for the device relay adapter.
// Pure functions over caller buffers; host-tested (relay_endpoint_test).

#include <stddef.h>
#include <stdint.h>

#include "RelayClient.h"

namespace opensmarttimer { namespace relay {

struct RelayEndpoint {
    char host[64];
    uint16_t port;
    char path[96];
};

// "wss://host[:port]/path" (path defaults to "/"). Host: letters, digits,
// '-', '.'; no userinfo, query or fragment.
bool parseWssUrl(const char* url, RelayEndpoint* out);
// "https://host[:port]" with an optional trailing '/' and no path.
bool parseHttpsOrigin(const char* origin, RelayEndpoint* out);
// Origins compare by host (case insensitive) and effective port.
bool sameOrigin(const RelayEndpoint& a, const RelayEndpoint& b);
// Owner-approved trusted page, public routing ID in the path; relay address
// and complete capability stay in the fragment, never in server requests.
bool shareUrlFromWss(const char* partner_origin, const char* wss_url,
                     const char* public_id, const char* wire, char* out, size_t cap);

// POST /api/v1/enroll request with Content-Length and Connection: close.
// Returns the byte count written, or 0 if it does not fit.
size_t buildEnrollRequest(const RelayEndpoint& origin, const char* enrollment_code,
                          const char* hardware_model, const char* chip_family,
                          const char* build_id, char* out, size_t cap,
                          const char* device_name = nullptr);

enum class EnrollParseResult : uint8_t {
    Ok,
    Incomplete,        // headers or declared body not fully received
    HttpError,         // non-200 status; *http_status says which
    Malformed,         // bad headers, chunked encoding or JSON shape
    PinMismatch,       // relay returned a pin other than the owner-supplied one
};

// Parses a complete HTTP/1.1 response. The returned SPKI pin must equal
// expected_pin: the owner-supplied pin authenticated this TLS session, and a
// different pin could not later authenticate the WSS connection.
EnrollParseResult parseEnrollResponse(const char* response, size_t len,
                                      const uint8_t expected_pin[kSpkiPinBytes],
                                      EnrollmentCredentials* out, int* http_status);

}}  // namespace opensmarttimer::relay
