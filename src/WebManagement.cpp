#include "WebApplication.h"

#if defined(SMART_TIMER_HARDWARE_V2)

#include <Update.h>
#include <esp_ota_ops.h>

#include "Hardware.h"
#include "LocalNetwork.h"
#include "TimerController.h"

// Bluetooth device management (docs/BLUETOOTH_MANAGEMENT.md). manage() runs the
// same route handler as the HTTP endpoint, with the reply captured.

namespace {
struct RouteInfo {
  const char* path;
  bool secretAllowed;
};

RouteInfo routeInfo(WebApplication::ManagementRoute route) {
  using Route = WebApplication::ManagementRoute;
  switch (route) {
    case Route::Status: return {"/api/v1/status", false};
    case Route::WifiStatus: return {"/api/v1/wifi/status", false};
    case Route::AccessPoint: return {"/api/v1/wifi/ap", false};
    case Route::AccessPointSet: return {"/api/v1/wifi/ap", true};
    case Route::DeviceName:
    case Route::DeviceNameSet: return {"/api/v1/device/name", false};
    case Route::Bluetooth: return {"/api/v1/bluetooth", false};
    case Route::Diagnostics: return {"/api/v1/system/diagnostics", false};
    case Route::Events: return {"/api/v1/events", false};
    case Route::Capabilities: return {"/api/v1/capabilities", false};
    case Route::WifiConfigure: return {"/api/v1/wifi/configure", true};
    // Internal validation names only: no HTTP routes expose this switch.
    case Route::WifiSuspend: return {"bluetooth:wifi-suspend", false};
    case Route::WifiResume: return {"bluetooth:wifi-resume", false};
    case Route::WifiForget: return {"/api/v1/wifi/forget", false};
    case Route::Restart: return {"/api/v1/system/restart", false};
    case Route::AppLabel: return {"/api/v1/bluetooth/apps/label", false};
    case Route::AppRevoke: return {"/api/v1/bluetooth/apps/revoke", false};
    case Route::PairingStart: return {"/api/v1/bluetooth/pairing/start", false};
    case Route::PairingCancel: return {"/api/v1/bluetooth/pairing/cancel", false};
    case Route::PairingRevoke: return {"/api/v1/bluetooth/pairing/revoke", false};
    case Route::WifiScanStart:
    case Route::WifiScanResults: return {"/api/v1/wifi/scan", false};
    case Route::OtaBegin: return {"/api/v1/ota/prepare", false};
    case Route::OtaFinish:
    case Route::OtaAbort: return {"/api/v1/ota", false};
    case Route::OtaDownload: return {"/api/v1/ota/download", false};
  }
  return {"", false};
}
}  // namespace

