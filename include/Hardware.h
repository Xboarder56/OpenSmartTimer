#pragma once

#include <Arduino.h>
#include "BoardProfile.h"

#if defined(SMART_TIMER_HARDWARE_V2)
#include <driver/gpio.h>
#endif

namespace Hardware {

using Profile = Board::Profile;
constexpr uint8_t kLockPinA = Profile::kLockPinA;
constexpr uint8_t kLockPinB = Profile::kLockPinB;
constexpr uint8_t kStatusPin = Profile::kStatusPin;
constexpr uint8_t kEmergencyPin = Profile::kEmergencyPin;
constexpr uint8_t kConfigPin = Profile::kConfigPin;
constexpr bool kHasSeparateLockPinB = Profile::kHasSeparateLockPinB;
constexpr bool kHasLockIndicatorPin = Profile::kHasLockIndicatorPin;
constexpr bool kLockActiveHigh = Profile::kLockActiveHigh;
constexpr bool kStatusActiveHigh = Profile::kStatusActiveHigh;
constexpr bool kInputActiveHigh = Profile::kInputActiveHigh;
#if defined(SMART_TIMER_HARDWARE_V2)
constexpr uint8_t kLockIndicatorPin = Profile::kLockIndicatorPin;
constexpr uint8_t kAuxiliaryInputPin = Profile::kAuxiliaryInputPin;
#endif
constexpr uint32_t kMaximumDurationMs = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kOtaShortPressMaxMs = 2UL * 1000UL;
constexpr uint32_t kBluetoothPairingHoldMinMs = 3UL * 1000UL;
constexpr uint32_t kBluetoothPairingHoldMaxMs = 5UL * 1000UL;
constexpr uint32_t kFactoryResetHoldMs = 10UL * 1000UL;

inline bool inputActive(uint8_t pin) {
  return digitalRead(pin) == (kInputActiveHigh ? HIGH : LOW);
}

inline bool emergencyInputActive() { return inputActive(kEmergencyPin); }

// An emergency edge seen by the interrupt, not yet handled by loop(). loop()
// only polls the pin, so a momentary press shorter than a loop stall (Wi-Fi
// mode changes, flash writes) would otherwise be missed. The interrupt writes
// this variable directly. A plain volatile flag is enough: losing a second
// edge while the first is handled changes nothing, both mean "release now".
inline volatile bool emergencyLatched = false;
inline bool emergencyLatchPending() { return emergencyLatched; }
inline bool consumeEmergencyLatch() {
  if (!emergencyLatched) return false;
  emergencyLatched = false;
  return true;
}

inline void writeStatus(bool on) {
  digitalWrite(kStatusPin, (on == kStatusActiveHigh) ? HIGH : LOW);
}

inline bool statusLedOn(uint32_t nowMs, bool apRunning,
                        bool stationConnected, bool standaloneMode) {
  // The amber hardware indicator already shows lock-driver state. Keep the
  // blue LED dedicated to network availability and maintenance feedback.
  if (stationConnected || (apRunning && standaloneMode)) {
    return true;
  }
  if (apRunning) {
    return (nowMs % 1000UL) < 500UL;  // Slow Recovery AP blink.
  }
  return (nowMs % 300UL) < 150UL;  // Fast reconnect blink.
}

inline bool otaArmedLedOn(uint32_t nowMs) {
  const uint32_t phase = nowMs % 2000UL;
  return phase < 100UL || (phase >= 250UL && phase < 350UL);
}

inline bool otaUnverifiedLedOn(uint32_t nowMs) {
  // Three blue flashes distinguish the one-upload validation override from
  // the ordinary two-flash OTA window. The amber indicator follows the lock
  // driver and must never be pulsed merely for maintenance feedback.
  const uint32_t phase = nowMs % 2000UL;
  return phase < 100UL || (phase >= 250UL && phase < 350UL) ||
         (phase >= 500UL && phase < 600UL);
}

inline bool otaInstallLedOn(uint32_t nowMs, bool accepted) {
  // Upload and validation run inside the synchronous HTTP handler, so the
  // caller also writes the LED directly as chunks arrive. Once validation has
  // succeeded, a fast blink makes the pending restart visually distinct.
  return !accepted || (nowMs % 160UL) < 80UL;
}

inline bool factoryResetHoldLedOn(uint32_t nowMs, uint32_t heldMs) {
  const uint32_t periodMs = heldMs >= 8000UL ? 120UL : 300UL;
  return (nowMs % periodMs) < periodMs / 2UL;
}

inline void writeLock(bool locked) {
  const uint8_t level = (locked == kLockActiveHigh) ? HIGH : LOW;
  digitalWrite(kLockPinA, level);
#if !defined(SMART_TIMER_HARDWARE_V2)
  digitalWrite(kLockPinB, level);
#else
  digitalWrite(kLockIndicatorPin, level);
#endif
}

#if defined(SMART_TIMER_HARDWARE_V2)
inline bool& failOpenInitializationState() {
  static bool initialized = false;
  return initialized;
}

inline bool failOpenInitializationSucceeded() {
  return failOpenInitializationState();
}
#endif

inline void beginFailOpen() {
  // Preload the output latches before enabling output mode to avoid a brief
  // active pulse while setup() takes ownership of the pins. Arduino-ESP32
  // ignores digitalWrite() until pinMode() registers a pin, so V2 must write
  // the ESP-IDF GPIO latch directly before enabling the output drivers.
#if defined(SMART_TIMER_HARDWARE_V2)
  const bool lockPreloaded =
      gpio_set_level(static_cast<gpio_num_t>(kLockPinA), 0) == ESP_OK;
  const bool indicatorPreloaded =
      gpio_set_level(static_cast<gpio_num_t>(kLockIndicatorPin), 0) == ESP_OK;
  const bool statusPreloaded =
      gpio_set_level(static_cast<gpio_num_t>(kStatusPin), 0) == ESP_OK;
#else
  digitalWrite(kLockPinA, LOW);
  digitalWrite(kLockPinB, LOW);
  digitalWrite(kStatusPin, LOW);
#endif
  pinMode(kLockPinA, OUTPUT);
#if !defined(SMART_TIMER_HARDWARE_V2)
  pinMode(kLockPinB, OUTPUT);
#else
  pinMode(kLockIndicatorPin, OUTPUT);
#endif
  pinMode(kStatusPin, OUTPUT);
  writeLock(false);

  // These inputs have external biasing on the original hardware. GPIO10 is
  // only usable with the recovered DOUT flash configuration.
  pinMode(kEmergencyPin, INPUT);
  pinMode(kConfigPin, INPUT);
#if defined(SMART_TIMER_HARDWARE_V2)
  pinMode(kAuxiliaryInputPin, INPUT);
  failOpenInitializationState() =
      lockPreloaded && indicatorPreloaded && statusPreloaded;
#endif
}

}  // namespace Hardware
