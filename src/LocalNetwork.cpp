#include "ResetTrace.h"
#include "LocalNetwork.h"

#include "DeviceConfig.h"
#include "Platform.h"

#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

namespace {
constexpr uint32_t kReconnectIntervalMs = 30000;
constexpr uint32_t kRecoveryClientRetryDeferralMs = 5000;
constexpr uint32_t kFallbackDelayMs = 20000;
constexpr uint32_t kCredentialTimeoutMs = 20000;
constexpr uint32_t kConnectingWindowMs = 15000;
constexpr uint32_t kStationHandoffMaximumMs = 2000;
constexpr uint32_t kStationHandoffAcknowledgedMs = 1500;
constexpr uint32_t kScanTimeoutMs = 15000;
#if defined(SMART_TIMER_HARDWARE_V2)
// Per-channel active scan window. The Arduino default (100-300 ms) kept the
// station off its own channel for 3.7 s, and 9.4 s with Bluetooth sharing the
// radio, during which no HTTP, event-stream or relay traffic was served.
constexpr uint32_t kScanChannelMinMs = 60;
constexpr uint32_t kScanChannelMaxMs = 120;
#endif
constexpr uint32_t kExpectedDisconnectWindowMs = 2000;
}

void LocalNetwork::begin() {
  WiFi.persistent(false);
#if defined(SMART_TIMER_HARDWARE_V2)
  // Favor bounded local-control latency over modem-sleep savings. In the
  // pinned C3 core this also records the policy before STA is started.
  // PMF/WPA3 and RF coexistence remain enabled; this is not a PMF downgrade.
  WiFi.setSleep(false);
#endif
  WiFi.setAutoReconnect(true);
  registerWifiDiagnostics();

  refreshNames();
  Platform::configureStationHostname(hostName_);

  startConfiguredNetwork();
}

void LocalNetwork::startConfiguredNetwork() {
  WiFi.setAutoReconnect(true);
  disconnectedSinceMs_ = 0;
  lastReconnectAttemptMs_ = 0;
  reconnectDeferredUntilMs_ = 0;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  if (config_.hasWifi()) {
    WiFi.mode(WIFI_STA);
    disconnectedSinceMs_ = millis();
    connectStation();
  } else {
    startAccessPoint();
  }
  startMdnsIfNeeded();
}

bool LocalNetwork::setWifiSuspended(bool suspended) {
#if !defined(SMART_TIMER_HARDWARE_V2)
  (void)suspended;
  return false;
#else
  if (suspended == wifiSuspended_) return true;
  // Never interrupt a credential test that may still commit its result.
  if (credentialTestPending_ || scanInProgress()) return false;
  if (!suspended) {
    wifiSuspended_ = false;
    startConfiguredNetwork();
    return true;
  }
  expectStationDisconnect();
  if (mdnsStarted_) {
    Platform::stopMdns();
    mdnsStarted_ = false;
  }
  dnsServer_.stop();
  WiFi.setAutoReconnect(false);
  // persistent(false) was set at boot; this stops station and AP without changing
  // the saved DeviceConfig credentials or ESP-IDF persistent Wi-Fi settings.
  const bool stopped = WiFi.mode(WIFI_OFF);
  accessPointRunning_ = false;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  reconnectDeferredUntilMs_ = 0;
  disconnectedSinceMs_ = 0;
  discardScanResults();
  if (!stopped) {
    startConfiguredNetwork();
    return false;
  }
  wifiSuspended_ = true;
  lastWifiError_ = "";
  return true;
#endif
}

