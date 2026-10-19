#include "BleManagementProtocol.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace BleManagement;

namespace {
std::vector<uint8_t> frame(uint8_t opcode, uint16_t requestId, uint32_t boot,
                           uint32_t counter, const char* body,
                           size_t secretLength) {
  const size_t bodyLength = strlen(body);
  std::vector<uint8_t> out(kRequestHeaderBytes + bodyLength + secretLength + kMacBytes, 0);
  out[0] = kVersion;
  out[1] = opcode;
  writeLe16(out.data() + 2, requestId);
  writeLe32(out.data() + 4, boot);
  writeLe32(out.data() + 8, counter);
  writeLe16(out.data() + 12, static_cast<uint16_t>(bodyLength));
  out[14] = static_cast<uint8_t>(secretLength);
  memcpy(out.data() + kRequestHeaderBytes, body, bodyLength);
  for (size_t i = 0; i < secretLength; ++i) out[kRequestHeaderBytes + bodyLength + i] = 0xa0 + i;
  for (size_t i = 0; i < kMacBytes; ++i) out[out.size() - kMacBytes + i] = 0xee;
  return out;
}

std::vector<uint8_t> hex(const std::string& text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
  return out;
}

// Minimal extraction from the published JSON vectors: every string value of `key`.
std::vector<std::string> jsonStrings(const std::string& json, const std::string& key) {
  std::vector<std::string> values;
  const std::string marker = "\"" + key + "\"";
  for (size_t at = json.find(marker); at != std::string::npos; at = json.find(marker, at + 1)) {
    size_t cursor = json.find(':', at) + 1;
    while (json[cursor] == ' ' || json[cursor] == '\n') ++cursor;
    const bool array = json[cursor] == '[';
    const size_t stop = array ? json.find(']', cursor) : json.find('"', json.find('"', cursor) + 1) + 1;
    for (size_t quote = json.find('"', cursor); quote != std::string::npos && quote < stop;
         quote = json.find('"', json.find('"', quote + 1) + 1)) {
      const size_t close = json.find('"', quote + 1);
      values.push_back(json.substr(quote + 1, close - quote - 1));
      if (!array) break;
    }
  }
  return values;
}

void fragmentThrough(Assembler& assembler, const std::vector<uint8_t>& message, size_t payload) {
  uint8_t index = 0;
  for (size_t offset = 0; offset < message.size(); offset += payload, ++index) {
    const size_t take = std::min(payload, message.size() - offset);
    std::vector<uint8_t> write(kFragmentHeaderBytes + take);
    write[0] = index;
    write[1] = offset + take == message.size() ? 1 : 0;
    memcpy(write.data() + kFragmentHeaderBytes, message.data() + offset, take);
    const auto result = assembler.add(write.data(), write.size());
    assert(result == (write[1] ? Assembler::Result::Complete : Assembler::Result::Pending));
  }
}
}  // namespace

