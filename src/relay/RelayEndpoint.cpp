#include "RelayEndpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Crypto.h"
#include "RelayJson.h"

namespace opensmarttimer { namespace relay {

namespace {
bool hostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '.';
}

// Parses "host[:port]" up to `end`; defaults the port.
bool parseAuthority(const char* begin, const char* end, uint16_t default_port, RelayEndpoint* out) {
    const char* colon = static_cast<const char*>(memchr(begin, ':', size_t(end - begin)));
    const char* host_end = colon ? colon : end;
    const size_t host_len = size_t(host_end - begin);
    if (!host_len || host_len >= sizeof(out->host)) return false;
    for (const char* p = begin; p < host_end; ++p) if (!hostChar(*p)) return false;
    memcpy(out->host, begin, host_len);
    out->host[host_len] = '\0';
    out->port = default_port;
    if (colon) {
        const char* digits = colon + 1;
        if (digits == end || end - digits > 5) return false;
        uint32_t port = 0;
        for (const char* p = digits; p < end; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10 + uint32_t(*p - '0');
        }
        if (!port || port > 65535) return false;
        out->port = uint16_t(port);
    }
    return true;
}

const char* findHeaderEnd(const char* text, size_t len) {
    for (size_t i = 0; i + 3 < len; ++i)
        if (text[i] == '\r' && text[i + 1] == '\n' && text[i + 2] == '\r' && text[i + 3] == '\n')
            return text + i + 4;
    return nullptr;
}

bool headerEquals(const char* line, size_t len, const char* name, const char** value, size_t* value_len) {
    const size_t name_len = strlen(name);
    if (len <= name_len || line[name_len] != ':') return false;
    for (size_t i = 0; i < name_len; ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != name[i]) return false;
    }
    const char* v = line + name_len + 1;
    const char* end = line + len;
    while (v < end && (*v == ' ' || *v == '\t')) ++v;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *value = v;
    *value_len = size_t(end - v);
    return true;
}
}  // namespace

bool parseWssUrl(const char* url, RelayEndpoint* out) {
    if (!url || !out || strncmp(url, "wss://", 6) != 0) return false;
    *out = RelayEndpoint{};
    const char* authority = url + 6;
    const char* slash = strchr(authority, '/');
    const char* end = slash ? slash : authority + strlen(authority);
    if (!parseAuthority(authority, end, 443, out)) return false;
    const char* path = slash ? slash : "/";
    const size_t path_len = strlen(path);
    if (path_len >= sizeof(out->path)) return false;
    for (const char* p = path; *p; ++p)
        if (*p <= ' ' || *p == '?' || *p == '#' || *p == '\x7f') return false;
    memcpy(out->path, path, path_len + 1);
    return true;
}

bool parseHttpsOrigin(const char* origin, RelayEndpoint* out) {
    if (!origin || !out || strncmp(origin, "https://", 8) != 0) return false;
    *out = RelayEndpoint{};
    const char* authority = origin + 8;
    size_t len = strlen(authority);
    if (len && authority[len - 1] == '/') --len;
    if (memchr(authority, '/', len)) return false;
    if (!parseAuthority(authority, authority + len, 443, out)) return false;
    strcpy(out->path, "/");
    return true;
}

bool sameOrigin(const RelayEndpoint& a, const RelayEndpoint& b) {
    if (a.port != b.port) return false;
    const char* left = a.host;
    const char* right = b.host;
    for (; *left && *right; ++left, ++right) {
        const char l = *left >= 'A' && *left <= 'Z' ? char(*left + ('a' - 'A')) : *left;
        const char r = *right >= 'A' && *right <= 'Z' ? char(*right + ('a' - 'A')) : *right;
        if (l != r) return false;
    }
    return *left == *right;
}

bool shareUrlFromWss(const char* partner_origin, const char* wss_url,
                     const char* public_id, const char* wire, char* out, size_t cap) {
    RelayEndpoint endpoint{};
    uint8_t public_bytes[16];
    if (!wire || !*wire || !out || !public_id || strlen(public_id) != 22
        || b64urlDecode(public_id, 22, public_bytes, sizeof(public_bytes)) != 16
        || !parseWssUrl(wss_url, &endpoint)) return false;
    RelayEndpoint partner{};
    if (!parseHttpsOrigin(partner_origin, &partner) || sameOrigin(endpoint, partner)) return false;
    char relay[88],encoded[120];
    int n=endpoint.port==443
        ? snprintf(relay,sizeof(relay),"https://%s",endpoint.host)
        : snprintf(relay,sizeof(relay),"https://%s:%u",endpoint.host,unsigned(endpoint.port));
    if(n<=0 || size_t(n)>=sizeof(relay) || b64urlEncode((const uint8_t*)relay,size_t(n),encoded,sizeof(encoded))<=0) return false;
    char page[88];
    const int page_len = partner.port == 443
        ? snprintf(page, sizeof(page), "https://%s", partner.host)
        : snprintf(page, sizeof(page), "https://%s:%u", partner.host, unsigned(partner.port));
    if (page_len <= 0 || size_t(page_len) >= sizeof(page)) return false;
    n = snprintf(out, cap, "%s/s/%s#relay=%s&cap=%s", page, public_id, encoded, wire);
    return n > 0 && size_t(n) < cap;
}

