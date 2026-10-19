#include "RelayEndpoint.h"
#include "Crypto.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace opensmarttimer::relay;

namespace {
std::string b64(const uint8_t* data, size_t len) {
    char out[64]; assert(b64urlEncode(data, len, out, sizeof(out)) > 0); return out;
}
std::string response(int status, const std::string& body, const char* extra = "") {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\ncontent-type: application/json\r\n"
        + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
}  // namespace

int main() {
    RelayEndpoint e{};
    assert(parseWssUrl("wss://relay.example.com/api/v1/timer", &e));
    assert(!strcmp(e.host, "relay.example.com") && e.port == 443 && !strcmp(e.path, "/api/v1/timer"));
    assert(parseWssUrl("wss://10.0.0.5:8443", &e) && e.port == 8443 && !strcmp(e.path, "/"));
    for (const char* bad : {"ws://h/x", "wss://", "wss://h:0/", "wss://h:65536/", "wss://u@h/", "wss://h/a b",
                            "wss://h/x?q=1", "wss://h/x#f", "wss://h:/x", "https://h/"})
        assert(!parseWssUrl(bad, &e));
    assert(parseHttpsOrigin("https://relay.example.com", &e) && e.port == 443);
    assert(parseHttpsOrigin("https://relay.example.com:9443/", &e) && e.port == 9443);
    assert(!parseHttpsOrigin("https://relay.example.com/enroll", &e));
    assert(!parseHttpsOrigin("http://relay.example.com", &e));

    char url[700];
    const char* id = "AAAAAAAAAAAAAAAAAAAAAA";
    assert(shareUrlFromWss("https://relay.example.com", "wss://api.example.com/api/v1/timer", id, "ost-cap.v1.a.b", url, sizeof(url)));
    assert(!strcmp(url, "https://relay.example.com/s/AAAAAAAAAAAAAAAAAAAAAA#relay=aHR0cHM6Ly9hcGkuZXhhbXBsZS5jb20&cap=ost-cap.v1.a.b"));
    assert(shareUrlFromWss("https://partner.test:8444/", "wss://r.local:8443/api/v1/timer", id, "t", url, sizeof(url)));
    assert(!strcmp(url, "https://partner.test:8444/s/AAAAAAAAAAAAAAAAAAAAAA#relay=aHR0cHM6Ly9yLmxvY2FsOjg0NDM&cap=t"));
    assert(!shareUrlFromWss("https://PARTNER.test:443/", "wss://partner.test/api/v1/timer", id, "t", url, sizeof(url)));
    assert(!shareUrlFromWss("https://partner.test", "wss://r.local/", id, "t", url, 10));
    assert(!shareUrlFromWss("https://partner.test/path", "wss://r.local/", id, "t", url, sizeof(url)));
    assert(!shareUrlFromWss("https://partner.test", "wss://r.local/", "short", "t", url, sizeof(url)));
    assert(!shareUrlFromWss(nullptr, "wss://r.local/", id, "t", url, sizeof(url)));
    RelayEndpoint other_origin{};
    assert(parseHttpsOrigin("https://RELAY.example.com:443/", &other_origin));
    assert(parseHttpsOrigin("https://relay.example.com", &e) && sameOrigin(e, other_origin));
    other_origin.port = 8443;
    assert(!sameOrigin(e, other_origin));

    assert(parseHttpsOrigin("https://relay.example.com", &e));
    char request[1024];
    const size_t n = buildEnrollRequest(e, "code-\"1\"", "MagBound Smart Timer V2", "ESP32-C3",
                                        "aabbccddeeff00112233aabb", request, sizeof(request));
    assert(n && strstr(request, "POST /api/v1/enroll HTTP/1.1\r\nHost: relay.example.com\r\n"));
    const char* body = strstr(request, "\r\n\r\n") + 4;
    assert(strstr(request, ("Content-Length: " + std::to_string(strlen(body)) + "\r\n").c_str()));
    assert(strstr(body, "\"enrollment_code\":\"code-\\\"1\\\"\""));
    assert(!strstr(body, "\"device_name\"")); // older callers remain valid
    assert(buildEnrollRequest(e, "code", "MagBound Smart Timer V2", "ESP32-C3",
                              "aabbccddeeff00112233aabb", request, sizeof(request), "Kitchen \"A\"\\room"));
    body = strstr(request, "\r\n\r\n") + 4;
    assert(strstr(body, "\"device_name\":\"Kitchen \\\"A\\\"\\\\room\""));
    assert(strstr(request, ("Content-Length: " + std::to_string(strlen(body)) + "\r\n").c_str()));
    const std::string long_code(128, 'A'), full_name(32, '\"');
    assert(buildEnrollRequest(e, long_code.c_str(), "MagBound Smart Timer V2", "ESP32-C3",
                              "aabbccddeeff00112233aabb", request, 896, full_name.c_str()));
    body = strstr(request, "\r\n\r\n") + 4;
    assert(strstr(request, ("Content-Length: " + std::to_string(strlen(body)) + "\r\n").c_str()));
    assert(!buildEnrollRequest(e, "c", "m", "f", "b", request, 40));

    uint8_t secret[32], pin[32], other[32];
    for (int i = 0; i < 32; ++i) { secret[i] = uint8_t(i); pin[i] = uint8_t(100 + i); other[i] = 7; }
    const std::string good = "{\"timer_id\":\"abc_DEF-123\",\"enrollment_secret\":\"" + b64(secret, 32)
        + "\",\"wss_endpoint\":\"wss://relay.example.com/api/v1/timer\",\"spki_sha256_pin\":\"" + b64(pin, 32) + "\"}";
    EnrollmentCredentials cred{}; int status = 0;
    std::string r = response(200, good);
    assert(parseEnrollResponse(r.data(), r.size(), pin, &cred, &status) == EnrollParseResult::Ok && status == 200);
    assert(!strcmp(cred.timer_id, "abc_DEF-123") && !memcmp(cred.enrollment_secret, secret, 32));
    assert(!memcmp(cred.spki_pin_primary, pin, 32) && !strcmp(cred.wss_endpoint, "wss://relay.example.com/api/v1/timer"));
    // Partial bodies wait; pin substitution and malformed shapes fail.
    assert(parseEnrollResponse(r.data(), r.size() - 5, pin, &cred, &status) == EnrollParseResult::Incomplete);
    assert(parseEnrollResponse(r.data(), 20, pin, &cred, &status) == EnrollParseResult::Incomplete);
    assert(parseEnrollResponse(r.data(), r.size(), other, &cred, &status) == EnrollParseResult::PinMismatch);
    r = response(410, "{\"detail\":\"code_consumed\"}");
    assert(parseEnrollResponse(r.data(), r.size(), pin, &cred, &status) == EnrollParseResult::HttpError && status == 410);
    r = response(200, good, "Transfer-Encoding: chunked\r\n");
    assert(parseEnrollResponse(r.data(), r.size(), pin, &cred, &status) == EnrollParseResult::Malformed);
    std::string bad = good; bad.replace(bad.find("wss://"), 6, "ws://");
    r = response(200, bad);
    assert(parseEnrollResponse(r.data(), r.size(), pin, &cred, &status) == EnrollParseResult::Malformed);
    bad = good; bad.replace(bad.find("abc_DEF"), 3, "a/c");
    r = response(200, bad);
    assert(parseEnrollResponse(r.data(), r.size(), pin, &cred, &status) == EnrollParseResult::Malformed);
    std::puts("relay endpoint URL, share-link and enrollment message tests passed");
    return 0;
}
