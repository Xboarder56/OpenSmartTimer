#if defined(ESP_PLATFORM)
#include "EspEnroll.h"

#include "EspTls.h"
#include "RelayEndpoint.h"
#include "esp_timer.h"

namespace opensmarttimer { namespace relay {

namespace {
constexpr int kConnectTimeoutMs = 10000;
constexpr int kIoTimeoutMs = 5000;
constexpr int64_t kResponseDeadlineUs = 15LL * 1000 * 1000;

struct TransportGuard {
    esp_transport_handle_t handle;
    ~TransportGuard() {
        if (handle) { esp_transport_close(handle); esp_transport_destroy(handle); }
    }
};
}  // namespace

EnrollOutcome enrollOverHttps(const char* origin, const char* code, const uint8_t pin[kSpkiPinBytes],
                              const char* hardware_model, const char* chip_family,
                              const char* build_id, EnrollmentCredentials* out, int* http_status,
                              const char* device_name) {
    if (http_status) *http_status = 0;
    RelayEndpoint endpoint{};
    if (!parseHttpsOrigin(origin, &endpoint)) return EnrollOutcome::InvalidOrigin;
    char request[896];
    const size_t request_len = buildEnrollRequest(endpoint, code, hardware_model, chip_family, build_id,
                                                  request, sizeof(request), device_name);
    if (!request_len) return EnrollOutcome::InvalidOrigin;

    TlsPinPolicy policy{pin, false, false, {}};
    TransportGuard tls{newPinnedTlsTransport(&policy)};
    if (!tls.handle) return EnrollOutcome::HeapExhausted;
    if (esp_transport_connect(tls.handle, endpoint.host, endpoint.port, kConnectTimeoutMs) < 0)
        return policy.leaf_seen && !policy.pin_matched ? EnrollOutcome::PinMismatch : EnrollOutcome::TlsFailed;
    if (!policy.pin_matched) return EnrollOutcome::PinMismatch;
    if (esp_transport_write(tls.handle, request, int(request_len), kIoTimeoutMs) != int(request_len))
        return EnrollOutcome::TlsFailed;

    char response[2049];
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + kResponseDeadlineUs;
    while (esp_timer_get_time() < deadline) {
        const int n = esp_transport_read(tls.handle, response + received, int(sizeof(response) - 1 - received),
                                         kIoTimeoutMs);
        if (n > 0) received += size_t(n);
        response[received] = '\0';
        const EnrollParseResult parsed = parseEnrollResponse(response, received, pin, out, http_status);
        switch (parsed) {
            case EnrollParseResult::Ok: return EnrollOutcome::Ok;
            case EnrollParseResult::HttpError: return EnrollOutcome::HttpError;
            case EnrollParseResult::Malformed: return EnrollOutcome::Malformed;
            case EnrollParseResult::PinMismatch: return EnrollOutcome::PinMismatch;
            case EnrollParseResult::Incomplete: break;
        }
        if (n < 0 || received >= sizeof(response) - 1) return EnrollOutcome::Malformed;
    }
    return EnrollOutcome::Timeout;
}

const char* enrollOutcomeName(EnrollOutcome outcome) {
    switch (outcome) {
        case EnrollOutcome::Ok: return "enrolled";
        case EnrollOutcome::InvalidOrigin: return "invalid_relay_url";
        case EnrollOutcome::TlsFailed: return "enroll_tls_failed";
        case EnrollOutcome::PinMismatch: return "enroll_pin_mismatch";
        case EnrollOutcome::Timeout: return "enroll_timeout";
        case EnrollOutcome::HttpError: return "enroll_rejected";
        case EnrollOutcome::Malformed: return "enroll_malformed_response";
        case EnrollOutcome::HeapExhausted: return "heap_exhausted";
    }
    return "unknown";
}

}}  // namespace opensmarttimer::relay
#endif
