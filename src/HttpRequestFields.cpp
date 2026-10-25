#include "HttpRequestFields.h"

#include <cstring>
#include <strings.h>
#include <limits>

void HttpRequestFields::clear() {
  // Request fields can contain Wi-Fi credentials. Do not retain them after use.
  memset(storage_, 0, sizeof(storage_));
  count_ = used_ = position_ = length_ = 0;
  body_ = nullptr;
  error_ = Error::None;
}

bool HttpRequestFields::fail(Error error) {
  if (error_ == Error::None) error_ = error;
  return false;
}

void HttpRequestFields::whitespace() {
  while (position_ < length_ && (body_[position_] == ' ' ||
         body_[position_] == '\n' || body_[position_] == '\r' ||
         body_[position_] == '\t')) ++position_;
}

bool HttpRequestFields::take(char character) {
  if (position_ >= length_ || body_[position_] != character) return false;
  ++position_;
  return true;
}

bool HttpRequestFields::append(char character) {
  if (used_ == sizeof(storage_)) return fail(Error::TooLarge);
  storage_[used_++] = character;
  return true;
}

bool HttpRequestFields::scalar(uint32_t value) {
  // NUL cannot be represented by the downstream C-string/Arduino APIs.
  if (value == 0 || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) return fail(Error::InvalidJson);
  if (value < 0x80) return append(static_cast<char>(value));
  if (value < 0x800) {
    return append(static_cast<char>(0xC0 | (value >> 6))) &&
           append(static_cast<char>(0x80 | (value & 63)));
  }
  if (value < 0x10000) {
    return append(static_cast<char>(0xE0 | (value >> 12))) &&
           append(static_cast<char>(0x80 | ((value >> 6) & 63))) &&
           append(static_cast<char>(0x80 | (value & 63)));
  }
  return append(static_cast<char>(0xF0 | (value >> 18))) &&
         append(static_cast<char>(0x80 | ((value >> 12) & 63))) &&
         append(static_cast<char>(0x80 | ((value >> 6) & 63))) &&
         append(static_cast<char>(0x80 | (value & 63)));
}

bool HttpRequestFields::hexQuad(uint32_t& value) {
  value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (position_ == length_) return fail(Error::InvalidJson);
    const char c = body_[position_++];
    const int digit = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return fail(Error::InvalidJson);
    value = (value << 4) | static_cast<uint32_t>(digit);
  }
  return true;
}

bool HttpRequestFields::string(uint16_t& offset) {
  if (!take('"')) return fail(Error::InvalidJson);
  offset = static_cast<uint16_t>(used_);
  while (position_ < length_) {
    uint32_t c = static_cast<uint8_t>(body_[position_++]);
    if (c == '"') return append('\0');
    if (c < 0x20) return fail(Error::InvalidJson);
    if (c == '\\') {
      if (position_ == length_) return fail(Error::InvalidJson);
      c = static_cast<uint8_t>(body_[position_++]);
      switch (c) {
        case '"': case '\\': case '/': break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
          if (!hexQuad(c)) return false;
          if (c >= 0xD800 && c <= 0xDBFF) {
            uint32_t low;
            if (!take('\\') || !take('u') || !hexQuad(low) ||
                low < 0xDC00 || low > 0xDFFF) return fail(Error::InvalidJson);
            c = 0x10000 + ((c - 0xD800) << 10) + low - 0xDC00;
          }
          break;
        }
        default: return fail(Error::InvalidJson);
      }
    } else if (c >= 0x80) {
      // Validate unescaped UTF-8 as well as JSON Unicode escapes.
      unsigned continuation;
      uint32_t minimum;
      if (c >= 0xC2 && c <= 0xDF) { continuation = 1; minimum = 0x80; c &= 31; }
      else if (c >= 0xE0 && c <= 0xEF) { continuation = 2; minimum = 0x800; c &= 15; }
      else if (c >= 0xF0 && c <= 0xF4) { continuation = 3; minimum = 0x10000; c &= 7; }
      else return fail(Error::InvalidJson);
      while (continuation--) {
        if (position_ == length_) return fail(Error::InvalidJson);
        const uint8_t next = static_cast<uint8_t>(body_[position_++]);
        if ((next & 0xC0) != 0x80) return fail(Error::InvalidJson);
        c = (c << 6) | (next & 63);
      }
      if (c < minimum) return fail(Error::InvalidJson);
    }
    if (!scalar(c)) return false;
  }
  return fail(Error::InvalidJson);
}

