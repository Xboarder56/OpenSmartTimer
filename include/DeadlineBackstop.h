#pragma once

#include <stdint.h>

// Opens the lock at a timed release even when loop() is stalled.
//
// Timed releases (expiry and a share link's hold limit) are applied by
// TimerController::tick(), which runs in loop(). A blocked loop, for example
// the V2 WebServer waiting on one slow client, delays them. On V2 the
// backstop arms a one-shot esp_timer for kGraceMs after the next timed
// release. Its callback runs in the higher-priority esp_timer task, drives
// the lock output and amber indicator inactive, and marks itself fired;
// TimerController then records the release on the next loop pass. Normally
// tick() releases first and cancels it, so it acts only when the loop is
// late. It never energizes the output, so any fault fails open.
//
// V1 runs a non-blocking web server and keeps these as no-ops. Host builds
// use an inspectable fake.
namespace DeadlineBackstop {

constexpr uint32_t kGraceMs = 200;

// releaseAtMs is on the millis() clock, the one TimerController uses.
#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)
void armAt(uint32_t releaseAtMs);
void cancel();
bool fired();
uint32_t firedCount();
uint32_t armFailedCount();
#elif defined(SMART_TIMER_HARDWARE_V2)
struct HostState {
  bool armed = false;
  uint32_t releaseAtMs = 0;
  uint32_t armCount = 0;
  uint32_t cancelCount = 0;
  bool fired = false;
};
inline HostState host;
inline void armAt(uint32_t releaseAtMs) {
  host.armed = true;
  host.releaseAtMs = releaseAtMs;
  host.fired = false;
  ++host.armCount;
}
inline void cancel() {
  host.armed = false;
  host.fired = false;
  ++host.cancelCount;
}
inline bool fired() { return host.fired; }
inline uint32_t firedCount() { return 0; }
inline uint32_t armFailedCount() { return 0; }
#else
inline void armAt(uint32_t) {}
inline void cancel() {}
inline bool fired() { return false; }
inline uint32_t firedCount() { return 0; }
inline uint32_t armFailedCount() { return 0; }
#endif

}  // namespace DeadlineBackstop