int main() {
  // A well-formed request exposes every field and the exact signed span.
  const auto wifi = frame(0x50, 7, 0x7ca9f21b, 42, "{\"ssid\":\"Home\"}", 12);
  Request request;
  assert(parseRequest(wifi.data(), wifi.size(), request));
  assert(request.opcode == 0x50 && request.requestId == 7);
  assert(request.bootId == 0x7ca9f21b && request.counter == 42);
  assert(request.bodyLength == 15 && request.secretLength == 12);
  assert(memcmp(request.body, "{\"ssid\":\"Home\"}", 15) == 0);
  assert(request.secret == request.body + 15 && request.secret[0] == 0xa0);
  assert(request.signedLength == kRequestHeaderBytes + 15 + 12);
  assert(request.mac == wifi.data() + request.signedLength);

  // Lengths must add up exactly; version, request ID, counter and secret size are checked.
  auto bad = wifi; bad.pop_back();
  assert(!parseRequest(bad.data(), bad.size(), request));
  bad = wifi; bad[0] = 2;
  assert(!parseRequest(bad.data(), bad.size(), request));
  bad = wifi; bad[2] = bad[3] = 0;
  assert(!parseRequest(bad.data(), bad.size(), request));
  bad = wifi; bad[8] = bad[9] = bad[10] = bad[11] = 0;
  assert(!parseRequest(bad.data(), bad.size(), request));
  const auto oversizeSecret = frame(0x50, 1, 1, 1, "{}", 65);
  assert(!parseRequest(oversizeSecret.data(), oversizeSecret.size(), request));
  std::vector<uint8_t> tooLong(kMaxFrameBytes + 1, 0);
  assert(!parseRequest(tooLong.data(), tooLong.size(), request));
  assert(!parseRequest(nullptr, 40, request));

  // A maximum firmware chunk fits in one frame.
  const size_t chunkFrame = kRequestHeaderBytes + 4 + 472 + kMacBytes;
  assert(chunkFrame <= kMaxFrameBytes);

  // Opcode classes.
  assert(knownOpcode(0x5b) && knownOpcode(0x5c));
  assert(mutatingOpcode(0x5b) && mutatingOpcode(0x5c));
  assert(!secretOpcode(0x5b) && !secretOpcode(0x5c));
  assert(knownOpcode(0x3f) && knownOpcode(0x47) && knownOpcode(0x5a) && knownOpcode(0x73));
  assert(!knownOpcode(0x48) && !knownOpcode(0x5d) && !knownOpcode(0x62) && !knownOpcode(0x74));
  assert(!mutatingOpcode(0x3f) && !mutatingOpcode(0x40) && !mutatingOpcode(0x47) && !mutatingOpcode(0x61));
  assert(mutatingOpcode(0x50) && mutatingOpcode(0x60) && mutatingOpcode(0x71));
  assert(secretOpcode(0x50) && secretOpcode(0x52) && !secretOpcode(0x53));

  // IV layout: LE32(counter) ‖ LE16(request id) ‖ zeros.
  uint8_t iv[16];
  secretIv(0x01020304, 0x0a0b, iv);
  const uint8_t expectedIv[16] = {4, 3, 2, 1, 0x0b, 0x0a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  assert(memcmp(iv, expectedIv, sizeof(iv)) == 0);

  // Fragments reassemble at small and large MTUs.
  std::vector<uint8_t> big(kMaxFrameBytes);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
  for (size_t payload : {18u, 123u, 242u, 510u}) {
    Assembler assembler;
    fragmentThrough(assembler, big, payload);
    assert(assembler.length() == big.size());
    assert(memcmp(assembler.data(), big.data(), big.size()) == 0);
  }
  {
    Assembler assembler;
    const uint8_t first[] = {0, 0, 1, 2};
    const uint8_t skipped[] = {2, 1, 3};
    assert(assembler.add(first, sizeof(first)) == Assembler::Result::Pending);
    assert(assembler.add(skipped, sizeof(skipped)) == Assembler::Result::SequenceError);
    assert(assembler.length() == 0);
    const uint8_t orphan[] = {1, 1, 9};
    assert(assembler.add(orphan, sizeof(orphan)) == Assembler::Result::SequenceError);
    const uint8_t badFlags[] = {0, 3, 1};
    assert(assembler.add(badFlags, sizeof(badFlags)) == Assembler::Result::SequenceError);
    const uint8_t restart[] = {0, 1, 5, 6};
    assert(assembler.add(first, sizeof(first)) == Assembler::Result::Pending);
    assert(assembler.add(restart, sizeof(restart)) == Assembler::Result::Complete);
    assert(assembler.length() == 2 && assembler.data()[0] == 5);  // Index 0 restarts.
    std::vector<uint8_t> overflow(kFragmentHeaderBytes + kMaxFrameBytes + 1, 0);
    assert(assembler.add(overflow.data(), overflow.size()) == Assembler::Result::SequenceError);
    const uint8_t tooMany[] = {32, 1, 0};
    assert(assembler.add(tooMany, sizeof(tooMany)) == Assembler::Result::SequenceError);
  }

  // Response header and ready frame layouts.
  uint8_t header[kResponseHeaderBytes];
  assert(writeResponseHeader(header, 0x44, 9, 200, 1500, 484, 484) == kResponseHeaderBytes);
  const uint8_t expectedHeader[] = {1, 0x44, 9, 0, 200, 0, 0xdc, 0x05, 0xe4, 0x01, 0xe4, 0x01};
  assert(memcmp(header, expectedHeader, sizeof(header)) == 0);
  uint8_t ready[kReadyFrameBytes];
  writeReadyFrame(ready, 0x50, 7, 202, 40);
  const uint8_t expectedReady[] = {1, 0x50, 7, 0, 202, 0, 40, 0};
  assert(memcmp(ready, expectedReady, sizeof(ready)) == 0);
  assert(kMaxSegmentBytes == 484);

  // Published golden vectors (tests/vectors/ost_ble_mgmt_v1.json): the
  // reference encoder's frames parse identically and its fragments reassemble.
  {
    std::string path = __FILE__;
    path = path.substr(0, path.rfind("/host/")) + "/vectors/ost_ble_mgmt_v1.json";
    std::ifstream file(path);
    assert(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string json = text.str();
    auto frames = jsonStrings(json, "frame_hex");  // Wi-Fi configure, Wi-Fi status.
    assert(frames.size() == 2);
    frames.push_back(jsonStrings(json, "continue_frame_hex").at(0));
    const auto wifiFrame = hex(frames[0]);
    Request vector;
    assert(parseRequest(wifiFrame.data(), wifiFrame.size(), vector));
    assert(vector.opcode == 0x50 && vector.requestId == 0x1234 && vector.counter == 41);
    assert(vector.bootId == 0x7ca9f21b && vector.secretLength == 28);
    assert(std::string(reinterpret_cast<const char*>(vector.body), vector.bodyLength) ==
           "{\"ssid\":\"Home Wi-Fi\"}");
    uint8_t vectorIv[16];
    secretIv(vector.counter, vector.requestId, vectorIv);
    assert(hex(jsonStrings(json, "iv_hex").at(0)) == std::vector<uint8_t>(vectorIv, vectorIv + 16));
    for (const char* key : {"fragments_mtu23_hex", "fragments_mtu247_hex"}) {
      Assembler assembler;
      const auto pieces = jsonStrings(json, key);
      assert(!pieces.empty());
      for (size_t i = 0; i < pieces.size(); ++i) {
        const auto piece = hex(pieces[i]);
        const auto result = assembler.add(piece.data(), piece.size());
        assert(result == (i + 1 == pieces.size() ? Assembler::Result::Complete : Assembler::Result::Pending));
      }
      assert(std::vector<uint8_t>(assembler.data(), assembler.data() + assembler.length()) == wifiFrame);
    }
    for (size_t i = 1; i < frames.size(); ++i) {
      const auto other = hex(frames[i]);
      assert(parseRequest(other.data(), other.size(), vector));
    }
    const auto segment = hex(jsonStrings(json, "segment0_hex").at(0));
    assert(segment.size() == kMaxFrameBytes && segment[0] == kVersion && segment[1] == 0x40);
    assert(readLe16(segment.data() + 10) == kMaxSegmentBytes);
  }

  std::cout << "BLE management framing, fragment, layout and vector tests passed\n";
}