bool HttpRequestFields::number(uint16_t& offset) {
  offset = static_cast<uint16_t>(used_);
  if (take('-') && !append('-')) return false;
  if (position_ == length_ || body_[position_] < '0' || body_[position_] > '9')
    return fail(Error::InvalidJson);
  const bool zero = body_[position_] == '0';
  if (!append(body_[position_++])) return false;
  while (position_ < length_ && body_[position_] >= '0' && body_[position_] <= '9') {
    if (zero) return fail(Error::InvalidJson);
    if (!append(body_[position_++])) return false;
  }
  // The API uses integers exclusively. Fractions/exponents are rejected,
  // never silently rounded or accepted as integer prefixes.
  return append('\0');
}

const HttpRequestFields::Field* HttpRequestFields::find(const char* key) const {
  if (error_ != Error::None) return nullptr;
  for (size_t i = 0; i < count_; ++i)
    if (strcmp(name(fields_[i]), key) == 0) return &fields_[i];
  return nullptr;
}

bool HttpRequestFields::parse(const char* body, size_t length) {
  clear();
  if (length > kMaximumBodyBytes) return fail(Error::TooLarge);
  if (body == nullptr) return fail(Error::InvalidJson);
  body_ = body; length_ = length;
  whitespace();
  if (!take('{')) return fail(Error::InvalidJson);
  whitespace();
  if (!take('}')) {
    for (;;) {
      if (count_ == kMaximumFields) return fail(Error::TooLarge);
      Field next{};
      if (!string(next.name)) return false;
      if (find(name(next))) return fail(Error::DuplicateField);
      whitespace();
      if (!take(':')) return fail(Error::InvalidJson);
      whitespace();
      if (position_ == length_) return fail(Error::InvalidJson);
      if (body_[position_] == '"') {
        next.type = Type::String;
        if (!string(next.value)) return false;
      } else if (body_[position_] == '-' ||
                 (body_[position_] >= '0' && body_[position_] <= '9')) {
        next.type = Type::Integer;
        if (!number(next.value)) return false;
      } else {
        const char* literal = body_[position_] == 't' ? "true" :
            body_[position_] == 'f' ? "false" : "null";
        const size_t size = strlen(literal);
        if (size > length_ - position_ || memcmp(body_ + position_, literal, size))
          return fail(Error::InvalidJson);
        next.type = literal[0] == 'n' ? Type::Null : Type::Boolean;
        next.value = static_cast<uint16_t>(used_);
        if (next.type != Type::Null)
          for (size_t i = 0; i < size; ++i) if (!append(literal[i])) return false;
        if (!append('\0')) return false;
        position_ += size;
      }
      fields_[count_++] = next;
      whitespace();
      if (take('}')) break;
      if (!take(',')) return fail(Error::InvalidJson);
      whitespace();
    }
  }
  whitespace();
  if (position_ != length_) return fail(Error::InvalidJson);
  body_ = nullptr;
  return true;
}

