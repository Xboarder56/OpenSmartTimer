#if defined(ESP8266)
#include <lwip/priv/tcp_priv.h>
#endif
#include "ResetTrace.h"
#include "WebApplication.h"

#if defined(ESP8266)
#include <Updater.h>
#else
#include <Update.h>
#include <esp_task_wdt.h>
#endif

#include "Hardware.h"
#if defined(SMART_TIMER_HARDWARE_V2)
#include "BluetoothApplication.h"
#endif
#include "LocalNetwork.h"
#include "OtaImageValidation.h"
#include "ReleaseUpdateConfig.h"
#include "Platform.h"
#include "TimerController.h"
#include "UpdateDownload.h"
#include "generated_build_identity.h"
#include "generated_web_assets.h"

#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

namespace {
constexpr uint32_t kOtaPrepareWindowMs = 10UL * 1000UL;
#if defined(SMART_TIMER_HARDWARE_V2)
constexpr uint32_t kVendorRollbackSize = 1287440;
constexpr uint32_t kVendorRollbackCrc32 = 0xBF421EF6;
#else
constexpr uint32_t kLegacy030Size = 483472;
constexpr uint32_t kLegacy030Crc32 = 0x4D66232B;
constexpr uint32_t kLegacy130Size = 493136;
constexpr uint32_t kLegacy130Crc32 = 0xC7225497;
#endif

uint32_t parseUnsigned(const String& value) {
  uint32_t result;
  return HttpRequestFields::unsignedInteger(value.c_str(), result) ? result : 0;
}

String updateErrorString() {
#if defined(ESP8266)
  return Update.getErrorString();
#else
  return String(Update.errorString());
#endif
}
}  // namespace

WebApplication::WebApplication(TimerController& timer, LocalNetwork& network)
    : timer_(timer), network_(network), commandAuthority_(timer) {}

void WebApplication::begin() {
#if defined(SMART_TIMER_HARDWARE_V2)
  ready_ = false;
#endif
  bootId_ = Platform::random32() ^ Platform::numericDeviceId() ^ micros();
  if (bootId_ == 0) {
    bootId_ = 1;
  }
  commandAuthority_.begin(bootId_);
  minimumObservedFreeHeapBytes_ = Platform::minimumFreeHeapBytes();
  const char* headers[] = {"Origin", "Host", "Sec-Fetch-Site", "Content-Type"};
  server_.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  registerEventStreamHook();
  registerStaticRoutes();
  registerModernRoutes();
  registerLegacyRoutes();

  server_.onNotFound([this]() {
    if (otaQuietActive(millis())) {
      sendOtaBusy();
      return;
    }
    if (server_.uri().startsWith("/api/")) {
      sendError(404, "not found");
      return;
    }
    if (server_.method() == HTTP_GET) {
      sendIndex();
    } else {
      sendError(404, "not found");
    }
  });
  server_.begin();
#if defined(SMART_TIMER_HARDWARE_V2)
  ready_ = true;
#endif
}

void WebApplication::noteHeapLowWater(uint32_t freeHeap) {
  HeapLowWater snapshot;
  snapshot.freeBytes = freeHeap;
  snapshot.atMs = millis();
  for (const EventStreamClient& slot : eventStreamClients_) {
    if (slot.acceptedAtMs == 0) continue;
    ++snapshot.sseClients;
    snapshot.ssePendingBytes += slot.pendingWire.length();
  }
#if defined(ESP8266)
  for (const tcp_pcb* pcb = tcp_active_pcbs; pcb != nullptr; pcb = pcb->next) {
    if (snapshot.tcpActive < 255) ++snapshot.tcpActive;
    snapshot.tcpQueuedBytes += TCP_SND_BUF - pcb->snd_buf;
  }
  for (const tcp_pcb* pcb = tcp_tw_pcbs; pcb != nullptr; pcb = pcb->next) {
    if (snapshot.tcpTimeWait < 255) ++snapshot.tcpTimeWait;
  }
#endif
  strncpy(snapshot.lastUri, ResetTrace::current().uri, sizeof(snapshot.lastUri) - 1);
  heapLowWater_ = snapshot;
}

