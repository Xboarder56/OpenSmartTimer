#include "WebApplication.h"
#include "DeviceConfig.h"
#include "Hardware.h"
#include "LocalNetwork.h"
#include "TimerController.h"
#if defined(SMART_TIMER_HARDWARE_V2)
#include "BluetoothApplication.h"
#endif
#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

namespace {
uint32_t parseUnsigned(const String& value) {
  uint32_t result;
  return HttpRequestFields::unsignedInteger(value.c_str(), result) ? result : 0;
}

bool printableAscii(const String& value) {
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if (character < 0x20 || character > 0x7E) {
      return false;
    }
  }
  return true;
}

}  // namespace

void WebApplication::registerModernRoutes() {
  server_.on("/api/v1/status", HTTP_GET, [this]() { routeStatus(); });

  server_.on("/api/v1/timer/start", HTTP_POST, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    const uint32_t duration = parseUnsigned(requestValue("duration_ms"));
    const uint32_t minimum = parseUnsigned(requestValue("min_ms"));
    const uint32_t maximum = parseUnsigned(requestValue("max_ms"));
    const bool randomDuration = requestValue("min_ms").length() != 0 ||
                                requestValue("max_ms").length() != 0;
    if (randomDuration && requestValue("duration_ms").length() != 0) {
      sendError(400, "choose a fixed duration or a random range, not both", "invalid_random_range");
      return;
    }
    if (randomDuration) {
      if (minimum == 0 || maximum < minimum ||
          maximum > Hardware::kMaximumDurationMs) {
        sendError(400, "invalid random range", "invalid_random_range");
        return;
      }
    } else if (duration == 0 || duration > Hardware::kMaximumDurationMs) {
      sendError(400, "duration must be between 1 ms and 24 hours",
                "invalid_duration");
      return;
    }
    TimerCommand command;
    command.op = TimerCommandOp::Start;
    command.valueA = static_cast<int32_t>(randomDuration ? minimum : duration);
    command.valueB = static_cast<int32_t>(randomDuration ? maximum : 0);
    command.randomValue = Platform::random32();
    handleTimerMutation("start", command);
  });

  const auto simpleMutation = [this](const char* action, TimerCommandOp op) {
    if (!allowRequest(true)) {
      return;
    }
    TimerCommand command;
    command.op = op;
    handleTimerMutation(action, command);
  };
  server_.on("/api/v1/timer/stop", HTTP_POST, [simpleMutation]() {
    simpleMutation("release", TimerCommandOp::Release);
  });
  server_.on("/api/v1/timer/release", HTTP_POST, [simpleMutation]() {
    simpleMutation("release", TimerCommandOp::Release);
  });
  server_.on("/api/v1/timer/pause", HTTP_POST, [simpleMutation]() {
    simpleMutation("pause", TimerCommandOp::Pause);
  });
  server_.on("/api/v1/timer/resume", HTTP_POST, [simpleMutation]() {
    simpleMutation("resume", TimerCommandOp::Resume);
  });
  server_.on("/api/v1/timer/adjust", HTTP_POST, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    const String value = requestValue("delta_ms");
    int32_t delta = 0;
    if (!HttpRequestFields::signedInteger(value.c_str(), delta) ||
        delta < -static_cast<long>(Hardware::kMaximumDurationMs) ||
        delta > static_cast<long>(Hardware::kMaximumDurationMs)) {
      sendError(400, "invalid adjustment", "invalid_adjustment");
      return;
    }
    TimerCommand command;
    command.op = TimerCommandOp::Adjust;
    command.valueA = delta;
    handleTimerMutation("adjust", command);
  });
  server_.on("/api/v1/timer/seal", HTTP_POST,
             [this]() { handleLockControlsMutation(true); });
  server_.on("/api/v1/timer/lock-controls", HTTP_POST,
             [this]() { handleLockControlsMutation(false); });

  server_.on("/api/v1/wifi/scan", HTTP_POST, [this]() { routeWifiScanStart(); });
  server_.on("/api/v1/wifi/scan", HTTP_GET, [this]() { routeWifiScanResults(); });
  server_.on("/api/v1/wifi/status", HTTP_GET, [this]() { routeWifiStatus(); });
  server_.on("/api/v1/timer/controls", HTTP_GET, [this]() {
    if (allowRequest(false)) sendSessionControls();
  });
  server_.on("/api/v1/timer/controls", HTTP_POST,
             [this]() { routeSessionControlsSet(); });
  server_.on("/api/v1/wifi/ap", HTTP_GET, [this]() { routeAccessPoint(); });
  server_.on("/api/v1/wifi/ap", HTTP_PUT, [this]() { routeAccessPointSet(); });
  server_.on("/api/v1/wifi/configure", HTTP_POST, [this]() { routeWifiConfigure(); });
  server_.on("/api/v1/wifi/forget", HTTP_POST, [this]() { routeWifiForget(); });
  server_.on("/api/v1/system/restart", HTTP_POST, [this]() { routeRestart(); });
  server_.on("/api/v1/system/identify", HTTP_POST, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    TimerCommand command;
    command.op = TimerCommandOp::Identify;
    const uint32_t nowMs = millis();
    if (commandAuthority_.execute(command, nowMs).result !=
        TimerCommandResult::Applied) {
      sendError(409, "identify requires an open timer", "timer_active");
      return;
    }
    beginIdentify(nowMs);
    sendJson(200, "{\"identifying\":true,\"duration_ms\":5000}");
  });
  server_.on("/api/v1/system/diagnostics", HTTP_GET, [this]() { routeDiagnostics(); });