void LocalNetwork::loop(uint32_t nowMs) {
  if (wifiSuspended_) return;
  if (expectedDisconnectUntilMs_ != 0 &&
      static_cast<int32_t>(nowMs - expectedDisconnectUntilMs_) >= 0) {
    expectedDisconnectUntilMs_ = 0;
  }
  if (accessPointRunning_) {
    ResetTrace::mark(ResetTrace::kNetworkDns);
    dnsServer_.processNextRequest();
  }
  ResetTrace::mark(ResetTrace::kNetworkScan);
  updateScan(nowMs);
  ResetTrace::mark(ResetTrace::kNetworkStation);
  const wl_status_t stationStatus = WiFi.status();

  if (credentialTestPending_) {
    if (stationStatus == WL_CONNECTED) {
      if (!config_.setWifi(candidateSsid_, candidatePassword_)) {
        failCredentialTest("could not save Wi-Fi settings; previous settings retained");
        return;
      }
      candidateSsid_ = "";
      candidatePassword_ = "";
      credentialTestPending_ = false;
      credentialAttemptStartedMs_ = 0;
      lastWifiError_ = "";
      stationHandoffPending_ = accessPointRunning_;
      stationHandoffAcknowledged_ = false;
      stationHandoffDeadlineMs_ = nowMs + kStationHandoffMaximumMs;
    } else {
      const char* immediateError = wifiErrorForStatus(stationStatus);
      if (immediateError != nullptr) {
        failCredentialTest(immediateError);
      } else if (nowMs - credentialAttemptStartedMs_ >=
                 kCredentialTimeoutMs) {
        failCredentialTest("connection timed out");
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    disconnectedSinceMs_ = 0;
    reconnectDeferredUntilMs_ = 0;
    if (accessPointRunning_ && config_.hasWifi() &&
        (!stationHandoffPending_ ||
         static_cast<int32_t>(nowMs - stationHandoffDeadlineMs_) >= 0)) {
      stopAccessPoint();
    }
    startMdnsIfNeeded();
    ResetTrace::mark(ResetTrace::kNetworkMdns);
    Platform::serviceMdns();
    return;
  }
  if (disconnectedSinceMs_ == 0) {
    disconnectedSinceMs_ = nowMs;
  }
  if (!config_.hasWifi()) {
    if (!accessPointRunning_) {
      startAccessPoint();
    }
    return;
  }
  if (!accessPointRunning_ &&
      nowMs - disconnectedSinceMs_ >= kFallbackDelayMs) {
    startAccessPoint();
  }
  if (config_.hasWifi() &&
      nowMs - lastReconnectAttemptMs_ >= kReconnectIntervalMs &&
      (reconnectDeferredUntilMs_ == 0 ||
       static_cast<int32_t>(nowMs - reconnectDeferredUntilMs_) >= 0)) {
    if (accessPointRunning_ && accessPointClients() != 0) {
      // A station attempt can retune the single ESP8266 radio and make its AP
      // appear unstable. Preserve a connected recovery/control session and
      // retry shortly after the last AP client leaves.
      reconnectDeferredUntilMs_ = nowMs + kRecoveryClientRetryDeferralMs;
    } else {
      connectStation();
    }
  }
}

void LocalNetwork::setStationCredentials(const String& ssid,
                                         const String& password) {
  discardScanResults();
  candidateSsid_ = ssid;
  candidatePassword_ = password;
  lastAttemptedSsid_ = ssid;
  credentialTestPending_ = true;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  reconnectDeferredUntilMs_ = 0;
  credentialAttemptStartedMs_ = millis();
  lastWifiError_ = "";
  expectStationDisconnect();
  WiFi.disconnect();
  if (!accessPointRunning_) {
    startAccessPoint();
  } else {
    WiFi.mode(WIFI_AP_STA);
  }
  disconnectedSinceMs_ = millis();
  lastReconnectAttemptMs_ = millis();
  ++stationConnectAttemptCount_;
  WiFi.begin(candidateSsid_.c_str(), candidatePassword_.c_str());
}

bool LocalNetwork::enterStandaloneMode() {
  if (!config_.clearWifi()) {
    lastWifiError_ = "could not save Wi-Fi settings; previous settings retained";
    return false;
  }
  candidateSsid_ = "";
  candidatePassword_ = "";
  lastAttemptedSsid_ = "";
  credentialTestPending_ = false;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  reconnectDeferredUntilMs_ = 0;
  credentialAttemptStartedMs_ = 0;
  lastWifiError_ = "";
  expectStationDisconnect();
  WiFi.disconnect();
  if (accessPointRunning_) {
    stopAccessPoint();
  }
  disconnectedSinceMs_ = 0;
  startAccessPoint();
  return true;
}

bool LocalNetwork::prepareStandaloneRestart() {
  if (!config_.clearWifi()) return false;
  candidateSsid_ = "";
  candidatePassword_ = "";
  lastAttemptedSsid_ = "";
  credentialTestPending_ = false;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  reconnectDeferredUntilMs_ = 0;
  credentialAttemptStartedMs_ = 0;
  lastWifiError_ = "";
  return true;
}

bool LocalNetwork::configurationSaveFailed() const { return config_.lastSaveFailed(); }

String LocalNetwork::accessPointSecurity() const {
  return config_.accessPointSecured() ? String("wpa2") : String("open");
}

bool LocalNetwork::accessPointPasswordSet() const {
  return config_.accessPointSecured();
}

bool LocalNetwork::accessPointUsesDeviceName() const {
  return config_.accessPointUsesDeviceName();
}

String LocalNetwork::stationAddress() const {
  return stationConnected() ? WiFi.localIP().toString() : String();
}

String LocalNetwork::deviceName() const { return config_.deviceName(); }

bool LocalNetwork::setDeviceName(const String& name) {
  if (!config_.setDeviceName(name)) {
    return false;
  }
  refreshNames();
  return true;
}

bool LocalNetwork::setAccessPointConfig(const String& ssid,
                                        const String& password,
                                        bool useDeviceName,
                                        bool keepExistingPassword, bool clearStation) {
  if (!config_.setAccessPoint(ssid, password, useDeviceName,
                              keepExistingPassword, clearStation)) {
    return false;
  }
  refreshNames();
  return true;
}

String LocalNetwork::stationName() const {
  return stationConnected() ? WiFi.SSID() : String();
}

String LocalNetwork::savedStationName() const {
  return config_.hasWifi() ? String(config_.wifiSsid()) : String();
}

String LocalNetwork::targetStationName() const {
  return credentialTestPending_ ? candidateSsid_ : String();
}

bool LocalNetwork::standaloneMode() const { return !wifiSuspended_ && !config_.hasWifi(); }

String LocalNetwork::stationState(uint32_t nowMs) const {
  if (wifiSuspended_) return "disabled";
  if (credentialTestPending_) {
    return "connecting";
  }
  if (stationConnected()) {
    return "connected";
  }
  if (lastWifiError_.length() != 0) {
    return "failed";
  }
  if (!config_.hasWifi()) {
    return "standalone";
  }
  if (lastReconnectAttemptMs_ != 0 &&
      nowMs - lastReconnectAttemptMs_ < kConnectingWindowMs) {
    return "connecting";
  }
  return "disconnected";
}

int32_t LocalNetwork::stationRssi() const {
  return stationConnected() ? WiFi.RSSI() : 0;
}

uint32_t LocalNetwork::retryInMs(uint32_t nowMs) const {
  if (wifiSuspended_ || stationConnected() || !config_.hasWifi() || credentialTestPending_) {
    return 0;
  }
  if (reconnectDeferredUntilMs_ != 0 &&
      static_cast<int32_t>(reconnectDeferredUntilMs_ - nowMs) > 0) {
    return reconnectDeferredUntilMs_ - nowMs;
  }
  const uint32_t elapsed = nowMs - lastReconnectAttemptMs_;
  return elapsed >= kReconnectIntervalMs ? 0
                                         : kReconnectIntervalMs - elapsed;
}

bool LocalNetwork::stationRetryPaused() const {
  return config_.hasWifi() && !credentialTestPending_ &&
         !stationConnected() && accessPointRunning_ &&
         accessPointClients() != 0;
}

uint8_t LocalNetwork::accessPointClients() const {
  return accessPointRunning_ ? WiFi.softAPgetStationNum() : 0;
}

void LocalNetwork::acknowledgeStationHandoff() {
  if (stationHandoffPending_ && !stationHandoffAcknowledged_) {
    stationHandoffAcknowledged_ = true;
    stationHandoffDeadlineMs_ = millis() + kStationHandoffAcknowledgedMs;
  }
}

bool LocalNetwork::startScan() {
  if (wifiSuspended_) return false;
  if (scanState_ == ScanState::Scanning) {
    return true;
  }
  if (credentialTestPending_) {
    return false;
  }
  WiFi.scanDelete();
  scanResultCount_ = 0;
#if defined(SMART_TIMER_HARDWARE_V2)
  WiFi.setScanActiveMinTime(kScanChannelMinMs);
  const int16_t result =
      WiFi.scanNetworks(true, false, false, kScanChannelMaxMs);
#else
  const int16_t result = WiFi.scanNetworks(true);
#endif
  if (result == WIFI_SCAN_RUNNING) {
    scanState_ = ScanState::Scanning;
    scanStartedAtMs_ = millis();
    return true;
  }
  if (result >= 0) {
    scanState_ = ScanState::Ready;
    scanResultCount_ = result;
    scanStartedAtMs_ = 0;
    return true;
  }
  scanState_ = ScanState::Failed;
  scanStartedAtMs_ = 0;
  return false;
}

String LocalNetwork::scanJson() const {
  String result = "{\"state\":\"" + scanStateName() + "\"";
  result += ",\"retry_in_ms\":";
  result += scanState_ == ScanState::Scanning ? "500" : "0";
  result += ",\"networks\":[";
  const int count =
      scanState_ == ScanState::Ready ? scanResultCount_ : 0;
  for (int index = 0; index < count; ++index) {
    if (index != 0) {
      result += ',';
    }
    result += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(index)) +
              "\",\"rssi\":" + String(WiFi.RSSI(index)) +
              ",\"secure\":" +
              String(Platform::scanNetworkIsOpen(index) ? "false"
                                                        : "true") +
              "}";
  }
  result += "]}";
  return result;
}

String LocalNetwork::legacySsidList() {
  if (scanState_ != ScanState::Ready) {
    startScan();
    return String();
  }
  String result;
  for (int index = 0; index < scanResultCount_; ++index) {
    if (index != 0) {
      result += '\v';
    }
    result += WiFi.SSID(index);
  }
  return result;
}

String LocalNetwork::scanStateName() const {
  switch (scanState_) {
    case ScanState::Scanning:
      return "scanning";
    case ScanState::Ready:
      return "ready";
    case ScanState::Failed:
      return "failed";
    case ScanState::Idle:
    default:
      return "idle";
  }
}

bool LocalNetwork::scanInProgress() const {
  return scanState_ == ScanState::Scanning;
}

void LocalNetwork::discardScanResults() {
  if (scanState_ == ScanState::Scanning) {
    return;
  }
  WiFi.scanDelete();
  scanState_ = ScanState::Idle;
  scanStartedAtMs_ = 0;
  scanResultCount_ = 0;
}

void LocalNetwork::connectStation() {
  if (wifiSuspended_) return;
  reconnectDeferredUntilMs_ = 0;
  lastReconnectAttemptMs_ = millis();
  ++stationConnectAttemptCount_;
  WiFi.begin(config_.wifiSsid(), config_.wifiPassword());
}

void LocalNetwork::refreshNames() {
  hostName_ = config_.hostName();
  if (config_.accessPointUsesDeviceName()) {
    accessPointName_ = hostName_ + "-" + Platform::shortDeviceId();
    accessPointName_.toUpperCase();
  } else {
    accessPointName_ = config_.accessPointSsid();
  }
}

void LocalNetwork::failCredentialTest(const String& error) {
  lastWifiError_ = error;
  candidateSsid_ = "";
  candidatePassword_ = "";
  credentialTestPending_ = false;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  credentialAttemptStartedMs_ = 0;
  expectStationDisconnect();
  WiFi.disconnect();
  disconnectedSinceMs_ = millis();
  if (config_.hasWifi()) {
    if (!accessPointRunning_ || accessPointClients() == 0) {
      connectStation();
    } else {
      reconnectDeferredUntilMs_ = millis() + kRecoveryClientRetryDeferralMs;
    }
  }
}

void LocalNetwork::registerWifiDiagnostics() {
#if defined(ESP8266)
  stationDisconnectedHandler_ = WiFi.onStationModeDisconnected(
      [this](const WiFiEventStationModeDisconnected& event) {
        const uint32_t nowMs = millis();
        const bool expected =
            expectedDisconnectUntilMs_ != 0 &&
            static_cast<int32_t>(expectedDisconnectUntilMs_ - nowMs) >= 0;
        expectedDisconnectUntilMs_ = 0;
        ++stationDisconnectCount_;
        if (!expected) {
          ++stationUnexpectedDisconnectCount_;
        }
        lastStationDisconnectReason_ = static_cast<uint8_t>(event.reason);
        lastStationDisconnectExpected_ = expected;
        lastStationDisconnectAtMs_ = nowMs == 0 ? 1 : nowMs;
        stationConnectedSinceMs_ = 0;
      });
  stationGotIpHandler_ = WiFi.onStationModeGotIP(
      [this](const WiFiEventStationModeGotIP&) {
        const uint32_t nowMs = millis();
        lastStationConnectedAtMs_ = nowMs == 0 ? 1 : nowMs;
        stationConnectedSinceMs_ = lastStationConnectedAtMs_;
      });
  stationDhcpTimeoutHandler_ = WiFi.onStationModeDHCPTimeout([this]() {
    ++stationDhcpTimeoutCount_;
  });
#else
  WiFi.onEvent(
      [this](arduino_event_id_t, arduino_event_info_t info) {
        const uint32_t nowMs = millis();
        const bool expected =
            expectedDisconnectUntilMs_ != 0 &&
            static_cast<int32_t>(expectedDisconnectUntilMs_ - nowMs) >= 0;
        expectedDisconnectUntilMs_ = 0;
        ++stationDisconnectCount_;
        if (!expected) {
          ++stationUnexpectedDisconnectCount_;
        }
        lastStationDisconnectReason_ =
            static_cast<uint8_t>(info.wifi_sta_disconnected.reason);
        if (!expected && lastStationDisconnectReason_ == 209) ++stationSaQueryTimeoutCount_;
        lastStationDisconnectRssi_ = info.wifi_sta_disconnected.rssi;
        lastStationDisconnectExpected_ = expected;
        lastStationDisconnectAtMs_ = nowMs == 0 ? 1 : nowMs;
        stationConnectedSinceMs_ = 0;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(
      [this](arduino_event_id_t, arduino_event_info_t) {
        const uint32_t nowMs = millis();
        lastStationConnectedAtMs_ = nowMs == 0 ? 1 : nowMs;
        stationConnectedSinceMs_ = lastStationConnectedAtMs_;
      },
      ARDUINO_EVENT_WIFI_STA_GOT_IP);
#endif
}

void LocalNetwork::expectStationDisconnect() {
  expectedDisconnectUntilMs_ = millis() + kExpectedDisconnectWindowMs;
}

const char* LocalNetwork::lastStationDisconnectReasonName() const {
  return disconnectReasonName(lastStationDisconnectReason_);
}

uint32_t LocalNetwork::stationConnectedDurationMs(uint32_t nowMs) const {
  return stationConnected() && stationConnectedSinceMs_ != 0
             ? nowMs - stationConnectedSinceMs_
             : 0;
}

const char* LocalNetwork::disconnectReasonName(uint8_t reason) {
  switch (reason) {
    case 0:
      return "none";
    case 1:
      return "unspecified";
    case 2:
      return "authentication_expired";
    case 3:
      return "authentication_left";
    case 4:
      return "association_expired";
    case 5:
      return "association_limit";
    case 6:
      return "not_authenticated";
    case 7:
      return "not_associated";
    case 8:
      return "association_left";
    case 9:
      return "association_not_authenticated";
    case 13:
      return "invalid_information_element";
    case 14:
      return "mic_failure";
    case 15:
      return "four_way_handshake_timeout";
    case 16:
      return "group_key_update_timeout";
    case 23:
      return "authentication_failed_8021x";
    case 200:
      return "beacon_timeout";
    case 201:
      return "network_not_found";
    case 202:
      return "authentication_failed";
    case 203:
      return "association_failed";
    case 204:
      return "handshake_timeout";
    case 209:
      return "sa_query_timeout";
    default:
      return "unknown";
  }
}

void LocalNetwork::startAccessPoint() {
  if (wifiSuspended_) return;
  if (accessPointRunning_) {
    return;
  }
  if (mdnsStarted_) {
    Platform::stopMdns();
    mdnsStarted_ = false;
  }
  if (config_.hasWifi()) {
    WiFi.setAutoReconnect(false);
  }
  WiFi.mode(config_.hasWifi() ? WIFI_AP_STA : WIFI_AP);
  const bool started = config_.accessPointSecured()
                           ? WiFi.softAP(accessPointName_.c_str(),
                                         config_.accessPointPassword(), 1,
                                         false,
                                         LocalNetwork::kAccessPointClientLimit)
                           : WiFi.softAP(accessPointName_.c_str(), nullptr, 1,
                                         false,
                                         LocalNetwork::kAccessPointClientLimit);
#if defined(SMART_TIMER_HARDWARE_V2)
  if (!started) {
    accessPointRunning_ = false;
    if (config_.hasWifi()) {
      WiFi.setAutoReconnect(true);
    }
    return;
  }
#else
  // Keep the field-tested V1 behavior; ESP8266 historically ignored this
  // return value and proceeded to start captive DNS.
  (void)started;
#endif
  dnsServer_.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer_.start(53, "*", WiFi.softAPIP());
  accessPointRunning_ = true;
  startMdnsIfNeeded();
}

void LocalNetwork::stopAccessPoint() {
  if (mdnsStarted_) {
    Platform::stopMdns();
    mdnsStarted_ = false;
  }
  dnsServer_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  accessPointRunning_ = false;
  stationHandoffPending_ = false;
  stationHandoffAcknowledged_ = false;
  stationHandoffDeadlineMs_ = 0;
  startMdnsIfNeeded();
}

void LocalNetwork::startMdnsIfNeeded() {
  if (mdnsStarted_) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED && !accessPointRunning_) {
    return;
  }
  if (MDNS.begin(hostName_.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("smarttimer", "tcp", 80);
    MDNS.addServiceTxt("smarttimer", "tcp", "api", "v1");
    MDNS.addServiceTxt("smarttimer", "tcp", "id",
                       Platform::deviceId());
    MDNS.addServiceTxt("smarttimer", "tcp", "version",
                       SMART_TIMER_VERSION);
    MDNS.addServiceTxt("smarttimer", "tcp", "path", "/api/v1/status");
    MDNS.addServiceTxt("smarttimer", "tcp", "events",
                       "/api/v1/events/stream");
    MDNS.addServiceTxt("smarttimer", "tcp", "model",
                       Platform::hardwareModel());
    mdnsStarted_ = true;
  }
}

void LocalNetwork::updateScan(uint32_t nowMs) {
  if (scanState_ != ScanState::Scanning) {
    return;
  }
  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (nowMs - scanStartedAtMs_ >= kScanTimeoutMs) {
      WiFi.scanDelete();
      scanState_ = ScanState::Failed;
      scanStartedAtMs_ = 0;
    }
    return;
  }
  scanStartedAtMs_ = 0;
  if (result >= 0) {
    scanResultCount_ = result;
    scanState_ = ScanState::Ready;
  } else {
    WiFi.scanDelete();
    scanResultCount_ = 0;
    scanState_ = ScanState::Failed;
  }
}

const char* LocalNetwork::wifiErrorForStatus(wl_status_t status) {
#if defined(ESP8266)
  // ESP8266 assigns value 6 to WL_WRONG_PASSWORD. ESP32 assigns the same
  // numeric value to WL_DISCONNECTED, which is a normal transient state
  // while a station connection is being established.
  if (status == WL_WRONG_PASSWORD) {
    return "wrong password";
  }
#endif
  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "network not found";
    case WL_CONNECT_FAILED:
      return "connection failed";
    default:
      return nullptr;
  }
}

String LocalNetwork::jsonEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char character = input[i];
    if (character == '"' || character == '\\') {
      output += '\\';
    }
    if (static_cast<uint8_t>(character) >= 0x20) {
      output += character;
    }
  }
  return output;
}