WebApplication::ManagementReply WebApplication::manage(
    ManagementRoute route, const char* body, size_t bodyLength,
    const char* secret, size_t secretLength) {
  ManagementReply reply;
  const RouteInfo info = routeInfo(route);
  requestFields_.clear();
  requestIsJson_ = false;
  capture_ = &reply;
  if (bodyLength != 0 &&
      (!requestFields_.parse(body, bodyLength) ||
       !requestFields_.validateForRoute(info.path))) {
    sendRequestFieldError();
  } else if (requestFields_.find("password") != nullptr) {
    // Passwords travel only in the encrypted secret, never in the JSON.
    sendError(400, "send passwords in the encrypted secret field", "invalid_field");
  } else if (secretLength != 0 && !info.secretAllowed) {
    sendError(400, "this operation takes no secret", "invalid_field");
  } else {
    requestIsJson_ = true;
    captureSecret_ = secretLength != 0 ? secret : nullptr;
    captureSecretLength_ = secretLength;
    switch (route) {
      case ManagementRoute::Status: routeStatus(); break;
      case ManagementRoute::WifiStatus: routeWifiStatus(); break;
      case ManagementRoute::AccessPoint: routeAccessPoint(); break;
      case ManagementRoute::AccessPointSet: routeAccessPointSet(); break;
      case ManagementRoute::DeviceName: routeDeviceName(); break;
      case ManagementRoute::DeviceNameSet: routeDeviceNameSet(); break;
      case ManagementRoute::Bluetooth: routeBluetooth(); break;
      case ManagementRoute::Diagnostics: routeDiagnostics(); break;
      case ManagementRoute::Events: routeEvents(); break;
      case ManagementRoute::Capabilities: routeCapabilities(); break;
      case ManagementRoute::WifiConfigure: routeWifiConfigure(); break;
      case ManagementRoute::WifiSuspend: routeBluetoothWifiMode(true); break;
      case ManagementRoute::WifiResume: routeBluetoothWifiMode(false); break;
      case ManagementRoute::WifiForget: routeWifiForget(); break;
      case ManagementRoute::Restart: routeRestart(); break;
      case ManagementRoute::AppLabel: handleBluetoothAppUpdate(false); break;
      case ManagementRoute::AppRevoke: handleBluetoothAppUpdate(true); break;
      case ManagementRoute::PairingStart: handleBluetoothPairingStart(); break;
      case ManagementRoute::PairingCancel: handleBluetoothPairingCancel(); break;
      case ManagementRoute::PairingRevoke: handleBluetoothPairingRevoke(); break;
      case ManagementRoute::WifiScanStart: routeWifiScanStart(); break;
      case ManagementRoute::WifiScanResults: routeWifiScanResults(); break;
      case ManagementRoute::OtaBegin: routeBluetoothOtaBegin(); break;
      case ManagementRoute::OtaFinish: routeBluetoothOtaFinish(); break;
      case ManagementRoute::OtaAbort: routeBluetoothOtaAbort(); break;
      case ManagementRoute::OtaDownload: routeOtaDownload(); break;
    }
  }
  if (reply.body.length() == 0) {
    sendError(500, "the operation produced no reply", "internal_error");
  }
  capture_ = nullptr;
  captureSecret_ = nullptr;
  captureSecretLength_ = 0;
  requestFields_.clear();
  requestIsJson_ = false;
  return reply;
}

// Reachable only through authenticated Bluetooth management, on the main loop.
// No persistent configuration is changed and no restart is scheduled.
void WebApplication::routeBluetoothWifiMode(bool suspended) {
  if (!allowRequest(true)) return;
  if (!timer_.isOpen()) {
    sendError(409, "network changes require an open timer", "timer_active");
    return;
  }
  if (suspended) {
    // First-boot OTA validation needs a working recovery network. Do not let
    // a temporary preference delay validation and trigger rollback on reboot.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
      sendError(409, "wait for post-update startup checks before turning Wi-Fi off", "boot_validation_pending");
      return;
    }
  }
  if (!network_.setWifiSuspended(suspended)) {
    sendError(409, "unable to change network mode; wait for Wi-Fi connection or scan to finish", "network_mode_busy");
    return;
  }
  sendWifiStatus();
}

// The Bluetooth update keeps every rule of the multipart upload: an open timer,
// the physical Reset-button arm (consumed here), no scan, valid metadata, the
// same streaming checks and the same held-back final chunk.
void WebApplication::routeBluetoothOtaBegin() {
  const uint32_t nowMs = millis();
  if (bluetoothOtaActive_ || otaQuietActive(nowMs)) {
    sendError(409, "firmware update in progress", "ota_in_progress");
    return;
  }
  if (mutationTransitionActive(nowMs)) {
    sendError(409, "device transition already pending");
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "firmware updates require an open timer", "timer_active");
    return;
  }
  if (network_.scanInProgress()) {
    sendError(409, "wait for the Wi-Fi scan to finish before updating",
              "wifi_scan_in_progress");
    return;
  }
  if (!otaArmed(nowMs)) {
    sendError(403,
              "press and release the Reset button within two seconds to arm OTA while open",
              "ota_not_armed");
    return;
  }
  otaResetUploadState();
  if (requestValue("release_signature").length() && !ReleaseUpdate::hasVerificationMemory()) {
    sendError(503, "not enough memory to verify the release; close other control pages and retry",
              "release_verification_busy");
    return;
  }
  const bool unverifiedAllowed = otaUnverifiedAllowed(nowMs);
  otaArmedUntilMs_ = 0;
  otaUnverifiedAllowed_ = false;
  if (!parseOtaMetadata(otaExpectedSize_, otaExpectedCrc32_)) {
    sendError(400, "missing or invalid firmware size/checksum metadata",
              "invalid_ota_metadata");
    return;
  }
  otaAllowed_ = true;
  if (!parseReleaseMetadata(otaExpectedSize_, otaExpectedCrc32_, otaRelease_, otaSignedRelease_)) {
    otaAllowed_ = false;
    sendError(400, "invalid release signature", "invalid_release_signature");
    return;
  }
  otaUploadUnverifiedAllowed_ = unverifiedAllowed;
  if (!otaBeginImage(nowMs)) {
    otaAllowed_ = false;
    otaQuietUntilMs_ = 0;
    sendError(503, otaError_, "ota_start_failed");
    return;
  }
  bluetoothOtaActive_ = true;
  sendJson(202, "{\"chunk_bytes\":" + String(kBluetoothOtaChunkBytes) +
                    ",\"image_size\":" + String(otaExpectedSize_) +
                    ",\"unverified_firmware_allowed\":" +
                    String(unverifiedAllowed ? "true" : "false") + "}");
}