#if defined(SMART_TIMER_HARDWARE_V2)
  server_.on("/api/v1/bluetooth", HTTP_GET, [this]() { routeBluetooth(); });
  server_.on("/api/v1/bluetooth/enable", HTTP_GET, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    if (!timer_.isOpen()) {
      sendError(409, "Bluetooth enable requires an open timer", "timer_active");
      return;
    }
    BluetoothApplication* bluetooth = BluetoothApplication::active();
    if (bluetooth == nullptr || !bluetooth->setRadioEnabled(true, bootId_)) {
      sendError(503, "Bluetooth failed to initialize", "bluetooth_unavailable");
      return;
    }
    sendBluetoothStatus();
  });
  server_.on("/api/v1/bluetooth/disable", HTTP_GET, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    if (!timer_.isOpen()) {
      sendError(409, "Bluetooth disable requires an open timer",
                "timer_active");
      return;
    }
    BluetoothApplication* bluetooth = BluetoothApplication::active();
    if (bluetooth == nullptr || !bluetooth->setRadioEnabled(false, bootId_)) {
      sendError(503, "Bluetooth failed to stop", "bluetooth_unavailable");
      return;
    }
    sendBluetoothStatus();
  });
  server_.on("/api/v1/bluetooth/pairing/start", HTTP_POST, [this]() {
    handleBluetoothPairingStart();
  });
  server_.on("/api/v1/bluetooth/pairing/cancel", HTTP_POST, [this]() {
    handleBluetoothPairingCancel();
  });
  server_.on("/api/v1/bluetooth/pairing/revoke", HTTP_POST, [this]() {
    handleBluetoothPairingRevoke();
  });
  server_.on("/api/v1/bluetooth/apps", HTTP_GET, [this]() {
    if (allowRequest(false)) sendBluetoothStatus();
  });
  server_.on("/api/v1/bluetooth/apps/label", HTTP_PUT, [this]() {
    handleBluetoothAppUpdate(false);
  });
  server_.on("/api/v1/bluetooth/apps/revoke", HTTP_POST, [this]() {
    handleBluetoothAppUpdate(true);
  });
#endif
#if defined(SMART_TIMER_RELAY)
  registerRelayRoutes();
#endif
  server_.on("/api/v1/events", HTTP_GET, [this]() { routeEvents(); });
  server_.on("/api/v1/capabilities", HTTP_GET, [this]() { routeCapabilities(); });
  server_.on("/api/v1/device/name", HTTP_GET, [this]() { routeDeviceName(); });
  server_.on("/api/v1/device/name", HTTP_PUT, [this]() { routeDeviceNameSet(); });

  server_.on("/api/v1/ota/prepare", HTTP_POST,
             [this]() { handleOtaPrepare(); });

  registerOtaRoute("/api/v1/ota");