void WebApplication::loop(uint32_t nowMs) {
  const uint32_t freeHeap = Platform::minimumFreeHeapBytes();
  if (minimumObservedFreeHeapBytes_ == 0 ||
      freeHeap < minimumObservedFreeHeapBytes_) {
    minimumObservedFreeHeapBytes_ = freeHeap;
    noteHeapLowWater(freeHeap);
  }
  ResetTrace::mark(ResetTrace::kWeb);
  server_.handleClient();
  ResetTrace::mark(ResetTrace::kWebAfter);
  requestFields_.clear();
  requestIsJson_ = false;
  nowMs = millis();  // Accepted sockets may be newer than the caller's tick.
  const String wifiState = network_.stationState(nowMs);
  if (wifiState != lastObservedWifiState_) {
    if (wifiState == "connecting") {
      timer_.recordEvent(TimerEventType::WifiConnecting, nowMs);
    } else if (wifiState == "connected") {
      timer_.recordEvent(TimerEventType::WifiConnected, nowMs);
    } else if (wifiState == "failed") {
      timer_.recordEvent(TimerEventType::WifiFailed, nowMs);
    } else if (wifiState == "standalone") {
      timer_.recordEvent(TimerEventType::WifiStandalone, nowMs);
    }
    lastObservedWifiState_ = wifiState;
  }
  const uint32_t unexpectedDisconnects =
      network_.stationUnexpectedDisconnectCount();
  if (unexpectedDisconnects != lastObservedWifiUnexpectedDisconnectCount_) {
    timer_.recordEvent(TimerEventType::WifiDisconnected, nowMs,
                       network_.lastStationDisconnectReason());
    lastObservedWifiUnexpectedDisconnectCount_ = unexpectedDisconnects;
  }
  const uint32_t dhcpTimeouts = network_.stationDhcpTimeoutCount();
  if (dhcpTimeouts != lastObservedWifiDhcpTimeoutCount_) {
    timer_.recordEvent(TimerEventType::WifiDhcpTimeout, nowMs);
    lastObservedWifiDhcpTimeoutCount_ = dhcpTimeouts;
  }
  // Give the acknowledgement time to drain before changing its network or
  // rebooting. A stalled V1 response is aborted after a bounded five seconds.
  bool responsePending = false;
#if defined(ESP8266)
  responsePending = server_.responsePending();
#endif
  if (pendingNetworkAtMs_ != 0 &&
      static_cast<int32_t>(nowMs - pendingNetworkAtMs_) >= 0 && !responsePending) {
    pendingNetworkAtMs_ = 0;
    if (pendingStandalone_) {
      network_.enterStandaloneMode();
    } else {
      network_.setStationCredentials(pendingSsid_, pendingWifiPassword_);
    }
    pendingSsid_ = "";
    pendingWifiPassword_ = "";
    pendingStandalone_ = false;
  }
#if defined(SMART_TIMER_HARDWARE_V2)
  serviceUpdateDownload(nowMs);
#endif
  if (bluetoothOtaActive_ && !otaQuietActive(nowMs)) {
    // The Bluetooth sender stopped for the whole quiet window: cancel the
    // update and keep the running firmware.
    otaAbortImage();
    bluetoothOtaActive_ = false;
    otaAllowed_ = false;
    otaPendingChunkLength_ = 0;
    timer_.recordEvent(TimerEventType::OtaFailed, nowMs);
  }
  if (restartAtMs_ != 0 && static_cast<int32_t>(nowMs - restartAtMs_) >= 0 &&
      !responsePending) {
    if (!timer_.isOpen()) {
      restartAtMs_ = 0;
      return;
    }
    Hardware::writeLock(false);
    delay(25);
    ESP.restart();
  }
  observeState(nowMs, &wifiState);
  ResetTrace::mark(ResetTrace::kWebStreams);
  serviceEventStreams(nowMs);
}

