#include "ResetTrace.h"
#include <Arduino.h>

#include "BluetoothApplication.h"
#include "DeviceConfig.h"
#include "Hardware.h"
#include "LocalNetwork.h"
#include "TimerController.h"
#include "WebApplication.h"
#if defined(SMART_TIMER_RELAY)
#include "RelayRuntime.h"
#include "generated_build_identity.h"
#endif

#if defined(SMART_TIMER_HARDWARE_V2)
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#ifndef OPENSMARTTIMER_FRAMEWORK_PROFILE
#error "The V2 build requires the pinned OpenSmartTimer framework profile"
#endif

extern "C" const char kOpenSmartTimerFrameworkProfile[]
    __attribute__((used)) =
        "OPENSMARTTIMER_FRAMEWORK_PROFILE="
        OPENSMARTTIMER_FRAMEWORK_PROFILE;

// Arduino otherwise confirms a pending OTA image before setup() initializes
// the fail-open outputs, storage, Wi-Fi, or local web recovery path.
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace {
// Emergency edge interrupt: drive the lock output inactive at once, then let
// loop() perform the normal Emergency release. Direct register writes only;
// nothing here may execute from flash.
void IRAM_ATTR emergencyEdgeIsr() {
#if defined(SMART_TIMER_HARDWARE_V2)
  const uint32_t mask = (1UL << Hardware::kLockPinA) | (1UL << Hardware::kLockIndicatorPin);
  if (Hardware::kLockActiveHigh) REG_WRITE(GPIO_OUT_W1TC_REG, mask);
  else REG_WRITE(GPIO_OUT_W1TS_REG, mask);
#else
  static_assert(Hardware::kLockPinA < 16 && Hardware::kLockPinB < 16,
                "GPOC/GPOS address GPIO0-15");
  const uint32_t mask = (1UL << Hardware::kLockPinA) | (1UL << Hardware::kLockPinB);
  if (Hardware::kLockActiveHigh) GPOC = mask;
  else GPOS = mask;
#endif
  Hardware::emergencyLatched = true;
}

// On V2 the Arduino GPIO ISR service is not IRAM-resident in this framework
// build, so the interrupt is deferred while flash is being written. Firmware
// updates, pairing and configuration writes require an open timer. Bluetooth
// can still write flash during a session (bonded reconnects store their
// subscriptions, a repeated pairing attempt removes an old bond). The GPIO
// latches the edge, so a press then is handled late, when the write ends.
void installEmergencyLatch() {
  attachInterrupt(digitalPinToInterrupt(Hardware::kEmergencyPin), emergencyEdgeIsr,
                  Hardware::kInputActiveHigh ? RISING : FALLING);
}

TimerController timerController;
DeviceConfig deviceConfig;
LocalNetwork localNetwork(deviceConfig);
WebApplication webApplication(timerController, localNetwork);
#if defined(SMART_TIMER_HARDWARE_V2)
BluetoothApplication bluetoothApplication(timerController, deviceConfig,
                                          localNetwork);
#endif
#if defined(SMART_TIMER_RELAY)
// Experimental; allocates nothing and opens no connection until enabled.
opensmarttimer::relay::RelayRuntime relayRuntime(
    timerController, SMART_TIMER_BUILD_ID, Platform::hardwareModel(),
    Platform::chipFamily());
#endif

struct DebouncedInput {
  uint8_t pin;
  bool raw = false;
  bool stable = false;
  uint32_t changedAtMs = 0;

  bool update(uint32_t nowMs) {
    const bool next = digitalRead(pin) ==
                      (Hardware::kInputActiveHigh ? HIGH : LOW);
    if (next != raw) {
      raw = next;
      changedAtMs = nowMs;
    }
    if (stable != raw && nowMs - changedAtMs >= 35) {
      stable = raw;
      return true;
    }
    return false;
  }
};

DebouncedInput emergencyInput{Hardware::kEmergencyPin};
DebouncedInput configInput{Hardware::kConfigPin};
uint32_t configPressStartedMs = 0;
bool configPressTracking = false;
bool configResetHandled = false;
#if defined(SMART_TIMER_RELAY)
bool factoryResetRequested = false;
uint32_t configRelayApprovalId = 0;
#endif

#if defined(SMART_TIMER_HARDWARE_V2)
constexpr uint32_t kBootHealthStableMs = 5000;
constexpr uint32_t kBootValidationRetryMs = 1000;
constexpr uint32_t kBluetoothAutoStartDelayMs = 15000;
// Expiry and the emergency input are serviced only by loop(). If it hangs,
// the watchdog panics and the reboot drives the lock output low. The window
// exceeds the WebServer's 5 s header + 5 s body waits for one slow client;
// the OTA upload handler feeds it per chunk.
constexpr uint32_t kLoopWatchdogTimeoutMs = 20000;

void enableLoopWatchdog() {
  const esp_task_wdt_config_t config = {
      .timeout_ms = kLoopWatchdogTimeoutMs,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&config) != ESP_OK) {
    Serial.println(F("Loop watchdog reconfiguration failed."));
    return;
  }
  enableLoopWDT();
}
bool failOpenInitialized = false;
bool configStorageReady = false;
bool bootValidationFinished = false;
bool bluetoothAutoStartRequired = false;
bool bluetoothAutoStartAttempted = false;
uint32_t bootHealthReadySinceMs = 0;
uint32_t nextBootValidationAttemptMs = 0;

