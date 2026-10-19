#include "HttpRequestFields.h"
#include <cassert>
#include <initializer_list>
#include <cstring>
#include <iostream>
#include <string>

int main() {
  HttpRequestFields fields;
  auto parse = [&](const std::string& body) { return fields.parse(body.data(), body.size()); };
  for (const auto* body : {"", "[]", "null", "{", "{\"duration_ms\":1200",
       "{\"duration_ms\":1200} NOT_JSON", "{\"nested\":{\"duration_ms\":1200}}",
       "{\"duration_ms\":[1200]}", "{\"duration_ms\":1,}", "{\"a\":01}",
       "{\"a\":1.0}", "{\"a\":1e3}", "{\"a\":+1}", "{\"a\":truejunk}",
       "{\"a\":\"\\q\"}", "{\"a\":\"\\u0000\"}", "{\"a\":\"\\uD800\"}",
       "{\"a\":\"\\uDC00\"}", "{\"a\":\"line\nfeed\"}"}) {
    assert(!parse(body));
    assert(fields.find("duration_ms") == nullptr);
  }
  assert(!parse("{\"nested\":{\"if_boot_id\":\"current\"},\"if_boot_id\":\"stale\"}"));
  assert(!parse("{\"duration_ms\":1200,\"d\\u0075ration_ms\":2000}"));
  assert(fields.error() == HttpRequestFields::Error::DuplicateField);
  assert(parse("{\"replace_app_id\":2,\"label\":\"Phone\"}"));
  assert(fields.validateForRoute("/api/v1/bluetooth/pairing/start"));
  assert(parse("{\"app_id\":2,\"label\":\"Phone\"}"));
  assert(fields.validateForRoute("/api/v1/bluetooth/apps/label"));
  assert(parse("{\"app_id\":\"2\"}"));
  assert(!fields.validateForRoute("/api/v1/bluetooth/apps/revoke"));
  assert(parse("{\"app_id\":2,\"label\":\"Not allowed\"}"));
  assert(!fields.validateForRoute("/api/v1/bluetooth/apps/revoke"));
  // Relay setup and cancellation accept only their own typed fields.
  assert(parse("{\"relay_url\":\"https://r.example\",\"enrollment_code\":\"abcdefgh\",\"spki_pin\":\"x\",\"partner_origin\":\"https://control.example\",\"if_boot_id\":\"bcb49155\"}"));
  assert(fields.validateForRoute("/api/v1/relay/enroll"));
  for (const char* body : {"{\"partner_origin\":true}", "{\"if_boot_id\":1}"}) {
    assert(parse(body));
    assert(!fields.validateForRoute("/api/v1/relay/enroll"));
    assert(fields.error() == HttpRequestFields::Error::InvalidType);
  }
  assert(parse("{\"setup_request_id\":1,\"if_boot_id\":\"bcb49155\"}"));
  assert(fields.validateForRoute("/api/v1/relay/enroll/cancel"));
  for (const char* body : {"{\"setup_request_id\":\"1\"}", "{\"setup_request_id\":true}",
                           "{\"if_boot_id\":false}"}) {
    assert(parse(body));
    assert(!fields.validateForRoute("/api/v1/relay/enroll/cancel"));
    assert(fields.error() == HttpRequestFields::Error::InvalidType);
  }
  for (const char* body : {"{\"setup_request_id\":1,\"partner_origin\":\"https://other.example\"}",
                           "{\"setup_request_id\":1,\"enrollment_code\":\"abcdefgh\"}"}) {
    assert(parse(body));
    assert(!fields.validateForRoute("/api/v1/relay/enroll/cancel"));
    assert(fields.error() == HttpRequestFields::Error::InvalidField);
  }
  assert(parse("{\"relay_url\":\"https://r.example\",\"private_ca\":false}"));
  assert(!fields.validateForRoute("/api/v1/relay/enroll"));
  assert(parse("{\"ttl_s\":900,\"label\":\"Partner\"}"));
  assert(fields.validateForRoute("/api/v1/relay/links"));
  assert(parse("{\"ttl_s\":\"900\"}"));
  assert(!fields.validateForRoute("/api/v1/relay/links"));
  assert(parse("{\"job_id\":3,\"if_boot_id\":\"bcb49155\",\"ttl_s\":900}"));
  assert(fields.validateForRoute("/api/v1/relay/links"));
  assert(parse("{\"job_id\":\"3\",\"ttl_s\":900}"));
  assert(!fields.validateForRoute("/api/v1/relay/links"));
  assert(parse("{\"job_id\":4,\"if_boot_id\":\"bcb49155\",\"fp_hash\":\"AAAAAAAAAAAAAAAAAAAAAA\"}"));
  assert(fields.validateForRoute("/api/v1/relay/links/revoke"));
  assert(parse("{\"job_id\":5,\"if_boot_id\":\"bcb49155\",\"fp_hash\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"ttl_s\":3600}"));
  assert(fields.validateForRoute("/api/v1/relay/links/extend"));
  assert(parse("{\"job_id\":5,\"ttl_s\":\"3600\"}"));
  assert(!fields.validateForRoute("/api/v1/relay/links/extend"));
  assert(!fields.validateForRoute("/api/v1/relay/enable"));
  assert(parse(" \t{\"duration_ms\":1200,\"if_boot_id\":\"abc\",\"if_session_id\":null}\r\n"));
  assert(fields.validateForRoute("/api/v1/timer/start"));
  assert(strcmp(fields.value(*fields.find("duration_ms")), "1200") == 0);
  assert(parse("{\"duration_ms\":\"1200\"}"));
  assert(!fields.validateForRoute("/api/v1/timer/start"));
  assert(fields.error() == HttpRequestFields::Error::InvalidType);
  assert(parse("{\"duration_ms\":1200,\"if_boot_i\":\"abc\"}"));
  assert(!fields.validateForRoute("/api/v1/timer/start"));
  assert(fields.error() == HttpRequestFields::Error::InvalidField);
  assert(parse("{\"name\":\"\\u4e2d\\u6587 \\uD83D\\uDD12\"}"));
  assert(fields.validateForRoute("/api/v1/device/name"));
  assert(strcmp(fields.value(*fields.find("name")), u8"中文 🔒") == 0);
  assert(parse(u8"{\"name\":\"中文 🔒\"}"));
  assert(strcmp(fields.value(*fields.find("name")), u8"中文 🔒") == 0);
  assert(!parse(std::string("{\"name\":\"") + char(0xC0) + char(0x80) + "\"}"));
  assert(!parse(std::string("{\"name\":\"") + char(0xED) + char(0xA0) + char(0x80) + "\"}"));
  assert(!parse(std::string(2049, ' ')));
  assert(fields.error() == HttpRequestFields::Error::TooLarge);
  assert(!parse("{\"name\":\"" + std::string(1024, 'x') + "\"}"));
  assert(fields.error() == HttpRequestFields::Error::TooLarge);
  std::string many = "{";
  for (int i = 0; i < 17; ++i) many += (i ? "," : "") + std::string("\"k") + std::to_string(i) + "\":0";
  assert(!parse(many + "}"));
  assert(parse("{}"));
  assert(fields.count() == 0 && fields.find("name") == nullptr);
  assert(parse("{\"security\":\"wpa2\",\"use_device_name\":true,\"keep_password\":false}"));
  assert(fields.validateForRoute("/api/v1/wifi/ap"));
  uint32_t value = 0;
  for (const char* invalid : {"", "-1", "+1", " 1", "1 ", "1.0", "4294967296", "999999999999999999999999"})
    assert(!HttpRequestFields::unsignedInteger(invalid, value));
  assert(HttpRequestFields::unsignedInteger("4294967295", value) && value == UINT32_MAX);
  int32_t delta = 0;
  assert(HttpRequestFields::signedInteger("-2147483648", delta) && delta == INT32_MIN);
  assert(HttpRequestFields::signedInteger("2147483647", delta) && delta == INT32_MAX);
  assert(!HttpRequestFields::signedInteger("2147483648", delta));
  assert(!HttpRequestFields::signedInteger("-2147483649", delta));
  assert(!HttpRequestFields::signedInteger("--1", delta));
  // DNS rebinding: only names no public website can own are trusted.
  for (const char* ok : {"192.168.1.42", "192.168.1.42:80", "smarttimer", "smarttimer.local",
                         "SmartTimer.LOCAL.", "bedroom-timer.local:80", "timer.lan", "timer.home.arpa",
                         "timer.internal", "timer.localdomain", "[fe80::1]", "[fe80::1]:80"}) {
    assert(HttpRequestFields::localHost(ok));
  }
  for (const char* bad : {"", "evil.com", "smarttimer.evil.com", "smarttimer.local.evil.com",
                          "192.168.1.42.nip.io", "timer.local:8o", "timer local", ".local.com", "[x]evil.com", "[fe80::1]evil", "[]", "[fe80::1]:"}) {
    assert(!HttpRequestFields::localHost(bad));
  }
  // V1 event-stream Host lines: single, complete and local only.
  {
    auto feed = [](std::initializer_list<std::string> lines) {
      uint8_t count = 0; bool trusted = false;
      for (const std::string& full : lines) {
        const size_t captured = full.size() < 71 ? full.size() : 71;
        HttpRequestFields::noteStreamHeaderLine(full.substr(0, captured).c_str(), captured,
                                                full.size(), count, trusted);
      }
      return trusted;
    };
    assert(feed({"Host: 192.168.1.42"}));
    assert(feed({"Accept: text/event-stream", "host: SmartTimer.LOCAL:80"}));
    assert(!feed({"Accept: text/event-stream"}));                       // no Host
    assert(!feed({"Host: evil.example"}));
    assert(!feed({"Host: 192.168.1.42", "Host: evil.example"}));         // repeated
    assert(!feed({"Host: smarttimer.local", "Host: smarttimer.local"}));
    assert(!feed({"Host: " + std::string(61, 'a') + ".lan.attacker.example"}));  // cut off
    assert(feed({"Host: " + std::string(40, 'a') + ".lan"}));                    // fits
  }
  // Deterministic malformed-input sweep is also run under ASan/UBSan locally.
  uint32_t rng = 17;
  for (unsigned n = 0; n < 4000; ++n) {
    std::string bytes;
    for (unsigned i = 0; i < n % 73; ++i) { rng = rng * 1664525 + 1013904223; bytes += static_cast<char>(rng >> 24); }
    fields.parse(bytes.data(), bytes.size());
  }
  std::cout << "HTTP field parser tests passed\n";
}
