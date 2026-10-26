#include "UpdateFeedRecord.h"

#include <stdio.h>
#include <string.h>

namespace UpdateFeed {
namespace {

enum Field : uint8_t {
  kTarget = 1, kVersion = 2, kFile = 4, kCrc = 8, kSha = 16, kSignature = 32, kSize = 64,
};
constexpr uint8_t kAllFields = 127;

bool lowerHex(const char* text, size_t length) {
  if (strlen(text) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

void Parser::reset() { *this = Parser(); }

char* Parser::field(size_t& capacity) {
  struct Entry { const char* name; uint8_t bit; };
  static const Entry kFields[] = {
      {"target", kTarget}, {"release_version", kVersion}, {"file", kFile},
      {"image_crc32", kCrc}, {"image_sha256", kSha},
      {"release_signature", kSignature}, {"image_size", kSize},
  };
  current_ = 0;
  if (keyOverflow_) return nullptr;
  for (const Entry& entry : kFields) {
    if (strcmp(key_, entry.name) != 0) continue;
    current_ = entry.bit;
    switch (entry.bit) {
      case kTarget: capacity = sizeof(record_.target); return record_.target;
      case kVersion: capacity = sizeof(record_.version); return record_.version;
      case kFile: capacity = sizeof(record_.file); return record_.file;
      case kCrc: capacity = sizeof(record_.crc32); return record_.crc32;
      case kSha: capacity = sizeof(record_.sha256); return record_.sha256;
      case kSignature: capacity = sizeof(record_.signature); return record_.signature;
      default: return nullptr;  // image_size is a number
    }
  }
  return nullptr;
}

bool Parser::beginValue(char c) {
  if (current_ != 0 && (seen_ & current_)) return false;  // duplicate key
  if (c == '"') {
    if (current_ == kSize) return false;
    state_ = State::String;
    valueLength_ = 0;
    escape_ = false;
    return true;
  }
  if (c >= '0' && c <= '9') {
    if (current_ != 0 && current_ != kSize) return false;
    state_ = State::Number;
    numberIsSize_ = current_ == kSize;
    number_ = static_cast<uint32_t>(c - '0');
    numberDigits_ = 1;
    leadingZero_ = c == '0';
    return true;
  }
  if (c == 't' || c == 'f' || c == 'n') {
    if (current_ != 0) return false;
    state_ = State::Literal;
    return true;
  }
  return false;  // nested objects, arrays and signs are not part of the record
}

bool Parser::endString() {
  if (current_ != 0) {
    value_[valueLength_] = '\0';
    seen_ |= current_;
  }
  state_ = State::Comma;
  return true;
}

bool Parser::endNumber() {
  if (leadingZero_ && numberDigits_ > 1) return false;
  if (numberIsSize_) {
    record_.size = number_;
    seen_ |= kSize;
  }
  state_ = State::Comma;
  return true;
}

bool Parser::byte(char c) {
  const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
  switch (state_) {
    case State::Start:
      if (space) return true;
      if (c != '{') return false;
      state_ = State::KeyOrEnd;
      return true;
    case State::KeyOrEnd:
      if (space) return true;
      if (c == '}' && seen_ == 0 && keyLength_ == 0) {
        state_ = State::Done;
        return true;
      }
      if (c != '"') return false;
      state_ = State::Key;
      keyLength_ = 0;
      keyOverflow_ = false;
      key_[0] = '\0';
      return true;
    case State::Key:
      if (c == '"') {
        size_t capacity = 0;
        value_ = field(capacity);
        valueCapacity_ = capacity;
        state_ = State::Colon;
        return true;
      }
      if (c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
      if (keyLength_ + 1 < sizeof(key_)) {
        key_[keyLength_++] = c;
        key_[keyLength_] = '\0';
      } else {
        keyOverflow_ = true;  // longer than any wanted key: skip its value
      }
      return true;
    case State::Colon:
      if (space) return true;
      if (c != ':') return false;
      state_ = State::Value;
      return true;
    case State::Value:
      if (space) return true;
      return beginValue(c);
    case State::String:
      if (escape_) {
        escape_ = false;
        return current_ == 0;  // wanted fields never need escapes
      }
      if (c == '\\') {
        escape_ = true;
        return current_ == 0;
      }
      if (c == '"') return endString();
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (current_ != 0) {
        if (valueLength_ + 1 >= valueCapacity_) return false;
        value_[valueLength_++] = c;
      }
      return true;
    case State::Number:
      if (c >= '0' && c <= '9') {
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (numberIsSize_ && number_ > (UINT32_MAX - digit) / 10) return false;
        number_ = number_ * 10 + digit;
        ++numberDigits_;
        return true;
      }
      if (!numberIsSize_ && (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
        return true;  // an unwanted number's fraction or exponent
      if (!endNumber()) return false;
      return byte(c);
    case State::Literal:
      if (c >= 'a' && c <= 'z') return true;
      state_ = State::Comma;
      return byte(c);
    case State::Comma:
      if (space) return true;
      if (c == ',') {
        state_ = State::KeyOrEnd;
        keyLength_ = 1;  // a comma must be followed by another key
        return true;
      }
      if (c == '}') {
        state_ = State::Done;
        return true;
      }
      return false;
    case State::Done:
      return space;
    case State::Error:
      return false;
  }
  return false;
}

Result Parser::feed(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length && state_ != State::Error; ++i) {
    if (!byte(static_cast<char>(data[i]))) state_ = State::Error;
  }
  if (state_ == State::Error) return Result::Invalid;
  return state_ == State::Done ? Result::Complete : Result::Incomplete;
}

Result Parser::finish() const {
  if (state_ == State::Error) return Result::Invalid;
  if (state_ != State::Done || seen_ != kAllFields) return Result::Invalid;
  return Result::Complete;
}

bool valid(const Record& record, const char* target, const char* fileLabel) {
  if (strcmp(record.target, target) != 0) return false;
  if (record.size < 24 || record.size > 4194304) return false;
  if (!lowerHex(record.crc32, 8) || !lowerHex(record.sha256, 64) ||
      !lowerHex(record.signature, 128)) return false;
  char expected[sizeof(record.file)];
  const int length = snprintf(expected, sizeof(expected), "OpenSmartTimer-%s-%s.bin",
                              record.version, fileLabel);
  return length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
         strcmp(record.file, expected) == 0;
}

}  // namespace UpdateFeed