void serviceBootValidation(uint32_t nowMs) {
  if (bootValidationFinished) {
    return;
  }
  const bool recoveryNetworkReady = localNetwork.stationConnected() ||
                                    localNetwork.accessPointRunning();
  const bool bluetoothReady =
      !bluetoothAutoStartRequired ||
      (bluetoothAutoStartAttempted && bluetoothApplication.initialized() &&
       bluetoothApplication.synchronized());
  const bool healthy = failOpenInitialized && configStorageReady &&
                       recoveryNetworkReady && webApplication.ready() &&
                       bluetoothReady;
  if (!healthy) {
    bootHealthReadySinceMs = 0;
    return;
  }
  if (bootHealthReadySinceMs == 0) {
    bootHealthReadySinceMs = nowMs == 0 ? 1 : nowMs;
    return;
  }
  if (nowMs - bootHealthReadySinceMs < kBootHealthStableMs ||
      (nextBootValidationAttemptMs != 0 &&
       static_cast<int32_t>(nowMs - nextBootValidationAttemptMs) < 0)) {
    return;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr ||
      esp_ota_get_state_partition(running, &state) != ESP_OK) {
    nextBootValidationAttemptMs = nowMs + kBootValidationRetryMs;
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    bootValidationFinished = true;
    return;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    bootValidationFinished = true;
    Serial.println(F("V2 OTA image confirmed after recovery health check."));
  } else {
    nextBootValidationAttemptMs = nowMs + kBootValidationRetryMs;
  }
}

void serviceBluetoothAutoStart(uint32_t nowMs) {
  if (bluetoothAutoStartAttempted || !bluetoothAutoStartRequired ||
      nowMs < kBluetoothAutoStartDelayMs) {
    return;
  }
  bluetoothAutoStartAttempted = true;
  if (!bluetoothApplication.begin(webApplication.bootId())) {
    Serial.println(F("Persistent Bluetooth start failed; web recovery remains available."));
  }
}
#endif

void performFactoryReset() {
#if defined(SMART_TIMER_HARDWARE_V2)
  bluetoothApplication.clearBondsForFactoryReset(webApplication.bootId());
#endif
  const bool resetSaved = deviceConfig.factoryReset();
  Serial.println(resetSaved
      ? F("Factory settings restored; restarting with the default name and an open setup AP.")
      : F("Factory reset storage failed; restarting for recovery. Settings may be retained."));
  Hardware::writeLock(false);
  delay(50);
  ESP.restart();
}