namespace {
bool endsWithIgnoringCase(const char* text, size_t length, const char* suffix) {
  const size_t suffixLength = strlen(suffix);
  if (length < suffixLength) return false;
  for (size_t i = 0; i < suffixLength; ++i) {
    char c = text[length - suffixLength + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}
}  // namespace

bool HttpRequestFields::localHost(const char* host) {
  if (host == nullptr || *host == '\0') return false;
  size_t length = strlen(host);
  if (length > 253) return false;
  if (host[0] == '[') {  // IPv6 literal, optionally with :port
    const char* close = strchr(host, ']');
    if (close == nullptr || close == host + 1) return false;
    for (const char* p = host + 1; p < close; ++p) {
      const char c = *p;
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return false;
    }
    if (close[1] == '\0') return true;
    if (close[1] != ':' || close[2] == '\0') return false;
    for (const char* p = close + 2; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
  }
  // Drop an optional :port and one trailing dot.
  const char* colon = strchr(host, ':');
  if (colon != nullptr) {
    for (const char* p = colon + 1; *p; ++p) if (*p < '0' || *p > '9') return false;
    length = static_cast<size_t>(colon - host);
  }
  if (length > 0 && host[length - 1] == '.') --length;
  if (length == 0) return false;
  bool dot = false, digitsAndDotsOnly = true;
  for (size_t i = 0; i < length; ++i) {
    const char c = host[i];
    if (c == '.') dot = true;
    else if (c < '0' || c > '9') digitsAndDotsOnly = false;
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!allowed) return false;
  }
  if (!dot || digitsAndDotsOnly) return true;  // single label or IPv4 literal
  static const char* const kPrivateSuffixes[] = {".local", ".lan", ".home.arpa", ".internal", ".localdomain", ".home"};
  for (const char* suffix : kPrivateSuffixes) {
    if (endsWithIgnoringCase(host, length, suffix)) return true;
  }
  return false;
}

void HttpRequestFields::noteStreamHeaderLine(const char* line, size_t captured, size_t length,
                                             uint8_t& hostLines, bool& hostTrusted) {
  if (captured < 5 || strncasecmp(line, "host:", 5) != 0) return;
  const char* host = line + 5;
  while (*host == ' ' || *host == '\t') ++host;
  // A cut-off line is untrusted: "aaa...a.lan.attacker.example" truncated
  // could otherwise read as a local ".lan" name.
  const bool complete = length <= captured;
  if (hostLines < 255) ++hostLines;
  hostTrusted = hostLines == 1 && complete && localHost(host);
}

bool HttpRequestFields::unsignedInteger(const char* text, uint32_t& value) {
  value = 0;
  if (!text || !*text) return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*text - '0');
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

bool HttpRequestFields::signedInteger(const char* text, int32_t& value) {
  if (!text) return false;
  const bool negative = *text == '-';
  uint32_t magnitude;
  if (!unsignedInteger(text + (negative ? 1 : 0), magnitude) ||
      magnitude > (negative ? uint32_t{2147483648UL} : uint32_t{2147483647UL}))
    return false;
  value = negative ? static_cast<int32_t>(-static_cast<int64_t>(magnitude)) :
                     static_cast<int32_t>(magnitude);
  return true;
}

bool HttpRequestFields::validateForRoute(const char* route) {
  if (error_ != Error::None || !route) return false;
  const bool timer = strncmp(route, "/api/v1/timer/", 14) == 0;
  for (size_t i = 0; i < count_; ++i) {
    const Field& item = fields_[i];
    const char* key = name(item);
    auto is = [key](const char* wanted) { return strcmp(key, wanted) == 0; };
    auto at = [route](const char* wanted) { return strcmp(route, wanted) == 0; };
    bool allowed = timer && (is("operation_id") || is("if_boot_id") || is("if_session_id"));
    Type expected = Type::String;
    if (at("/api/v1/timer/start") && (is("duration_ms") || is("min_ms") || is("max_ms"))) {
      allowed = true; expected = Type::Integer;
    } else if (at("/api/v1/timer/adjust") && is("delta_ms")) {
      allowed = true; expected = Type::Integer;
    } else if (at("/api/v1/timer/seal") && is("sealed")) {
      allowed = true; expected = Type::Boolean;
    } else if (at("/api/v1/timer/controls") && (is("allow_pause") || is("allow_control_lock"))) {
      allowed = true; expected = Type::Boolean;
    } else if (at("/api/v1/wifi/configure") && (is("ssid") || is("password"))) {
      allowed = true;
    } else if (at("/api/v1/wifi/ap")) {
      if (is("ssid") || is("password") || is("security")) allowed = true;
      if (is("use_device_name") || is("keep_password") || is("standalone")) {
        allowed = true; expected = Type::Boolean;
      }
    } else if (at("/api/v1/device/name") && is("name")) {
      allowed = true;
    } else if (at("/api/v1/bluetooth/pairing/start")) {
      if (is("label")) allowed = true;
      if (is("replace_app_id")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/bluetooth/apps/label") || at("/api/v1/bluetooth/apps/revoke")) {
      if (is("app_id")) { allowed = true; expected = Type::Integer; }
      if (at("/api/v1/bluetooth/apps/label") && is("label")) allowed = true;
    } else if (at("/api/v1/ota/download")) {
      if (is("release_version")) allowed = true;
    } else if (at("/api/v1/ota/prepare") || at("/api/v1/ota")) {
      if (is("image_crc32") || is("release_version") || is("image_sha256") ||
          is("release_signature")) allowed = true;
      if (is("image_size")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/relay/enroll")) {
      if (is("relay_url") || is("enrollment_code") || is("spki_pin") ||
          is("partner_origin") || is("if_boot_id")) allowed = true;
    } else if (at("/api/v1/relay/enroll/cancel")) {
      if (is("if_boot_id")) allowed = true;
      if (is("setup_request_id")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/relay/links")) {
      if (is("label") || is("if_boot_id")) allowed = true;
      if (is("ttl_s") || is("job_id")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/relay/links/revoke")) {
      if (is("fp_hash") || is("if_boot_id")) allowed = true;
      if (is("job_id")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/relay/links/extend")) {
      if (is("fp_hash") || is("if_boot_id")) allowed = true;
      if (is("job_id") || is("ttl_s")) { allowed = true; expected = Type::Integer; }
    } else if (at("/api/v1/events")) {
      if (is("boot_id")) allowed = true;
      if (is("after")) { allowed = true; expected = Type::Integer; }
    }
    if (!allowed) return fail(Error::InvalidField);
    if (timer && is("if_session_id") && item.type == Type::Null) continue;
    if (item.type != expected) return fail(Error::InvalidType);
  }
  return true;
}
