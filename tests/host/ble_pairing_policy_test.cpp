#include "BlePairingPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  using namespace BlePairingPolicy;
  assert(nicknameWritable(true, false));
  assert(!nicknameWritable(true, true));
  assert(!nicknameWritable(false, false));
  assert(!nicknameWritable(false, true));
  for (State state : {State::SecretReady, State::WaitingForCommit}) {
    assert(secretReadable(state, true, true, true, true, true, 100, 1000, 900));
    assert(secretReadable(state, true, true, true, true, true, 101, 1000, 900));
    for (unsigned bit = 0; bit < 5; ++bit) {
      const unsigned flags = 31 ^ (1 << bit);
      assert(!secretReadable(state, flags & 1, flags & 2, flags & 4, flags & 8, flags & 16, 100, 1000, 900));
    }
    assert(!secretReadable(state, true, true, true, true, true, 900, 1000, 900));
    assert(!secretReadable(state, true, true, true, true, true, 1000, 1000, 1500));
    assert(secretReadable(state, true, true, true, true, true, UINT32_MAX - 10, 100, 100));
  }
  for (State state : {State::Idle, State::WaitingForConnection, State::WaitingForEncryption,
       State::WaitingForConfirmation, State::Committing, State::Complete, State::Failed})
    assert(!secretReadable(state, true, true, true, true, true, 100, 1000, 900));
  // Authenticated idle drop: only after a full quiet window, never early.
  constexpr uint32_t kIdle = 5UL * 60UL * 1000UL;
  assert(authenticatedConnectionIdle(true, true, false, 1000, 1000 + kIdle, kIdle));
  assert(!authenticatedConnectionIdle(true, true, false, 1000, 1000 + kIdle - 1, kIdle));
  assert(!authenticatedConnectionIdle(false, true, false, 1000, 1000 + kIdle, kIdle));
  assert(!authenticatedConnectionIdle(true, false, false, 1000, 1000 + kIdle, kIdle));
  assert(!authenticatedConnectionIdle(true, true, true, 1000, 1000 + kIdle, kIdle));
  assert(!authenticatedConnectionIdle(true, true, false, 0, 1000 + kIdle, kIdle));
  // An access stamped after the loop's timestamp is activity, not a wrap.
  assert(!authenticatedConnectionIdle(true, true, false, 5000, 4990, kIdle));
  // Across millis() rollover the elapsed time is still measured correctly.
  assert(!authenticatedConnectionIdle(true, true, false, UINT32_MAX - 10, 100, kIdle));
  assert(authenticatedConnectionIdle(true, true, false, UINT32_MAX - 10, kIdle - 11, kIdle));
  std::cout << "BLE pairing-secret retry/authorization/deadline and idle-connection tests passed\n";
}
