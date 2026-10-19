#pragma once

#include <Arduino.h>
#include <DNSServer.h>

#include "Platform.h"

class DeviceConfig;

class LocalNetwork {
 public:
  static constexpr uint8_t kAccessPointClientLimit = 4;

  explicit LocalNetwork(DeviceConfig& config) : config_(config) {}

  void begin();
  void loop(uint32_t nowMs);
  void setStationCredentials(const String& ssid, const String& password);
  bool enterStandaloneMode();
  bool prepareStandaloneRestart();
  bool configurationSaveFailed() const;
  // Saved settings that are not about the network, for the web application.
  DeviceConfig& settings() { return config_; }
  // V2 Bluetooth-only mode is RAM-only. BLE disconnect or boot restores networking.
  bool wifiSuspended() const { return wifiSuspended_; }
  bool setWifiSuspended(bool suspended);
  const char* modeName() const { return wifiSuspended_ ? "bluetooth" : standaloneMode() ? "ap" : "wifi"; }

  String accessPointName() const { return accessPointName_; }
  String accessPointSecurity() const;
  bool accessPointPasswordSet() const;
  bool accessPointUsesDeviceName() const;
  String deviceName() const;
  String hostName() const { return hostName_; }
  bool setDeviceName(const String& name);
  bool setAccessPointConfig(const String& ssid, const String& password,
                            bool useDeviceName, bool keepExistingPassword,
                            bool clearStation = false);
  bool accessPointRunning() const { return accessPointRunning_; }
  String stationAddress() const;
  String stationMacAddress() const { return WiFi.macAddress(); }
  String stationName() const;
  String savedStationName() const;
  String targetStationName() const;
  String lastAttemptedStationName() const { return lastAttemptedSsid_; }
  bool stationConnected() const { return !wifiSuspended_ && WiFi.status() == WL_CONNECTED; }
  bool standaloneMode() const;
  String stationState(uint32_t nowMs) const;
  String lastWifiError() const { return lastWifiError_; }
  int32_t stationRssi() const;
  uint32_t retryInMs(uint32_t nowMs) const;
  bool stationRetryPaused() const;
  uint32_t stationConnectAttemptCount() const {
    return stationConnectAttemptCount_;
  }
  uint32_t stationDisconnectCount() const { return stationDisconnectCount_; }
  uint32_t stationUnexpectedDisconnectCount() const {
    return stationUnexpectedDisconnectCount_;
  }
  uint32_t stationDhcpTimeoutCount() const {
    return stationDhcpTimeoutCount_;
  }
  uint32_t stationSaQueryTimeoutCount() const { return stationSaQueryTimeoutCount_; }
  int32_t lastStationDisconnectRssi() const { return lastStationDisconnectRssi_; }
  uint8_t lastStationDisconnectReason() const {
    return lastStationDisconnectReason_;
  }
  const char* lastStationDisconnectReasonName() const;
  bool lastStationDisconnectExpected() const {
    return lastStationDisconnectExpected_;
  }
  uint32_t lastStationDisconnectAtMs() const {
    return lastStationDisconnectAtMs_;
  }
  uint32_t lastStationConnectedAtMs() const {
    return lastStationConnectedAtMs_;
  }
  uint32_t stationConnectedDurationMs(uint32_t nowMs) const;
  static const char* disconnectReasonName(uint8_t reason);
  uint8_t accessPointClients() const;
  uint8_t accessPointClientLimit() const { return kAccessPointClientLimit; }
  void acknowledgeStationHandoff();
  bool startScan();
  String scanJson() const;
  String scanStateName() const;
  bool scanInProgress() const;
  void discardScanResults();
  String legacySsidList();

 private:
  void startConfiguredNetwork();
  void connectStation();
  void refreshNames();
  void startAccessPoint();
  void stopAccessPoint();
  void startMdnsIfNeeded();
  void updateScan(uint32_t nowMs);
  void failCredentialTest(const String& error);
  void registerWifiDiagnostics();
  void expectStationDisconnect();
  static const char* wifiErrorForStatus(wl_status_t status);
  static String jsonEscape(const String& input);

  DeviceConfig& config_;
  DNSServer dnsServer_;
  String accessPointName_;
  String hostName_;
  uint32_t lastReconnectAttemptMs_ = 0;
  uint32_t disconnectedSinceMs_ = 0;
  uint32_t credentialAttemptStartedMs_ = 0;
  uint32_t stationHandoffDeadlineMs_ = 0;
  uint32_t reconnectDeferredUntilMs_ = 0;
  uint32_t expectedDisconnectUntilMs_ = 0;
  uint32_t stationConnectAttemptCount_ = 0;
  uint32_t stationDisconnectCount_ = 0;
  uint32_t stationUnexpectedDisconnectCount_ = 0;
  uint32_t stationDhcpTimeoutCount_ = 0;
  uint32_t stationSaQueryTimeoutCount_ = 0;
  int32_t lastStationDisconnectRssi_ = 127; // unavailable sentinel
  uint32_t lastStationDisconnectAtMs_ = 0;
  uint32_t lastStationConnectedAtMs_ = 0;
  uint32_t stationConnectedSinceMs_ = 0;
  uint8_t lastStationDisconnectReason_ = 0;
  bool lastStationDisconnectExpected_ = false;
  String candidateSsid_;
  String candidatePassword_;
  String lastAttemptedSsid_;
  String lastWifiError_;
  bool wifiSuspended_ = false;
  bool credentialTestPending_ = false;
  bool stationHandoffPending_ = false;
  bool stationHandoffAcknowledged_ = false;
  bool mdnsStarted_ = false;
  bool accessPointRunning_ = false;
  enum class ScanState : uint8_t { Idle, Scanning, Ready, Failed };
  ScanState scanState_ = ScanState::Idle;
  uint32_t scanStartedAtMs_ = 0;
  int16_t scanResultCount_ = 0;
#if defined(ESP8266)
  WiFiEventHandler stationDisconnectedHandler_;
  WiFiEventHandler stationGotIpHandler_;
  WiFiEventHandler stationDhcpTimeoutHandler_;
#endif
};