#if defined(SMART_TIMER_HARDWARE_V2)
  server_.on("/api/v1/ota/download", HTTP_POST, [this]() { routeOtaDownload(); });
#endif
}

void WebApplication::registerLegacyRoutes() {
  server_.on("/isAlive", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain",
                   timer_.isOpen() ? "open" : "closed");
    }
  });
  server_.on("/getVersion", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain", SMART_TIMER_VERSION);
    }
  });

  server_.on("/getTimer", HTTP_GET, [this]() {
    if (!allowRequest(false)) {
      return;
    }
    const TimerSnapshot status = timer_.snapshot(millis());
    const String lockState = status.state == TimerState::Open ? "open" : "closed";
    const String paused = status.state == TimerState::Paused ? "1" : "0";
    server_.send(200, "text/plain",
                 String(status.remainingMs) + ";" + lockState + ";" + paused);
  });

  server_.on("/setMagnetLock", HTTP_GET, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    const bool close = requestValue("t_state") == "1";
    const uint32_t duration = parseUnsigned(requestValue("i_state"));
    if (close && timer_.emergencyInputActive()) {
      server_.send(409, "text/plain", "emergency input active");
      return;
    }
    const TimerSnapshot before = timer_.snapshot(millis());
    // The shipped UI uses a zero-duration close as an intermediate step and
    // immediately follows it with an open request when an adjustment reaches
    // zero. Acknowledge that exact sequence without energizing anew.
    const bool legacyZeroAdjustment =
        close && duration == 0 && before.state != TimerState::Open &&
        !before.sealed;
    const bool success =
        close ? (legacyZeroAdjustment || timer_.setRemaining(duration, millis()))
              : timer_.stop(false, millis());
    if (!success) {
      server_.send(409, "text/plain", timer_.snapshot(millis()).sealed
                                                ? "sealed"
                                                : "invalid");
      return;
    }
    server_.send(200, "text/plain", close ? "closed" : "open");
  });
  server_.on("/pause", HTTP_GET, [this]() {
    if (allowRequest(true)) {
      const bool success = timer_.pause(millis());
      const String response =
          success ? String(timer_.snapshot(millis()).remainingMs) + ";closed"
                  : "failed";
      server_.send(success ? 200 : 409, "text/plain", response);
    }
  });
  server_.on("/resume", HTTP_GET, [this]() {
    if (allowRequest(true)) {
      const bool success = timer_.resume(millis());
      const String response =
          success ? String(timer_.snapshot(millis()).remainingMs) + ";closed"
                  : "failed";
      server_.send(success ? 200 : 409, "text/plain", response);
    }
  });
  server_.on("/setLockMode", HTTP_GET, [this]() {
    if (allowRequest(true)) {
      String value = requestValue("l_state");
      if (value.length() == 0) {
        value = requestValue("lock_mode");
      }
      const bool success =
          (value == "1" || value == "true") &&
          timer_.setSealed(true, millis());
      server_.send(success ? 200 : 409, "text/plain",
                   success ? "OK" : "failed");
    }
  });
  server_.on("/getLockMode", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain",
                   timer_.snapshot(millis()).sealed ? "1" : "0");
    }
  });
  server_.on("/getSsid", HTTP_GET, [this]() {
    if (!allowRequest(false)) {
      return;
    }
    if (!timer_.isOpen()) {
      server_.send(409, "text/plain", "timer active");
      return;
    }
    server_.send(200, "text/plain", network_.legacySsidList());
  });
  server_.on("/connect", HTTP_GET, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    if (!timer_.isOpen()) {
      server_.send(409, "text/plain", "timer active");
      return;
    }
    const String ssid = requestValue("ssid");
    const String password = requestValue("pw");
    if (ssid == "standalone-mode") {
      server_.send(200, "text/plain", "OK");
      scheduleWifiChange("", "", true);
      return;
    }
    if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
      server_.send(400, "text/plain", "invalid");
      return;
    }
    server_.send(202, "text/plain", "connecting");
    scheduleWifiChange(ssid, password, false);
  });
  server_.on("/getWifiMode", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain",
                   network_.stationConnected() ? "STA" : "AP");
    }
  });
  server_.on("/getDeviceId", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain", Platform::deviceId());
    }
  });
  server_.on("/reset", HTTP_GET, [this]() {
    if (!allowRequest(true)) {
      return;
    }
    if (!timer_.isOpen()) {
      server_.send(409, "text/plain", "timer active");
      return;
    }
    restartAtMs_ = millis() + 250;
    timer_.recordEvent(TimerEventType::RestartRequested, millis());
    server_.send(202, "text/plain", "restarting");
  });
  server_.on("/UpdateCheck", HTTP_GET, [this]() {
    if (allowRequest(false)) {
      server_.send(200, "text/plain", "false");
    }
  });
  server_.on("/UpdateRun", HTTP_POST, [this]() {
    if (allowRequest(true)) {
      server_.send(410, "text/plain", "cloud update disabled");
    }
  });
  server_.on("/update", HTTP_GET, [this]() { sendIndex(); });
  registerOtaRoute("/update");
}