void WebApplication::observeState(uint32_t nowMs,
                                  const String* knownWifiState) {
  const TimerSnapshot timerStatus = timer_.snapshot(nowMs);
  const String wifiState = knownWifiState != nullptr
                               ? *knownWifiState
                               : network_.stationState(nowMs);
  uint16_t flags = static_cast<uint8_t>(timerStatus.state) & 0x03;
  flags |= timerStatus.sealed ? 0x04 : 0;
  flags |= timerStatus.emergencyInputActive ? 0x08 : 0;
  flags |= network_.standaloneMode() ? 0x10 : 0;
  flags |= network_.accessPointRunning() ? 0x20 : 0;
  flags |= network_.stationConnected() ? 0x40 : 0;
  flags |= otaArmed(nowMs) || identifying(nowMs) ? 0x80 : 0;
  flags |= otaUnverifiedAllowed(nowMs) ? 0x100 : 0;
  flags |= timerStatus.pauseAllowed ? 0 : 0x200;
  flags |= timerStatus.sealAllowed ? 0 : 0x400;
#if defined(SMART_TIMER_HARDWARE_V2)
  // Pages learn at once that a download began, failed or is installing.
  flags |= static_cast<uint16_t>(UpdateDownload::progress().phase) << 11;
#endif
  const uint32_t timerRevision = timer_.eventRevision();
  const uint8_t accessPointClients = network_.accessPointClients();

  const bool changed = stateObservationInitialized_ &&
      (timerRevision != observedTimerRevision_ ||
       wifiState != observedRevisionWifiState_ ||
       accessPointClients != observedAccessPointClients_ ||
       flags != observedStateFlags_);
  observedTimerRevision_ = timerRevision;
  observedRevisionWifiState_ = wifiState;
  observedAccessPointClients_ = accessPointClients;
  observedStateFlags_ = flags;
  if (!stateObservationInitialized_) {
    stateObservationInitialized_ = true;
  } else if (changed) {
    ++stateRevision_;
    if (stateRevision_ == 0) {
      stateRevision_ = 1;
    }
  }
}

void WebApplication::armOta(uint32_t nowMs) {
  if (timer_.isOpen() && !mutationTransitionActive(nowMs)) {
    if (otaArmed(nowMs)) {
      // A second physical short press deliberately opens the escape hatch for
      // one future application upload. It remains volatile and does not extend
      // the original five-minute authorization window.
      otaUnverifiedAllowed_ = true;
    } else {
      otaArmedUntilMs_ = nowMs + kOtaArmWindowMs;
      otaUnverifiedAllowed_ = false;
    }
  }
}

bool WebApplication::otaArmed(uint32_t nowMs) const {
  return otaArmedUntilMs_ != 0 &&
         static_cast<int32_t>(otaArmedUntilMs_ - nowMs) > 0;
}

bool WebApplication::otaUnverifiedAllowed(uint32_t nowMs) const {
  return otaUnverifiedAllowed_ && otaArmed(nowMs);
}

uint32_t WebApplication::otaArmedRemainingMs(uint32_t nowMs) const {
  if (!otaArmed(nowMs)) {
    return 0;
  }
  return otaArmedUntilMs_ - nowMs;
}

bool WebApplication::identifying(uint32_t nowMs) const {
  return identifyUntilMs_ != 0 &&
         static_cast<int32_t>(identifyUntilMs_ - nowMs) > 0;
}

bool WebApplication::identifyLedOn(uint32_t nowMs) const {
  return identifying(nowMs) && ((nowMs / 125UL) % 2UL == 0);
}

void WebApplication::registerStaticRoutes() {
  server_.on("/", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/index.html", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/styles.css", HTTP_GET, [this]() {
    sendAsset(web_web_styles_css, web_web_styles_css_len, "text/css", false);
  });
  server_.on("/app.js", HTTP_GET, [this]() {
    sendAsset(web_web_app_js, web_web_app_js_len,
              "application/javascript", false);
  });
  server_.on("/appearance.js", HTTP_GET, [this]() {
    sendAsset(web_web_appearance_js, web_web_appearance_js_len,
              "application/javascript", false);
  });
  server_.on("/updates.js", HTTP_GET, [this]() {
    sendAsset(web_web_updates_js, web_web_updates_js_len,
              "application/javascript", false);
  });
  server_.on("/i18n.js", HTTP_GET, [this]() {
    sendAsset(web_web_i18n_js, web_web_i18n_js_len,
              "application/javascript", false);
  });

  // Common captive-portal probes. All of them land on the local application.
  server_.on("/generate_204", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/fwlink", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/connecttest.txt", HTTP_GET, [this]() { sendIndex(); });
}

void WebApplication::registerOtaRoute(const String& path) {
  server_.on(path, HTTP_POST, [this]() { handleOtaComplete(); },
             [this]() { handleOtaUpload(); });
}

