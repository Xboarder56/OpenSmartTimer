#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "UpdateFeedRecord.h"

namespace {

const std::string kSha(64, 'a');
const std::string kSig(128, 'b');

std::string record(const std::string& extra = "",
                   const std::string& file = "OpenSmartTimer-1.0.0-rc.3-MagBoundV2-ESP32C3.bin") {
  // Field order and compact separators match update_feed.py (sort_keys).
  return "{\"file\":\"" + file + "\",\"image_crc32\":\"0a1b2c3d\",\"image_sha256\":\"" + kSha +
         "\",\"image_size\":1257504,\"release_notes\":\"Line one\\nQuoted \\\"text\\\" and \\\\ "
         "\\u00e9 " + std::string(8000, 'x') + "\",\"release_signature\":\"" + kSig +
         "\",\"release_version\":\"1.0.0-rc.3\"" + extra + ",\"target\":\"magbound_v2_esp32c3\"}\n";
}

UpdateFeed::Result parse(const std::string& text, size_t chunk, UpdateFeed::Parser& parser) {
  parser.reset();
  for (size_t at = 0; at < text.size(); at += chunk) {
    const size_t n = std::min(chunk, text.size() - at);
    if (parser.feed(reinterpret_cast<const uint8_t*>(text.data() + at), n) ==
        UpdateFeed::Result::Invalid) return UpdateFeed::Result::Invalid;
  }
  return parser.finish();
}

bool rejected(const std::string& text) {
  UpdateFeed::Parser parser;
  return parse(text, 7, parser) == UpdateFeed::Result::Invalid;
}

}  // namespace

int main() {
  // The real record, delivered in every chunk size a TLS read might give.
  const std::string good = record();
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{64}, size_t{1436}, good.size()}) {
    UpdateFeed::Parser parser;
    assert(parse(good, chunk, parser) == UpdateFeed::Result::Complete);
    const UpdateFeed::Record& r = parser.record();
    assert(std::string(r.target) == "magbound_v2_esp32c3");
    assert(std::string(r.version) == "1.0.0-rc.3");
    assert(r.size == 1257504);
    assert(std::string(r.crc32) == "0a1b2c3d");
    assert(std::string(r.sha256) == kSha && std::string(r.signature) == kSig);
    assert(UpdateFeed::valid(r, "magbound_v2_esp32c3", "MagBoundV2-ESP32C3"));
    assert(!UpdateFeed::valid(r, "magbound_v1_esp8266", "MagBoundV1-ESP8266"));
  }

  // Unknown keys of any simple type are skipped.
  assert(!rejected(record(",\"z_extra\":true,\"z_null\":null,\"z_n\":12.5e3")));
  assert(rejected(record(",\"z_count\":-1")));  // the record never carries signs

  // Structure errors.
  assert(rejected(""));
  assert(rejected("[]"));
  assert(rejected(good.substr(0, good.size() - 3)));                 // truncated
  assert(rejected(good + "{}"));                                     // trailing data
  assert(rejected(record(",\"z\":{\"a\":1}")));                      // nested object
  assert(rejected(record(",\"z\":[1]")));                            // array
  assert(rejected(record(",\"release_version\":\"1.0.0\"")));        // duplicate key
  assert(rejected("{\"target\":\"magbound_v2_esp32c3\"}"));          // missing fields
  assert(rejected(record(",")));                                     // trailing comma
  {
    std::string tampered = good;
    tampered.replace(tampered.find("1257504"), 7, "0125750");        // leading zero
    assert(rejected(tampered));
    tampered = good;
    tampered.replace(tampered.find("1257504"), 7, "\"12575\"");      // size as a string
    assert(rejected(tampered));
    tampered = good;
    tampered.replace(tampered.find("1257504"), 7, "99999999999");    // overflow
    assert(rejected(tampered));
    tampered = good;
    tampered.replace(tampered.find("rc.3\""), 4, "rc\\.3");          // escape in a wanted field
    assert(rejected(tampered));
    tampered = good;
    tampered.replace(tampered.find(kSig), kSig.size(), kSig + "b");  // value too long
    assert(rejected(tampered));
  }

  // Well-formed but not this model's update.
  {
    UpdateFeed::Parser parser;
    assert(parse(record("", "OpenSmartTimer-1.0.0-rc.2-MagBoundV2-ESP32C3.bin"), 64, parser) ==
           UpdateFeed::Result::Complete);
    assert(!UpdateFeed::valid(parser.record(), "magbound_v2_esp32c3", "MagBoundV2-ESP32C3"));
    std::string upper = record();
    upper.replace(upper.find("0a1b2c3d"), 8, "0A1B2C3D");
    assert(parse(upper, 64, parser) == UpdateFeed::Result::Complete);
    assert(!UpdateFeed::valid(parser.record(), "magbound_v2_esp32c3", "MagBoundV2-ESP32C3"));
    std::string tiny = record();
    tiny.replace(tiny.find("1257504"), 7, "23");
    assert(parse(tiny, 64, parser) == UpdateFeed::Result::Complete);
    assert(!UpdateFeed::valid(parser.record(), "magbound_v2_esp32c3", "MagBoundV2-ESP32C3"));
  }

  std::cout << "update feed record tests passed\n";
  return 0;
}
