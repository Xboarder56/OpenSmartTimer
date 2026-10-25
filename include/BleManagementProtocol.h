#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire format for the ost-ble/1 management extension (docs/BLUETOOTH_MANAGEMENT.md).
// Pure layout code, shared by the firmware and the host tests. Cryptography
// (HMAC-SHA256, AES-128-CTR) is performed by the caller with the session key.
namespace BleManagement {

constexpr uint8_t kVersion = 1;
constexpr size_t kMaxFrameBytes = 512;
constexpr size_t kRequestHeaderBytes = 15;
constexpr size_t kMacBytes = 16;
constexpr size_t kMaxSecretBytes = 64;
constexpr size_t kResponseHeaderBytes = 12;
constexpr size_t kMaxSegmentBytes = kMaxFrameBytes - kResponseHeaderBytes - kMacBytes;
constexpr size_t kMaxBodyBytes = 8192;
constexpr size_t kReadyFrameBytes = 8;
constexpr uint8_t kMaxFragments = 32;
constexpr size_t kFragmentHeaderBytes = 2;

// ATT application error codes returned for the final fragment.
constexpr uint8_t kAttBusy = 0x80;
constexpr uint8_t kAttFragmentError = 0x81;
constexpr uint8_t kAttMalformed = 0x82;

constexpr char kRequestLabel[] = "OST-MGMT-v1";
constexpr char kResponseLabel[] = "OST-MGMT-RSP-v1";
constexpr char kEncryptionLabel[] = "OST-MGMT-ENC-v1";

enum class Opcode : uint8_t {
  Continue = 0x3f,
  WifiStatus = 0x40,
  AccessPoint = 0x41,
  DeviceName = 0x42,
  Bluetooth = 0x43,
  Diagnostics = 0x44,
  Events = 0x45,
  Capabilities = 0x46,
  Status = 0x47,
  WifiConfigure = 0x50,
  WifiForget = 0x51,
  AccessPointSet = 0x52,
  DeviceNameSet = 0x53,
  Restart = 0x54,
  AppLabel = 0x55,
  AppRevoke = 0x56,
  PairingStart = 0x57,
  PairingCancel = 0x58,
  PairingRevoke = 0x59,
  BluetoothOff = 0x5a,
  WifiSuspend = 0x5b,
  WifiResume = 0x5c,
  WifiScanStart = 0x60,
  WifiScanResults = 0x61,
  OtaBegin = 0x70,
  OtaChunk = 0x71,
  OtaFinish = 0x72,
  OtaAbort = 0x73,
  OtaDownload = 0x74,
};

inline bool knownOpcode(uint8_t value) {
  return value == 0x3f || (value >= 0x40 && value <= 0x47) ||
         (value >= 0x50 && value <= 0x5c) || value == 0x60 || value == 0x61 ||
         (value >= 0x70 && value <= 0x74);
}

// Reads and Continue are repeatable; everything else is remembered so an
// exact retry returns the first result instead of running twice.
inline bool mutatingOpcode(uint8_t value) {
  return knownOpcode(value) && value != 0x3f && !(value >= 0x40 && value <= 0x47) &&
         value != static_cast<uint8_t>(Opcode::WifiScanResults);
}

// Only these two carry a password in the encrypted secret.
inline bool secretOpcode(uint8_t value) {
  return value == static_cast<uint8_t>(Opcode::WifiConfigure) ||
         value == static_cast<uint8_t>(Opcode::AccessPointSet);
}

inline uint16_t readLe16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0] | (source[1] << 8));
}
inline uint32_t readLe32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) | static_cast<uint32_t>(source[1]) << 8 |
         static_cast<uint32_t>(source[2]) << 16 | static_cast<uint32_t>(source[3]) << 24;
}
inline void writeLe16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}
inline void writeLe32(uint8_t* destination, uint32_t value) {
  for (int i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (8 * i));
}

struct Request {
  uint8_t opcode = 0;
  uint16_t requestId = 0;
  uint32_t bootId = 0;
  uint32_t counter = 0;
  const uint8_t* body = nullptr;
  uint16_t bodyLength = 0;
  const uint8_t* secret = nullptr;
  uint8_t secretLength = 0;
  size_t signedLength = 0;  // Bytes covered by the MAC.
  const uint8_t* mac = nullptr;
};