void handlePhysicalInputs(uint32_t nowMs) {
  emergencyInput.update(nowMs);
  // Assertion is intentionally immediate; only release is debounced. A held
  // emergency circuit both releases an active session and prevents a later
  // start until the circuit has remained open long enough to be stable.
  // A latched interrupt edge counts even if a momentary contact has already
  // opened again; the next pass then records the input clearing.
  const bool latchedEdge = Hardware::consumeEmergencyLatch();
  timerController.setEmergencyInputActive(
      latchedEdge || emergencyInput.stable || Hardware::emergencyInputActive(),
      nowMs);

  const bool configChanged = configInput.update(nowMs);
  if (configChanged && configInput.stable) {
    configPressTracking = timerController.isOpen();
    configPressStartedMs = nowMs;
    configResetHandled = false;
#if defined(SMART_TIMER_RELAY)
    opensmarttimer::relay::RelayStatusInfo relayStatus{};
    relayRuntime.status(&relayStatus);
    configRelayApprovalId = relayStatus.setup_pending ? relayStatus.setup_request_id : 0;
#endif
  } else if (configChanged && !configInput.stable) {
    if (configPressTracking && !configResetHandled &&
        timerController.isOpen()) {
      const uint32_t heldMs = nowMs - configPressStartedMs;
#if defined(SMART_TIMER_HARDWARE_V2)
      if (heldMs <= Hardware::kOtaShortPressMaxMs &&
          bluetoothApplication.pairingActive(nowMs)) {
        if (bluetoothApplication.pairingAwaitingConfirmation(nowMs) &&
            bluetoothApplication.confirmPhysicalPairing(nowMs)) {
          Serial.println(F("Bluetooth app confirmed; it may now finish pairing."));
        } else {
          Serial.println(F("Bluetooth pairing is active; OTA was not armed."));
        }
      }
#if defined(SMART_TIMER_RELAY)
      else if (heldMs <= Hardware::kOtaShortPressMaxMs && configRelayApprovalId != 0) {
        const auto approved = relayRuntime.confirmEnrollment(configRelayApprovalId);
        Serial.println(approved == opensmarttimer::relay::RelayOpResult::Accepted
            ? F("Relay setup physically approved; connecting.")
            : F("Relay setup was cancelled or expired; OTA was not armed."));
      }
#endif
      else if (heldMs >= Hardware::kBluetoothPairingHoldMinMs &&
                 heldMs <= Hardware::kBluetoothPairingHoldMaxMs) {
        if (!webApplication.mutationTransitionActive(nowMs) &&
            bluetoothApplication.startPhysicalPairing(
                nowMs, webApplication.bootId())) {
          Serial.println(F("Bluetooth pairing available for two minutes."));
        }
      } else
#endif
      if (heldMs <= Hardware::kOtaShortPressMaxMs) {
        webApplication.armOta(nowMs);
        Serial.println(webApplication.otaUnverifiedAllowed(nowMs)
                           ? F("Unverified firmware allowed for one upload.")
                           : F("Firmware update armed for five minutes."));
      }
    }
    configPressTracking = false;
    configPressStartedMs = 0;
    configResetHandled = false;
#if defined(SMART_TIMER_RELAY)
    configRelayApprovalId = 0;
#endif
    return;
  }

  if (!timerController.isOpen()) {
    configPressTracking = false;
    configPressStartedMs = 0;
    configResetHandled = false;
    return;
  }

  if (configPressTracking && configInput.stable && !configResetHandled &&
      nowMs - configPressStartedMs >= Hardware::kFactoryResetHoldMs) {
    configResetHandled = true;
#if defined(SMART_TIMER_RELAY)
    factoryResetRequested = true;
#else
    performFactoryReset();
#endif
  }
}

bool configButtonHoldActive() {
  return configPressTracking && configInput.stable &&
         timerController.isOpen();
}
}  // namespace

void setup() {
  // GPIO safety happens before Serial, EEPROM, Wi-Fi, or the web server.
  timerController.begin();
  installEmergencyLatch();
  ResetTrace::begin();
#if defined(SMART_TIMER_HARDWARE_V2)
  failOpenInitialized = Hardware::failOpenInitializationSucceeded();
#endif
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("OpenSmartTimer firmware starting"));
#if defined(SMART_TIMER_HARDWARE_V2)
  Serial.println(kOpenSmartTimerFrameworkProfile);
#endif

#if defined(SMART_TIMER_HARDWARE_V2)
  configStorageReady = deviceConfig.begin();
  bluetoothAutoStartRequired =
      configStorageReady && deviceConfig.bluetoothEnabled();
#else
  deviceConfig.begin();
#endif
  timerController.setControlPolicy(deviceConfig.pauseAllowed(),
                                   deviceConfig.controlLockAllowed());
  localNetwork.begin();
  webApplication.begin();
#if defined(SMART_TIMER_HARDWARE_V2)
  // Bluetooth settings run the same route handlers as the HTTP API.
  bluetoothApplication.setManagementHost(&webApplication);
#endif
#if defined(SMART_TIMER_RELAY)
  // A relay Identify command runs the same LED window as REST.
  relayRuntime.setIdentifyHandler(
      [](void*) { webApplication.beginIdentify(millis()); }, nullptr);
  // While the relay writes flash it holds the timer open: REST/BLE starts
  // and OTA wait (the V2 GPIO ISR can be deferred during flash writes).
  webApplication.setMutationInterlock(
      [](void*) { return relayRuntime.maintenanceActive(); }, nullptr);
  relayRuntime.begin(webApplication.bootId(), localNetwork.deviceName().c_str());