bool WebApplication::readOperationId(String& operationId) {
  operationId = requestValue("operation_id");
  if (operationId.length() > 64 || !printableAscii(operationId)) {
    sendError(400, "operation_id must be at most 64 printable characters",
              "invalid_operation_id");
    return false;
  }
  return true;
}

String WebApplication::sessionId() const {
  char text[TimerCommandAuthority::kSessionTextSize];
  commandAuthority_.sessionText(text, sizeof(text));
  return String(text);
}

void WebApplication::handleTimerMutation(const char* action,
                                         TimerCommand command) {
  String operationId;
  if (!readOperationId(operationId)) {
    return;
  }
  // An exact successful replay is answered before any precondition so a
  // retry after a lost response stays unambiguous.
  const TimerOperationLedger::Match match = operations_.match(
      operationId.c_str(), command.op, command.valueA, command.valueB);
  if (match == TimerOperationLedger::Match::Conflict) {
    sendError(409, "operation_id was already used for another mutation",
              "operation_id_conflict");
    return;
  }
  if (match == TimerOperationLedger::Match::Duplicate) {
    sendMutationStatus(action, false, true, operationId);
    return;
  }
  command.boot = commandAuthority_.bootFromText(
      requestValue("if_boot_id").c_str());
  // Start has no session precondition over REST; if_session_id is ignored.
  if (command.op != TimerCommandOp::Start) {
    command.session = commandAuthority_.sessionFromText(
        requestValue("if_session_id").c_str());
  }
  const TimerCommandOutcome outcome =
      commandAuthority_.execute(command, millis());
  if (outcome.result != TimerCommandResult::Applied) {
    sendTimerCommandError(action, outcome.result);
    return;
  }
  operations_.remember(operationId.c_str(), command.op, command.valueA,
                       command.valueB);
  sendMutationStatus(action, outcome.changed, false, operationId);
}

// REST keeps its published error codes; the shared result decides which.
void WebApplication::sendTimerCommandError(const char* action,
                                           TimerCommandResult result) {
  const bool start = strcmp(action, "start") == 0;
  switch (result) {
    case TimerCommandResult::StaleBoot:
      sendError(409, "if_boot_id does not match the current device boot",
                "stale_boot");
      return;
    case TimerCommandResult::StaleSession:
      sendError(409, "if_session_id does not match the current timer session",
                "stale_session");
      return;
    case TimerCommandResult::EmergencyInputActive:
      sendError(409, "emergency input is active", "emergency_input_active");
      return;
    case TimerCommandResult::MaximumDurationReached:
      sendError(409, "maximum lock duration reached",
                "maximum_duration_reached");
      return;
    case TimerCommandResult::ControlDisabled:
      sendError(409, "this control is switched off in the timer's settings",
                "control_disabled");
      return;
    case TimerCommandResult::InvalidDuration:
      if (start) {
        sendError(400, "duration must be between 1 ms and 24 hours",
                  "invalid_duration");
      } else {
        sendError(400, "invalid adjustment", "invalid_adjustment");
      }
      return;
    case TimerCommandResult::InvalidState:
    case TimerCommandResult::ControlsLocked:
      if (start) {
        sendError(409, "timer is already active", "timer_active");
      } else {
        sendError(409, String(action) + " is not allowed in the current state",
                  String(action) + "_not_allowed");
      }
      return;
    case TimerCommandResult::InvalidParameters:
    case TimerCommandResult::Applied:
    default:
      sendError(400, "invalid timer command", "invalid_parameters");
      return;
  }
}