void WebApplication::sendIndex() {
  // Mirrors the page's meta policy; frame-ancestors works only as a header.
  server_.sendHeader("Content-Security-Policy",
                     "default-src 'none'; script-src 'self'; style-src 'self'; "
                     "img-src 'self' data:; connect-src 'self' " SMART_TIMER_UPDATE_BASE_URL
                     " https://api.github.com; form-action 'self'; "
                     "base-uri 'none'; object-src 'none'; frame-ancestors 'none'");
  sendAsset(web_web_index_html, web_web_index_html_len, "text/html", false);
}

void WebApplication::sendAsset(const uint8_t* data, size_t length,
                               const char* mimeType, bool cache) {
  if (otaQuietActive(millis())) {
    sendOtaBusy();
    return;
  }
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control",
                     cache ? "public, max-age=86400" : "no-store");
  server_.send_P(200, mimeType, reinterpret_cast<PGM_P>(data), length);
}

void WebApplication::sendJson(int status, const String& body) {
  if (capture_ != nullptr) {
    capture_->status = status;
    capture_->body = body;
    return;
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(status, "application/json", body);
}

void WebApplication::sendError(int status, const String& message,
                               const String& code) {
  const TimerSnapshot timerStatus = timer_.snapshot(millis());
  String body;
  body.reserve(message.length() + code.length() + 80);
  body = "{\"error\":\"" + jsonEscape(message) + "\",\"code\":\"";
  body += jsonEscape(code) + "\",\"state\":\"";
  body += stateName(static_cast<uint8_t>(timerStatus.state));
  body += "\"}";
  sendJson(status, body);
}

void WebApplication::sendOtaBusy() {
  const TimerSnapshot timerStatus = timer_.snapshot(millis());
  if (capture_ == nullptr) server_.sendHeader("Retry-After", "30");
  String body =
      "{\"error\":\"firmware update in progress\",\"code\":\"ota_in_progress\",\"state\":\"";
  body += stateName(static_cast<uint8_t>(timerStatus.state));
  body += "\",\"retry_after_ms\":" + String(kOtaOtherClientRetryMs) + "}";
  sendJson(503, body);
}

bool WebApplication::otaQuietActive(uint32_t nowMs) const {
  return otaQuietUntilMs_ != 0 &&
         static_cast<int32_t>(otaQuietUntilMs_ - nowMs) > 0;
}

bool WebApplication::mutationTransitionActive(uint32_t nowMs, bool includeInterlock) const {
  if (includeInterlock && mutationInterlockActive()) return true;
  // A V2 download ends in a restart, so nothing may start a session first.
  return otaQuietActive(nowMs) || restartAtMs_ != 0 ||
         pendingNetworkAtMs_ != 0 || UpdateDownload::active();
}

bool WebApplication::parseOtaMetadata(uint32_t& size,
                                      uint32_t& checksum) const {
  const uint32_t freeSketchSpace = Platform::freeSketchSpaceBytes();
  if (freeSketchSpace <= 0x1000) {
    return false;
  }
  const uint32_t maximumSketchSpace =
      (freeSketchSpace - 0x1000) & 0xFFFFF000;
  size = parseUnsigned(requestValue("image_size"));
  const String expectedCrc = requestValue("image_crc32");
  char* crcEnd = nullptr;
  checksum = strtoul(expectedCrc.c_str(), &crcEnd, 16);
  return size != 0 && size <= maximumSketchSpace &&
         expectedCrc.length() == 8 && crcEnd != expectedCrc.c_str() &&
         *crcEnd == '\0';
}

bool WebApplication::parseReleaseMetadata(uint32_t size, uint32_t checksum,
    ReleaseUpdate::Descriptor& descriptor, bool& signedRelease) const {
  const String version = requestValue("release_version");
  const String sha = requestValue("image_sha256");
  const String signature = requestValue("release_signature");
  signedRelease = version.length() || sha.length() || signature.length();
  if (!signedRelease) return true;  // Local file/recovery uploads remain available.
  if (!ReleaseUpdate::configured() || !ReleaseUpdate::validVersion(version.c_str()) ||
      !ReleaseUpdate::decodeHex(sha.c_str(), descriptor.sha256, sizeof(descriptor.sha256)) ||
      !ReleaseUpdate::decodeHex(signature.c_str(), descriptor.signature, sizeof(descriptor.signature))) return false;
  strcpy(descriptor.version, version.c_str());
  descriptor.size = size;
  descriptor.crc32 = checksum;
  return ReleaseUpdate::verify(descriptor, ReleaseUpdate::target(), SMART_TIMER_UPDATE_PUBLIC_KEY);
}

void WebApplication::otaResetUploadState() {
  otaImageHash_.clear();
  otaSucceeded_ = false;
  otaInstallActive_ = false;
  otaError_ = "";
  otaHeaderLength_ = 0;
  otaMarkerMatched_ = 0;
  otaHeaderValid_ = false;
  otaMarkerFound_ = false;
  otaUploadUnverifiedAllowed_ = false;
  otaReceivedSize_ = 0;
  otaCrc32_ = 0xFFFFFFFFUL;
  otaPendingChunkLength_ = 0;
  otaSignedRelease_ = false;
}

bool WebApplication::otaBeginImage(uint32_t nowMs, bool quiet) {
  otaPreparedSize_ = 0;
  otaPreparedCrc32_ = 0;
  otaPrepareExpiresAtMs_ = 0;
  otaPreparedSignedRelease_ = false;
  network_.discardScanResults();
  if (quiet) otaQuietUntilMs_ = nowMs + kOtaUploadQuietWindowMs;
  if (!Update.begin(otaExpectedSize_)) {
    otaError_ = "unable to start update";
    return false;
  }
  otaInstallActive_ = true;
  if (otaSignedRelease_) otaImageHash_.begin();
  Hardware::writeStatus(true);
  timer_.recordEvent(TimerEventType::OtaStarted, millis(),
                     static_cast<int32_t>(otaExpectedSize_));
  return true;
}

void WebApplication::otaAcceptBytes(const uint8_t* data, size_t length, bool quiet) {
  if (quiet) otaQuietUntilMs_ = millis() + kOtaUploadQuietWindowMs;
  if (otaError_.length() != 0) {
    return;
  }
  Hardware::writeStatus(true);
  if (otaSignedRelease_) otaImageHash_.append(data, length);
  inspectFirmwareBytes(data, length);
  if (otaError_.length() == 0 && otaReceivedSize_ > otaExpectedSize_) {
    otaError_ = "firmware exceeds declared image size";
  }
  if (otaError_.length() == 0) {
    // Keep one chunk unwritten. Update.end(false) commits a full-length
    // image even when called as cleanup, so a rejected image must remain
    // incomplete until every product/integrity check has passed.
    if (otaPendingChunkLength_ != 0 &&
        Update.write(otaPendingChunk_, otaPendingChunkLength_) !=
            otaPendingChunkLength_) {
      otaError_ = "firmware write failed: " + updateErrorString();
    } else if (length > sizeof(otaPendingChunk_)) {
      otaError_ = "firmware upload chunk is too large";
    } else {
      memcpy(otaPendingChunk_, data, length);
      otaPendingChunkLength_ = length;
    }
  }
}

void WebApplication::otaCompleteImage() {
  if (otaInstallActive_) {
    Hardware::writeStatus(true);
  }
  if (otaError_.length() == 0) {
    const uint32_t actualCrc32 = ~otaCrc32_;
    if (otaReceivedSize_ != otaExpectedSize_ ||
        actualCrc32 != otaExpectedCrc32_) {
      otaError_ = "firmware length or checksum mismatch";
    }
    if (otaError_.length() == 0 && otaSignedRelease_ &&
        !otaImageHash_.matches(otaRelease_.sha256)) {
      otaError_ = "signed firmware SHA-256 mismatch";
    }
    const bool allowedVendorRollback =
#if defined(SMART_TIMER_HARDWARE_V2)
        otaExpectedSize_ == kVendorRollbackSize &&
        actualCrc32 == kVendorRollbackCrc32;
#else
        (otaExpectedSize_ == kLegacy030Size &&
         actualCrc32 == kLegacy030Crc32) ||
        (otaExpectedSize_ == kLegacy130Size &&
         actualCrc32 == kLegacy130Crc32);
#endif
    if (otaError_.length() == 0 &&
        (!otaHeaderValid_ ||
         (!otaMarkerFound_ && !allowedVendorRollback &&
          !otaUploadUnverifiedAllowed_))) {
      otaError_ = "not an " + String(Platform::otaImageDescription());
    }
    if (otaError_.length() == 0) {
      if (Update.write(otaPendingChunk_, otaPendingChunkLength_) !=
          otaPendingChunkLength_) {
        otaError_ = "final firmware write failed: " + updateErrorString();
      } else {
        otaPendingChunkLength_ = 0;
        otaSucceeded_ = Update.end(false);
      }
      if (!otaSucceeded_ && otaError_.length() == 0) {
        otaError_ = "firmware validation failed: " + updateErrorString();
      }
    }
  }
  otaImageHash_.clear();
  if (!otaSucceeded_ && Update.isRunning()) {
    Update.end();
  }
  if (!otaSucceeded_) {
    otaInstallActive_ = false;
  }
}

void WebApplication::otaAbortImage() {
  otaImageHash_.clear();
  if (Update.isRunning()) {
    Update.end();
  }
  otaError_ = "upload aborted";
  otaQuietUntilMs_ = 0;
  otaInstallActive_ = false;
}

void WebApplication::handleOtaUpload() {
  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    httpOtaRejectedDuringBluetooth_ = bluetoothOtaActive_;
  }
  // Reject the whole HTTP request without resetting or aborting the BLE
  // upload's shared validator, held-back chunk, arm, or quiet window.
  if (httpOtaRejectedDuringBluetooth_) {
#if defined(ESP32)
    esp_task_wdt_reset();
#endif
    yield();
    return;
  }
  if (upload.status == UPLOAD_FILE_START) {
    const uint32_t nowMs = millis();
    otaResetUploadState();
    if (mutationInterlockActive()) {
      otaAllowed_ = false;
      otaError_ = "wait for relay maintenance to finish before updating";
      return;
    }
    if (network_.scanInProgress()) {
      otaAllowed_ = false;
      otaError_ = "wait for the Wi-Fi scan to finish before updating";
      return;
    }
    const bool armed = otaArmed(nowMs);
    const bool unverifiedAllowed = otaUnverifiedAllowed(nowMs);
    const bool trustedHost = requestFromAccessPoint() ||
                             HttpRequestFields::localHost(server_.header("Host").c_str());
    otaAllowed_ = trustedHost && sameOrigin() && timer_.isOpen() && armed && !bluetoothOtaActive_;
    otaUploadUnverifiedAllowed_ = otaAllowed_ && unverifiedAllowed;
    if (otaAllowed_) {
      // Only an accepted upload uses up the physical arm. A rejected attempt
      // (another site, a running timer) must not cancel the owner's arm.
      otaArmedUntilMs_ = 0;
      otaUnverifiedAllowed_ = false;
    }
    if (!otaAllowed_) {
      otaError_ =
          "press and release the Reset button within two seconds to arm OTA while open";
      return;
    }
    if (!parseOtaMetadata(otaExpectedSize_, otaExpectedCrc32_)) {
      otaError_ = "missing or invalid firmware size/checksum metadata";
      return;
    }
    if (requestValue("release_signature").length() && !ReleaseUpdate::hasVerificationMemory()) {
      otaError_ = "not enough memory to verify the release; close other control pages and retry";
      return;
    }
    if (!parseReleaseMetadata(otaExpectedSize_, otaExpectedCrc32_, otaRelease_, otaSignedRelease_)) {
      otaError_ = "invalid release signature";
      return;
    }
    const bool prepared =
        otaPrepareExpiresAtMs_ != 0 &&
        static_cast<int32_t>(otaPrepareExpiresAtMs_ - nowMs) > 0;
    if (prepared &&
        (otaExpectedSize_ != otaPreparedSize_ ||
         otaExpectedCrc32_ != otaPreparedCrc32_ ||
         otaSignedRelease_ != otaPreparedSignedRelease_ ||
         (otaSignedRelease_ && memcmp(otaRelease_.sha256, otaPreparedSha256_, 32)))) {
      otaError_ = "firmware does not match the prepared upload";
      return;
    }
    otaBeginImage(nowMs);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaAllowed_) {
      otaAcceptBytes(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaAllowed_) {
      otaCompleteImage();
    } else if (!otaSucceeded_ && Update.isRunning()) {
      Update.end();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaAbortImage();
  }
#if defined(ESP32)
  // The whole multipart upload runs inside one handleClient() call, so loop()
  // cannot feed the loop-task watchdog until it finishes. Each received chunk
  // is progress; a stalled upload still ends at the WebServer's POST timeout.
  esp_task_wdt_reset();
#endif
  yield();
}

void WebApplication::handleOtaComplete() {
  if (httpOtaRejectedDuringBluetooth_) {
    httpOtaRejectedDuringBluetooth_ = false;
    sendOtaBusy();
    return;
  }
  if (!otaAllowed_ || !otaSucceeded_) {
    timer_.recordEvent(TimerEventType::OtaFailed, millis());
    otaQuietUntilMs_ = 0;
    otaInstallActive_ = false;
    sendError(otaAllowed_ ? 400 : 403,
              otaError_.length() == 0 ? "update failed" : otaError_);
    return;
  }
  timer_.recordEvent(TimerEventType::OtaSucceeded, millis(),
                     static_cast<int32_t>(otaReceivedSize_));
  restartAtMs_ = millis() + 1500;
  sendJson(200, "{\"updated\":true,\"restarting\":true}");
}

void WebApplication::handleOtaPrepare() {
  if (!allowRequest(true)) {
    return;
  }
  const uint32_t nowMs = millis();
  if (!timer_.isOpen()) {
    sendError(409, "firmware updates require an open timer", "timer_active");
    return;
  }
  if (!otaArmed(nowMs)) {
    sendError(403,
              "press and release the Reset button within two seconds to arm OTA while open",
              "ota_not_armed");
    return;
  }
  if (network_.scanInProgress()) {
    sendError(409, "wait for the Wi-Fi scan to finish before updating",
              "wifi_scan_in_progress");
    return;
  }
  if (!parseOtaMetadata(otaPreparedSize_, otaPreparedCrc32_)) {
    sendError(400, "missing or invalid firmware size/checksum metadata",
              "invalid_ota_metadata");
    return;
  }
  ReleaseUpdate::Descriptor release;
  if (requestValue("release_signature").length() && !ReleaseUpdate::hasVerificationMemory()) {
    sendError(503, "not enough memory to verify the release; close other control pages and retry",
              "release_verification_busy");
    return;
  }
  if (!parseReleaseMetadata(otaPreparedSize_, otaPreparedCrc32_, release, otaPreparedSignedRelease_)) {
    sendError(400, "invalid release signature", "invalid_release_signature");
    return;
  }
  if (otaPreparedSignedRelease_) memcpy(otaPreparedSha256_, release.sha256, 32);
  otaPrepareExpiresAtMs_ = nowMs + kOtaPrepareWindowMs;
  network_.discardScanResults();
  otaQuietUntilMs_ = otaPrepareExpiresAtMs_;
  sendJson(200,
           "{\"ready\":true,\"quiet_window_ms\":" +
               String(kOtaPrepareWindowMs) +
               ",\"unverified_firmware_allowed\":" +
               String(otaUnverifiedAllowed(nowMs) ? "true" : "false") +
               "}");
}

void WebApplication::inspectFirmwareBytes(const uint8_t* data,
                                          size_t length) {
  for (size_t index = 0; index < length; ++index) {
    const uint8_t byte = data[index];
    ++otaReceivedSize_;
    otaCrc32_ ^= byte;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      otaCrc32_ =
          (otaCrc32_ >> 1) ^ (0xEDB88320UL & -(otaCrc32_ & 1UL));
    }
    if (otaHeaderLength_ < sizeof(otaHeader_)) {
      otaHeader_[otaHeaderLength_++] = byte;
      if (otaHeaderLength_ == OtaImageValidation::kRequiredHeaderLength) {
        otaHeaderValid_ = OtaImageValidation::matchesTarget(
            otaHeader_, otaHeaderLength_);
        if (!otaHeaderValid_) {
          otaError_ = OtaImageValidation::mismatchMessage();
          return;
        }
      }
    }

    if (!otaMarkerFound_) {
      const char* firmwareMarker = Platform::firmwareMarker();
      if (byte ==
          static_cast<uint8_t>(firmwareMarker[otaMarkerMatched_])) {
        ++otaMarkerMatched_;
        if (otaMarkerMatched_ == strlen(firmwareMarker)) {
          otaMarkerFound_ = true;
        }
      } else {
        otaMarkerMatched_ =
            byte == static_cast<uint8_t>(firmwareMarker[0]) ? 1 : 0;
      }
    }
  }
}