#endif

  Serial.print(F("Fallback Wi-Fi: "));
  Serial.println(localNetwork.accessPointName());
  Serial.print(F("Fallback Wi-Fi security: "));
  Serial.println(localNetwork.accessPointSecurity());
  Serial.println(F("The local HTTP API has no device login."));
  Serial.println(F("Open http://192.168.4.1 or http://smarttimer.local"));
#if defined(SMART_TIMER_HARDWARE_V2)
  // Subscribe after setup's one-time storage/radio initialization.
  enableLoopWatchdog();
#endif
}

void loop() {
  const uint32_t nowMs = millis();
  ResetTrace::mark(ResetTrace::kInputs);
  handlePhysicalInputs(nowMs);
  ResetTrace::mark(ResetTrace::kTimer);
  timerController.tick(nowMs);
#if defined(SMART_TIMER_RELAY)
  if (factoryResetRequested) {
    const auto reset = relayRuntime.eraseAllForFactoryReset();
    if (reset == opensmarttimer::relay::RelayOpResult::Ok) performFactoryReset();
    else if (reset != opensmarttimer::relay::RelayOpResult::Accepted) {
      factoryResetRequested = false;
      Serial.println(F("Relay reset failed; settings retained. Retry the Reset-button hold."));
    }
  }
#endif
  ResetTrace::mark(ResetTrace::kNetwork);
  localNetwork.loop(nowMs);
#if defined(SMART_TIMER_HARDWARE_V2)
  serviceBluetoothAutoStart(nowMs);
#endif
  bool statusLed = Hardware::statusLedOn(
      nowMs, localNetwork.accessPointRunning(),
      localNetwork.stationConnected(), localNetwork.standaloneMode());
  if (timerController.isOpen()) {
    if (configButtonHoldActive()) {
      statusLed = Hardware::factoryResetHoldLedOn(
          nowMs, nowMs - configPressStartedMs);
    }
    else if (webApplication.otaInstallActive()) {
      statusLed = Hardware::otaInstallLedOn(
          nowMs, webApplication.otaInstallSucceeded());
    }
#if defined(SMART_TIMER_HARDWARE_V2)
    else if (bluetoothApplication.pairingActive(nowMs)) {
      statusLed = bluetoothApplication.pairingLedOn(nowMs);
    } else if (bluetoothApplication.identifying(nowMs)) {
      statusLed = bluetoothApplication.identifyLedOn(nowMs);
    }
#endif
    else if (webApplication.identifying(nowMs)) {
      statusLed = webApplication.identifyLedOn(nowMs);
    } else if (webApplication.otaUnverifiedAllowed(nowMs)) {
      statusLed = Hardware::otaUnverifiedLedOn(nowMs);
    } else if (webApplication.otaArmed(nowMs)) {
      statusLed = Hardware::otaArmedLedOn(nowMs);
    }
  }
  Hardware::writeStatus(statusLed);
  webApplication.loop(nowMs);
#if defined(SMART_TIMER_HARDWARE_V2)
  ResetTrace::mark(ResetTrace::kBluetooth);
  bluetoothApplication.pauseAdvertising(localNetwork.scanInProgress());
  bluetoothApplication.loop(
      nowMs, !webApplication.mutationTransitionActive(nowMs));
#endif
#if defined(SMART_TIMER_RELAY)
  ResetTrace::mark(ResetTrace::kRelay);
  // Relay commands are applied here, on the timer's own loop, with the same
  // transition gate as REST and BLE. Local Bluetooth has priority: while an
  // app is connected or pairing is open, the relay pauses (memory).
  relayRuntime.loop(localNetwork.stationConnected(),
                    !webApplication.mutationTransitionActive(nowMs, false),
                    bluetoothApplication.connected() ||
                        bluetoothApplication.pairingActive(nowMs));
#endif
#if defined(SMART_TIMER_HARDWARE_V2)
  serviceBootValidation(nowMs);
#endif
  // A trace stuck here means the hang came after loop() returned: in the
  // SDK, the TCP stack or core housekeeping such as mDNS.
  ResetTrace::mark(ResetTrace::kLoopEnd);
  delay(1);
}
