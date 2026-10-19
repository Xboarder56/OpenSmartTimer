#pragma once

#include <cstddef>
#include <cstdint>

// Small, allocation-free flat JSON reader. It validates the entire document
// before exposing any field; transport/form handling belongs to WebRequests.
class HttpRequestFields {
 public:
  static constexpr size_t kMaximumBodyBytes = 2048;
  static constexpr size_t kStorageBytes = 1024;
  static constexpr size_t kMaximumFields = 16;
  enum class Type : uint8_t { String, Integer, Boolean, Null };
  enum class Error : uint8_t { None, InvalidJson, TooLarge, DuplicateField, InvalidField, InvalidType };
  struct Field { uint16_t name; uint16_t value; Type type; };

  bool parse(const char* body, size_t length);
  bool validateForRoute(const char* route);
  void clear();
  Error error() const { return error_; }
  size_t count() const { return count_; }
  const Field& field(size_t index) const { return fields_[index]; }
  const char* name(const Field& field) const { return storage_ + field.name; }
  const char* value(const Field& field) const { return storage_ + field.value; }
  const Field* find(const char* name) const;
  static bool unsignedInteger(const char* text, uint32_t& value);
  static bool signedInteger(const char* text, int32_t& value);
  // True for a Host header no public website can own: an IP literal, a
  // single-label name or a name under a non-public suffix such as .local.
  // Rejects DNS-rebinding requests that carry an attacker's domain.
  static bool localHost(const char* host);
  // Applies one header line read by the V1 event-stream reader, which keeps
  // only the first `captured` bytes of a line `length` bytes long. A Host is
  // trusted only if it is the request's single, complete Host line.
  static void noteStreamHeaderLine(const char* line, size_t captured, size_t length,
                                   uint8_t& hostLines, bool& hostTrusted);

 private:
  bool fail(Error error);
  void whitespace();
  bool take(char character);
  bool append(char character);
  bool scalar(uint32_t value);
  bool hexQuad(uint32_t& value);
  bool string(uint16_t& offset);
  bool number(uint16_t& offset);
  const char* body_ = nullptr;
  size_t length_ = 0;
  size_t position_ = 0;
  size_t used_ = 0;
  size_t count_ = 0;
  Error error_ = Error::None;
  Field fields_[kMaximumFields]{};
  char storage_[kStorageBytes]{};
};