void WebApplication::scheduleWifiChange(const String& ssid,
                                        const String& password,
                                        bool standalone) {
  pendingSsid_ = ssid;
  pendingWifiPassword_ = password;
  pendingStandalone_ = standalone;
  pendingNetworkAtMs_ = millis() + 500;
}

#if defined(SMART_TIMER_HARDWARE_V2)
// Starts the V2 on-device download (docs/GITHUB_UPDATES.md). The same gates
// as an upload apply: an open timer and the physical Reset-button arm, which
// the download uses up.
void WebApplication::routeOtaDownload() {
  if (!allowRequest(true)) {
    return;
  }
  const uint32_t nowMs = millis();
  if (!timer_.isOpen()) {
    sendError(409, "firmware updates require an open timer", "timer_active");
    return;
  }
  if (!otaArmed(nowMs)) {
    sendError(403,
              "press and release the Reset button within two seconds to arm OTA while open",
              "ota_not_armed");
    return;
  }
  if (!ReleaseUpdate::configured()) {
    sendError(409, "online updates are not configured in this build", "updates_not_configured");
    return;
  }
  if (!network_.stationConnected()) {
    sendError(409, "connect the timer to a Wi-Fi network with Internet access",
              "no_station");
    return;
  }
  if (network_.scanInProgress()) {
    sendError(409, "wait for the Wi-Fi scan to finish before updating",
              "wifi_scan_in_progress");
    return;
  }
  if (bluetoothOtaActive_ || otaInstallActive_ || UpdateDownload::active()) {
    sendError(409, "another firmware update is in progress", "update_in_progress");
    return;
  }
  const String version = requestValue("release_version");
  if (version.length() && !ReleaseUpdate::validVersion(version.c_str())) {
    sendError(400, "invalid release version", "invalid_field");
    return;
  }
  if (!UpdateDownload::start(version.c_str(), SMART_TIMER_VERSION)) {
    sendError(503, "the download could not start; try again", "update_busy");
    return;
  }
  otaArmedUntilMs_ = 0;
  otaUnverifiedAllowed_ = false;
  downloadInstallStarted_ = false;
  network_.discardScanResults();
  sendJson(202, "{\"started\":true}");
}

