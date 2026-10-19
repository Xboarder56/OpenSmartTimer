#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// String-like buffer with bool reserve/concat. Sticky allocation/size failure:
// callers must not publish a partial object as a successful status response.
template <class Buffer>
class CheckedJsonWriter {
 public:
  explicit CheckedJsonWriter(Buffer& output, size_t limit = 2560)
      : output_(output), limit_(limit), ok_(output.reserve(limit < 1344 ? limit : 1344)) {
    raw("{");
  }
  bool good() const { return ok_; }
  bool finish() { raw("}"); return good(); }
  void text(const char* key, const char* value) { field(key); quoted(value); }
  void number(const char* key, uint32_t value) {
    field(key); char text[11]; snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value)); raw(text);
  }
  void signedNumber(const char* key, int32_t value) {
    field(key); char text[12]; snprintf(text, sizeof(text), "%ld", static_cast<long>(value)); raw(text);
  }
  void boolean(const char* key, bool value) { field(key); raw(value ? "true" : "false"); }
  void null(const char* key) { field(key); raw("null"); }
  void object(const char* key) { field(key); raw("{"); first_ = true; }
  void endObject() { raw("}"); first_ = false; }

 private:
  void field(const char* key) {
    if (!first_) raw(",");
    first_ = false; quoted(key); raw(":");
  }
  void raw(const char* value) {
    const size_t length = strlen(value);
    if (!ok_ || output_.length() > limit_ || length > limit_ - output_.length()) {
      ok_ = false; return;
    }
    ok_ = output_.concat(value, length);
  }
  void quoted(const char* value) {
    raw("\"");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); ok_ && *p; ++p) {
      if (*p == '"') raw("\\\"");
      else if (*p == '\\') raw("\\\\");
      else if (*p < 0x20) {
        char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", *p); raw(escaped);
      } else {
        const char character[] = {static_cast<char>(*p), 0}; raw(character);
      }
    }
    raw("\"");
  }
  Buffer& output_;
  size_t limit_;
  bool ok_;
  bool first_ = true;
};
