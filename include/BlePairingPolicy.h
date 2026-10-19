#pragma once
#include <cstdint>

namespace BlePairingPolicy {
inline bool nicknameWritable(bool pendingSecretReadable, bool ownerNamed) {
  return pendingSecretReadable && !ownerNamed;
}
enum class State : uint8_t {
  Idle, WaitingForConnection, WaitingForEncryption, WaitingForConfirmation,
  SecretReady, WaitingForCommit, Committing, Complete, Failed
};

inline bool secretReadable(State state, bool physical, bool available,
                           bool selectedPeer, bool encrypted, bool bonded,
                           uint32_t now, uint32_t pairingDeadline,
                           uint32_t secretDeadline) {
  return physical && available && selectedPeer && encrypted && bonded &&
      (state == State::SecretReady || state == State::WaitingForCommit) &&
      pairingDeadline != 0 && secretDeadline != 0 &&
      static_cast<int32_t>(pairingDeadline - now) > 0 &&
      static_cast<int32_t>(secretDeadline - now) > 0;
}
// An authenticated connection that has sent no GATT read or write for the
// whole window is idle. The comparison is signed because the host task can
// record an access newer than the caller's loop timestamp.
inline bool authenticatedConnectionIdle(bool connected, bool authenticated,
                                        bool pairing, uint32_t lastPeerActivity,
                                        uint32_t now, uint32_t timeout) {
  return connected && authenticated && !pairing && lastPeerActivity != 0 &&
      static_cast<int32_t>(now - lastPeerActivity) >= static_cast<int32_t>(timeout);
}
}  // namespace BlePairingPolicy