void WebApplication::handleLockControlsMutation(bool requireSealedValue) {
  if (!allowRequest(true)) {
    return;
  }
  if (requireSealedValue) {
    const String value = requestValue("sealed");
    if (value != "1" && value != "true") {
      sendError(400, "sealed must be true", "invalid_sealed_value");
      return;
    }
  }
  TimerCommand command;
  command.op = TimerCommandOp::LockControls;
  handleTimerMutation("lock_controls", command);
}

// Route handlers shared by HTTP and Bluetooth management (manage()).
void WebApplication::routeStatus() {
  if (allowRequest(false)) {
    sendStatus();
  }
}

void WebApplication::routeWifiScanStart() {
  if (!allowRequest(true)) {
    return;
  }
  if (network_.wifiSuspended()) {
    sendError(409, "restore Wi-Fi first", "wifi_disabled");
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "Wi-Fi scanning requires an open timer",
              "timer_active");
    return;
  }
  if (!network_.startScan()) {
    sendError(503, "unable to start Wi-Fi scan", "wifi_scan_failed");
    return;
  }
  sendJson(202, network_.scanJson());
}

void WebApplication::routeWifiScanResults() {
  if (!allowRequest(false)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "Wi-Fi scanning requires an open timer",
              "timer_active");
    return;
  }
  sendJson(200, network_.scanJson());
}

void WebApplication::routeWifiStatus() {
  if (allowRequest(false)) {
    sendWifiStatus();
  }
}

void WebApplication::sendSessionControls() {
  const DeviceConfig& settings = network_.settings();
  String body = "{\"allow_pause\":";
  body += settings.pauseAllowed() ? "true" : "false";
  body += ",\"allow_control_lock\":";
  body += settings.controlLockAllowed() ? "true" : "false";
  body += ",\"recovered\":";
  body += settings.sessionControlsRecovered() ? "true" : "false";
  body += "}";
  sendJson(200, body);
}

// Local owner setting. A session in progress keeps whatever pause or lock it
// already has, so the switches only change while the timer is open.
void WebApplication::routeSessionControlsSet() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "session-control settings require an open timer",
              "timer_active");
    return;
  }
  DeviceConfig& settings = network_.settings();
  bool allowPause = settings.pauseAllowed();
  bool allowControlLock = settings.controlLockAllowed();
  bool supplied = false;
  bool valid = true;
  auto read = [&](const char* name, bool& target) {
    const String value = requestValue(name);
    if (value.length() == 0) return;
    if (value == "true" || value == "1") target = true;
    else if (value == "false" || value == "0") target = false;
    else valid = false;
    supplied = true;
  };
  read("allow_pause", allowPause);
  read("allow_control_lock", allowControlLock);
  if (!valid || !supplied) {
    sendError(400, "send allow_pause and/or allow_control_lock as booleans",
              "invalid_field");
    return;
  }
  if (!settings.setSessionControls(allowPause, allowControlLock)) {
    sendError(503, "could not save settings; previous settings retained",
              "storage_failed");
    return;
  }
  timer_.setControlPolicy(allowPause, allowControlLock);
  sendSessionControls();
}

void WebApplication::routeAccessPoint() {
  if (allowRequest(false)) {
    sendAccessPointConfig();
  }
}