// Structural checks only. The caller verifies the MAC and counter.
inline bool parseRequest(const uint8_t* frame, size_t length, Request& out) {
  if (frame == nullptr || length < kRequestHeaderBytes + kMacBytes ||
      length > kMaxFrameBytes || frame[0] != kVersion) {
    return false;
  }
  Request request;
  request.opcode = frame[1];
  request.requestId = readLe16(frame + 2);
  request.bootId = readLe32(frame + 4);
  request.counter = readLe32(frame + 8);
  request.bodyLength = readLe16(frame + 12);
  request.secretLength = frame[14];
  if (request.requestId == 0 || request.counter == 0 ||
      request.secretLength > kMaxSecretBytes ||
      kRequestHeaderBytes + request.bodyLength + request.secretLength + kMacBytes != length) {
    return false;
  }
  request.body = frame + kRequestHeaderBytes;
  request.secret = request.body + request.bodyLength;
  request.signedLength = kRequestHeaderBytes + request.bodyLength + request.secretLength;
  request.mac = frame + request.signedLength;
  out = request;
  return true;
}

// AES-128-CTR initial counter block for the secret. The per-authentication
// counter is unique, so the block is never reused under one session key.
inline void secretIv(uint32_t counter, uint16_t requestId, uint8_t iv[16]) {
  memset(iv, 0, 16);
  writeLe32(iv, counter);
  writeLe16(iv + 4, requestId);
}

// Reassembles one request from its fragments. Used only by the BLE host task.
class Assembler {
 public:
  enum class Result : uint8_t { Pending, Complete, SequenceError };

  Result add(const uint8_t* data, size_t length) {
    if (data == nullptr || length < kFragmentHeaderBytes) return fail();
    const uint8_t index = data[0];
    const uint8_t flags = data[1];
    if ((flags & ~0x01) != 0 || index >= kMaxFragments) return fail();
    if (index == 0) {
      length_ = 0;
    } else if (next_ < 0 || index != next_) {
      return fail();
    }
    const size_t payload = length - kFragmentHeaderBytes;
    if (length_ + payload > kMaxFrameBytes) return fail();
    memcpy(buffer_ + length_, data + kFragmentHeaderBytes, payload);
    length_ += payload;
    if ((flags & 0x01) != 0) {
      next_ = -1;
      complete_ = true;
      return Result::Complete;
    }
    next_ = static_cast<int8_t>(index + 1);
    complete_ = false;
    return Result::Pending;
  }
  void reset() { length_ = 0; next_ = -1; complete_ = false; }
  const uint8_t* data() const { return buffer_; }
  size_t length() const { return complete_ ? length_ : 0; }

 private:
  Result fail() { reset(); return Result::SequenceError; }
  uint8_t buffer_[kMaxFrameBytes]{};
  size_t length_ = 0;
  int8_t next_ = -1;
  bool complete_ = false;
};

// Writes the 12-byte response header. The caller appends the segment and the
// MAC over label ‖ LE32(request counter) ‖ header ‖ segment.
inline size_t writeResponseHeader(uint8_t* out, uint8_t opcode, uint16_t requestId,
                                  uint16_t status, uint16_t totalLength,
                                  uint16_t offset, uint16_t segmentLength) {
  out[0] = kVersion;
  out[1] = opcode;
  writeLe16(out + 2, requestId);
  writeLe16(out + 4, status);
  writeLe16(out + 6, totalLength);
  writeLe16(out + 8, offset);
  writeLe16(out + 10, segmentLength);
  return kResponseHeaderBytes;
}

inline void writeReadyFrame(uint8_t out[kReadyFrameBytes], uint8_t opcode,
                            uint16_t requestId, uint16_t status, uint16_t totalLength) {
  out[0] = kVersion;
  out[1] = opcode;
  writeLe16(out + 2, requestId);
  writeLe16(out + 4, status);
  writeLe16(out + 6, totalLength);
}

}  // namespace BleManagement