WebApplication::ManagementReply WebApplication::manageOtaChunk(
    uint32_t offset, const uint8_t* data, size_t length) {
  ManagementReply reply;
  capture_ = &reply;
  const uint32_t pendingOffset = otaReceivedSize_ - otaPendingChunkLength_;
  if (!bluetoothOtaActive_) {
    sendError(409, "start the firmware update first", "ota_not_started");
  } else if (length == 0 || length > kBluetoothOtaChunkBytes) {
    sendError(400, "firmware chunks carry 1-472 bytes", "invalid_ota_chunk");
  } else if (otaPendingChunkLength_ != 0 && offset == pendingOffset &&
             length == otaPendingChunkLength_ &&
             memcmp(data, otaPendingChunk_, length) == 0) {
    // Exact resend of the previous chunk after a lost reply: already held.
    otaQuietUntilMs_ = millis() + kOtaUploadQuietWindowMs;
    sendJson(200, "{\"accepted\":" + String(otaReceivedSize_) + ",\"duplicate\":true}");
  } else if (offset != otaReceivedSize_) {
    const TimerSnapshot timerStatus = timer_.snapshot(millis());
    sendJson(409, "{\"error\":\"firmware chunk offset does not follow the accepted bytes\","
                  "\"code\":\"ota_offset_mismatch\",\"state\":\"" +
                      String(stateName(static_cast<uint8_t>(timerStatus.state))) +
                      "\",\"expected_offset\":" + String(otaReceivedSize_) + "}");
  } else {
    otaAcceptBytes(data, length);
    if (otaError_.length() != 0) {
      const String error = otaError_;
      failBluetoothOta();
      sendError(400, error, "ota_failed");
    } else {
      sendJson(200, "{\"accepted\":" + String(otaReceivedSize_) + ",\"duplicate\":false}");
    }
  }
  capture_ = nullptr;
  return reply;
}

void WebApplication::failBluetoothOta() {
  otaImageHash_.clear();
  if (Update.isRunning()) {
    Update.end();
  }
  bluetoothOtaActive_ = false;
  otaAllowed_ = false;
  otaInstallActive_ = false;
  otaQuietUntilMs_ = 0;
  otaPendingChunkLength_ = 0;
  timer_.recordEvent(TimerEventType::OtaFailed, millis());
}

void WebApplication::routeBluetoothOtaFinish() {
  if (!bluetoothOtaActive_) {
    sendError(409, "start the firmware update first", "ota_not_started");
    return;
  }
  otaCompleteImage();
  bluetoothOtaActive_ = false;
  otaAllowed_ = false;
  if (!otaSucceeded_) {
    const String error = otaError_.length() == 0 ? String("update failed") : otaError_;
    failBluetoothOta();
    sendError(400, error, "ota_failed");
    return;
  }
  timer_.recordEvent(TimerEventType::OtaSucceeded, millis(),
                     static_cast<int32_t>(otaReceivedSize_));
  restartAtMs_ = millis() + 1500;
  sendJson(200, "{\"updated\":true,\"restarting\":true}");
}

void WebApplication::routeBluetoothOtaAbort() {
  if (!bluetoothOtaActive_) {
    sendJson(200, "{\"aborted\":false}");
    return;
  }
  otaAbortImage();
  failBluetoothOta();
  sendJson(200, "{\"aborted\":true}");
}

#endif  // SMART_TIMER_HARDWARE_V2