void WebApplication::routeAccessPointSet() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "access-point changes require an open timer",
              "timer_active");
    return;
  }
  const String security = requestValue("security");
  const String useDeviceNameValue = requestValue("use_device_name");
  const bool useDeviceName = useDeviceNameValue == "1" ||
                             useDeviceNameValue == "true";
  const String keepPasswordValue = requestValue("keep_password");
  const bool keepPassword = security == "wpa2" &&
                            (keepPasswordValue == "1" ||
                             keepPasswordValue == "true");
  const String password =
      security == "wpa2" ? requestValue("password") : String();
  const String standaloneValue = requestValue("standalone");
  const bool standalone = standaloneValue == "1" ||
                          standaloneValue == "true";
  if ((security != "open" && security != "wpa2") ||
      (security == "wpa2" && password.length() == 0 &&
       (!keepPassword || !network_.accessPointPasswordSet()))) {
    sendError(400,
              "use a 1-32 character SSID and an 8-63 character WPA2 password",
              "invalid_access_point_settings");
    return;
  }
  if (!network_.setAccessPointConfig(requestValue("ssid"), password,
                                    useDeviceName, keepPassword, standalone)) {
    if (network_.configurationSaveFailed()) {
      sendError(503, "could not save settings; previous settings retained", "storage_failed");
    } else {
      sendError(400, "use a 1-32 character SSID and an 8-63 character WPA2 password",
                "invalid_access_point_settings");
    }
    return;
  }
  timer_.recordEvent(TimerEventType::AccessPointConfigured, millis());
  restartAtMs_ = millis() + 1500;
  String body = "{\"updated\":true,\"restarting\":true,\"ap_ssid\":\"";
  body += jsonEscape(network_.accessPointName()) + "\",\"ap_security\":\"";
  body += jsonEscape(network_.accessPointSecurity()) +
          "\",\"standalone\":" + String(standalone ? "true" : "false") +
          "}";
  sendJson(202, body);
}

void WebApplication::routeWifiConfigure() {
  if (!allowRequest(true)) {
    return;
  }
  if (network_.wifiSuspended()) {
    sendError(409, "restore Wi-Fi first", "wifi_disabled");
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "network changes require an open timer",
              "timer_active");
    return;
  }
  if (network_.scanInProgress()) {
    sendError(409, "wait for the Wi-Fi scan to finish",
              "wifi_scan_in_progress");
    return;
  }
  const String ssid = requestValue("ssid");
  const String password = requestValue("password");
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    sendError(400, "invalid Wi-Fi credentials", "invalid_wifi_credentials");
    return;
  }
  sendJson(202, "{\"connecting\":true,\"wifi_state\":\"connecting\"}");
  scheduleWifiChange(ssid, password, false);
}

void WebApplication::routeWifiForget() {
  if (!allowRequest(true)) {
    return;
  }
  if (network_.wifiSuspended()) {
    sendError(409, "restore Wi-Fi first", "wifi_disabled");
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "network changes require an open timer",
              "timer_active");
    return;
  }
  sendJson(202, "{\"standalone\":true}");
  scheduleWifiChange("", "", true);
}

void WebApplication::routeRestart() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "restart requires an open timer", "timer_active");
    return;
  }
  restartAtMs_ = millis() + 250;
  timer_.recordEvent(TimerEventType::RestartRequested, millis());
  sendJson(202, "{\"restarting\":true}");
}

void WebApplication::routeDiagnostics() {
  if (allowRequest(false)) {
    sendDiagnostics();
  }
}

#if defined(SMART_TIMER_HARDWARE_V2)
void WebApplication::routeBluetooth() {
  if (allowRequest(false)) {
    sendBluetoothStatus();
  }
}
#endif

void WebApplication::routeEvents() {
  if (allowRequest(false)) {
    sendEvents();
  }
}

void WebApplication::routeCapabilities() {
  if (allowRequest(false)) {
    sendCapabilities();
  }
}

void WebApplication::routeDeviceName() {
  if (allowRequest(false)) {
    sendDeviceName();
  }
}

void WebApplication::routeDeviceNameSet() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "device naming requires an open timer", "timer_active");
    return;
  }
  if (!network_.setDeviceName(requestValue("name"))) {
    if (network_.configurationSaveFailed()) {
      sendError(503, "could not save settings; previous settings retained", "storage_failed");
    } else {
      sendError(400, "name must be 1-32 printable characters", "invalid_device_name");
    }
    return;
  }
  timer_.recordEvent(TimerEventType::DeviceRenamed, millis());
  restartAtMs_ = millis() + 500;
  String body = "{\"updated\":true,\"restarting\":true,\"name\":\"";
  body += jsonEscape(network_.deviceName()) + "\",\"hostname\":\"";
  body += jsonEscape(network_.hostName()) + "\",\"ap_ssid\":\"";
  body += jsonEscape(network_.accessPointName()) + "\",\"ap_security\":\"";
  body += jsonEscape(network_.accessPointSecurity()) + "\"}";
  sendJson(200, body);
}