// Installs the downloaded image from loop() through the upload's validators.
void WebApplication::serviceUpdateDownload(uint32_t nowMs) {
  UpdateDownload::Progress progress = UpdateDownload::progress();
  if (!downloadInstallStarted_) {
    if (progress.phase != UpdateDownload::Phase::Ready) return;
    ReleaseUpdate::Descriptor descriptor;
    if (!UpdateDownload::descriptor(descriptor)) return;
    otaResetUploadState();
    otaAllowed_ = true;
    otaExpectedSize_ = descriptor.size;
    otaExpectedCrc32_ = descriptor.crc32;
    otaSignedRelease_ = true;
    otaRelease_ = descriptor;
    if (!timer_.isOpen() || !otaBeginImage(nowMs, false)) {
      otaAllowed_ = false;
      UpdateDownload::finish(false, "install_failed");
      return;
    }
    downloadInstallStarted_ = true;
    UpdateDownload::beginImage();
    return;
  }
  uint8_t chunk[UpdateDownload::kChunkBytes];
  size_t length = 0;
  for (int pieces = 0; pieces < 8 && otaError_.length() == 0 &&
                       UpdateDownload::takeChunk(chunk, length); ++pieces) {
    otaAcceptBytes(chunk, length, false);
  }
  progress = UpdateDownload::progress();
  const bool rejected = otaError_.length() != 0;
  if (rejected || progress.phase == UpdateDownload::Phase::Failed) {
    UpdateDownload::finish(false, rejected ? "image_invalid" : nullptr);
    otaAbortImage();
    otaAllowed_ = false;
    downloadInstallStarted_ = false;
    timer_.recordEvent(TimerEventType::OtaFailed, nowMs);
    return;
  }
  if (progress.phase != UpdateDownload::Phase::Downloaded) return;
  otaCompleteImage();
  downloadInstallStarted_ = false;
  otaAllowed_ = false;
  if (!otaSucceeded_) {
    UpdateDownload::finish(false, "image_invalid");
    timer_.recordEvent(TimerEventType::OtaFailed, nowMs);
    return;
  }
  UpdateDownload::finish(true, nullptr);
  timer_.recordEvent(TimerEventType::OtaSucceeded, nowMs,
                     static_cast<int32_t>(otaReceivedSize_));
  restartAtMs_ = nowMs + 1500;
}
#endif
