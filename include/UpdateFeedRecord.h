#pragma once

#include <stddef.h>
#include <stdint.h>

// One model's signed update record, `updates/<target>.json` on the update
// site: a flat JSON object written by scripts/release/update_feed.py.
// The timer downloads it in pieces, so this parser takes bytes as they
// arrive and keeps only the fields it needs; release_notes (up to 8 KB) is
// skipped. The record is untrusted until ReleaseUpdate::verify() accepts its
// signature; these checks only reject malformed input early.
namespace UpdateFeed {

struct Record {
  char target[24]{};
  char version[48]{};
  char file[96]{};
  char crc32[9]{};
  char sha256[65]{};
  char signature[129]{};
  uint32_t size = 0;
};

enum class Result : uint8_t { Incomplete, Complete, Invalid };

class Parser {
 public:
  void reset();
  // Accepts the next bytes; Invalid is final until reset().
  Result feed(const uint8_t* data, size_t length);
  // Call after the last byte: Complete only for one whole object with every
  // required field.
  Result finish() const;
  const Record& record() const { return record_; }

 private:
  enum class State : uint8_t {
    Start, KeyOrEnd, Key, Colon, Value, String, Number, Literal, Comma, Done, Error
  };
  bool byte(char c);
  bool beginValue(char c);
  bool endString();
  bool endNumber();
  char* field(size_t& capacity);

  Record record_{};
  State state_ = State::Start;
  char key_[24]{};
  size_t keyLength_ = 0;
  bool keyOverflow_ = false;
  bool escape_ = false;
  char* value_ = nullptr;   // collected field, or nullptr when skipping
  size_t valueCapacity_ = 0;
  size_t valueLength_ = 0;
  uint32_t number_ = 0;
  bool numberIsSize_ = false;
  size_t numberDigits_ = 0;
  bool leadingZero_ = false;
  uint8_t seen_ = 0;        // bit per collected field
  uint8_t current_ = 0;     // bit of the field being read, 0 if skipped
};

// The browser's checks (web/updates.js validate()): this model's target,
// a 24 B–4 MB image, lowercase hex digests and the expected file name.
bool valid(const Record& record, const char* target, const char* fileLabel);

}  // namespace UpdateFeed
