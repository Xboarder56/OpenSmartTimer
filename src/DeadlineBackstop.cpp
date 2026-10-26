#include "DeadlineBackstop.h"

#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#include "Hardware.h"

namespace DeadlineBackstop {
namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t timer = nullptr;
// Guarded by mux: whether a release is due, and when on the esp_timer clock.
bool armed = false;
int64_t targetUs = 0;
volatile bool firedFlag = false;
uint32_t firedTotal = 0;  // guarded by mux
uint32_t armFailures = 0;

// Runs in the esp_timer task, which preempts loop(). A stale expiry (the
// session was extended, paused or released meanwhile) finds the target moved
// or disarmed and does nothing.
void onTimer(void*) {
  bool due = false;
  portENTER_CRITICAL(&mux);
  if (armed && esp_timer_get_time() - targetUs >= 0) {
    armed = false;
    firedFlag = true;
    ++firedTotal;
    due = true;
  }
  portEXIT_CRITICAL(&mux);
  if (!due) return;
  // The same register writes as the emergency interrupt: outputs off only.
  const uint32_t mask =
      (1UL << Hardware::kLockPinA) | (1UL << Hardware::kLockIndicatorPin);
  if (Hardware::kLockActiveHigh) REG_WRITE(GPIO_OUT_W1TC_REG, mask);
  else REG_WRITE(GPIO_OUT_W1TS_REG, mask);
}

bool ensureTimer() {
  if (timer != nullptr) return true;
  esp_timer_create_args_t args = {};
  args.callback = &onTimer;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "deadline";
  return esp_timer_create(&args, &timer) == ESP_OK;
}

}  // namespace

void armAt(uint32_t releaseAtMs) {
  // millis() is esp_timer_get_time() / 1000 on Arduino-ESP32.
  const int32_t untilMs = static_cast<int32_t>(releaseAtMs - millis());
  const int64_t delayUs =
      (static_cast<int64_t>(untilMs > 0 ? untilMs : 0) + kGraceMs) * 1000;
  portENTER_CRITICAL(&mux);
  armed = true;
  firedFlag = false;
  targetUs = esp_timer_get_time() + delayUs;
  portEXIT_CRITICAL(&mux);
  // Without the timer, tick() in loop() still releases, as before.
  if (!ensureTimer()) {
    ++armFailures;
    return;
  }
  esp_timer_stop(timer);  // Not running is fine.
  if (esp_timer_start_once(timer, static_cast<uint64_t>(delayUs)) != ESP_OK) {
    ++armFailures;
  }
}

void cancel() {
  portENTER_CRITICAL(&mux);
  armed = false;
  firedFlag = false;
  portEXIT_CRITICAL(&mux);
  if (timer != nullptr) esp_timer_stop(timer);
}

bool fired() { return firedFlag; }
uint32_t firedCount() {
  portENTER_CRITICAL(&mux);
  const uint32_t total = firedTotal;
  portEXIT_CRITICAL(&mux);
  return total;
}
uint32_t armFailedCount() { return armFailures; }

}  // namespace DeadlineBackstop

#endif