size_t buildEnrollRequest(const RelayEndpoint& origin, const char* code,
                          const char* hardware_model, const char* chip_family,
                          const char* build_id, char* out, size_t cap,
                          const char* device_name) {
    char body[512];
    JsonWriter jw(body, sizeof(body));
    jw.beginObject();
    jw.key("enrollment_code"); jw.valueString(code);
    jw.key("hardware_model"); jw.valueString(hardware_model);
    jw.key("chip_family"); jw.valueString(chip_family);
    jw.key("build_id"); jw.valueString(build_id);
    if (device_name && *device_name) { jw.key("device_name"); jw.valueString(device_name); }
    jw.endObject();
    const size_t body_len = jw.finalize();
    if (!body_len) return 0;
    char host[80];
    if (origin.port == 443) snprintf(host, sizeof(host), "%s", origin.host);
    else snprintf(host, sizeof(host), "%s:%u", origin.host, unsigned(origin.port));
    const int n = snprintf(out, cap,
        "POST /api/v1/enroll HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Accept: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
        host, unsigned(body_len - 1), body);
    return n > 0 && size_t(n) < cap ? size_t(n) : 0;
}

EnrollParseResult parseEnrollResponse(const char* response, size_t len,
                                      const uint8_t expected_pin[kSpkiPinBytes],
                                      EnrollmentCredentials* out, int* http_status) {
    if (http_status) *http_status = 0;
    const char* body = findHeaderEnd(response, len);
    if (!body) return EnrollParseResult::Incomplete;
    if (len < 12 || strncmp(response, "HTTP/1.", 7) != 0 || response[8] != ' ')
        return EnrollParseResult::Malformed;
    const int status = atoi(response + 9);
    if (http_status) *http_status = status;
    long content_length = -1;
    for (const char* line = strstr(response, "\r\n") + 2; line < body - 2;) {
        const char* eol = strstr(line, "\r\n");
        if (!eol || eol > body - 2) break;
        const char* value; size_t value_len;
        if (headerEquals(line, size_t(eol - line), "transfer-encoding", &value, &value_len))
            return EnrollParseResult::Malformed;  // uvicorn sends Content-Length for JSON
        if (headerEquals(line, size_t(eol - line), "content-length", &value, &value_len)) {
            char digits[12];
            if (!value_len || value_len >= sizeof(digits)) return EnrollParseResult::Malformed;
            memcpy(digits, value, value_len); digits[value_len] = '\0';
            char* end = nullptr;
            content_length = strtol(digits, &end, 10);
            if (*end || content_length < 0 || content_length > 2048) return EnrollParseResult::Malformed;
        }
        line = eol + 2;
    }
    if (content_length < 0) return EnrollParseResult::Malformed;
    const size_t available = len - size_t(body - response);
    if (available < size_t(content_length)) return EnrollParseResult::Incomplete;
    if (status != 200) return EnrollParseResult::HttpError;

    const size_t body_len = size_t(content_length);
    EnrollmentCredentials cred{};
    char secret[48], pin[48];
    if (jsonCopyString(body, body_len, "timer_id", cred.timer_id, sizeof(cred.timer_id)) <= 0
        || jsonCopyString(body, body_len, "wss_endpoint", cred.wss_endpoint, sizeof(cred.wss_endpoint)) <= 0
        || jsonCopyString(body, body_len, "enrollment_secret", secret, sizeof(secret)) != 43
        || jsonCopyString(body, body_len, "spki_sha256_pin", pin, sizeof(pin)) != 43)
        return EnrollParseResult::Malformed;
    for (const char* p = cred.timer_id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
              || *p == '-' || *p == '_')) return EnrollParseResult::Malformed;
    RelayEndpoint wss{};
    if (!parseWssUrl(cred.wss_endpoint, &wss)
        || b64urlDecode(secret, 43, cred.enrollment_secret, sizeof(cred.enrollment_secret)) != 32
        || b64urlDecode(pin, 43, cred.spki_pin_primary, sizeof(cred.spki_pin_primary)) != 32)
        return EnrollParseResult::Malformed;
    if (constantTimeCompare(cred.spki_pin_primary, expected_pin, kSpkiPinBytes) != 0)
        return EnrollParseResult::PinMismatch;
    *out = cred;
    return EnrollParseResult::Ok;
}

}}  // namespace opensmarttimer::relay
