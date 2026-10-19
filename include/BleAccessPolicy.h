#pragma once
#include <cstdint>

namespace BleAccessPolicy {
// Link encryption can be repaired by OS pairing. An application-key failure
// cannot: use a service-specific error rather than triggering security retries.
constexpr uint8_t kApplicationAuthorizationRequired = 0x83;
constexpr uint8_t authorizationError(bool encrypted, bool bonded) {
  return !encrypted ? 0x0f : !bonded ? 0x05 : kApplicationAuthorizationRequired;
}
// A revoked connection may read ONLY its already-signed final receipt. No
// retained application authority, new requests, continuation or reauthentication.
constexpr uint32_t kReceiptDrainMs = 2000;
inline bool receiptReadable(uint32_t now, uint32_t deadline, bool ready,
                            bool connected, bool encrypted, bool sameConnection) {
  return deadline != 0 && static_cast<int32_t>(deadline - now) > 0 &&
         ready && connected && encrypted && sameConnection;
}
}  // namespace BleAccessPolicy
