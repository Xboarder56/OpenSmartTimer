#pragma once

#include <Arduino.h>

#include "Platform.h"
#include "HttpRequestFields.h"
#include "SseWriteCursor.h"
#include "TimerCommandAuthority.h"
#include "ReleaseUpdate.h"

class LocalNetwork;

class WebApplication {
 public:
  WebApplication(TimerController& timer, LocalNetwork& network);

  void begin();
  void loop(uint32_t nowMs);
#if defined(SMART_TIMER_HARDWARE_V2)
  bool ready() const { return ready_; }
#endif
  uint32_t bootId() const { return bootId_; }
  void armOta(uint32_t nowMs);
  bool otaArmed(uint32_t nowMs) const;
  bool otaUnverifiedAllowed(uint32_t nowMs) const;
  uint32_t otaArmedRemainingMs(uint32_t nowMs) const;
  bool otaInstallActive() const { return otaInstallActive_; }
  bool otaInstallSucceeded() const { return otaSucceeded_; }
  bool identifying(uint32_t nowMs) const;
  bool identifyLedOn(uint32_t nowMs) const;
  // Starts the Identify LED window (REST, and relay commands via main.cpp).
  void beginIdentify(uint32_t nowMs) { identifyUntilMs_ = nowMs + kIdentifyWindowMs; }
  // Shared by the BLE transport so REST and BLE obey the same quiet/restart/
  // network-transition interlock.
  bool mutationTransitionActive(uint32_t nowMs, bool includeInterlock = true) const;
  // Lets another subsystem (the V2 relay while it writes flash) hold the timer
  // open: REST/BLE mutations and OTA are refused while `active` returns true.
  // Requests that belong to that subsystem pass interlockOwner to allowRequest.
  void setMutationInterlock(bool (*active)(void* context), void* context) {
    interlockActive_ = active;
    interlockContext_ = context;
  }
  bool mutationInterlockActive() const {
    return interlockActive_ && interlockActive_(interlockContext_);
  }

  // Bluetooth device management (docs/BLUETOOTH_MANAGEMENT.md). Each route runs
  // the same handler as its HTTP endpoint; the status and JSON body are
  // captured instead of being written to a socket.
  struct ManagementReply {
    int status = 500;
    String body;
  };
  enum class ManagementRoute : uint8_t {
    Status, WifiStatus, AccessPoint, DeviceName, Bluetooth, Diagnostics, Events,
    Capabilities, WifiConfigure, WifiForget, AccessPointSet, DeviceNameSet,
    Restart, AppLabel, AppRevoke, PairingStart, PairingCancel, PairingRevoke,
    WifiSuspend, WifiResume, WifiScanStart, WifiScanResults, OtaBegin, OtaFinish, OtaAbort,
    OtaDownload,
  };
  ManagementReply manage(ManagementRoute route, const char* body,
                         size_t bodyLength, const char* secret,
                         size_t secretLength);
  // Firmware chunk for a Bluetooth update started with OtaBegin.
  ManagementReply manageOtaChunk(uint32_t offset, const uint8_t* data,
                                 size_t length);
  static constexpr size_t kBluetoothOtaChunkBytes = 472;

 private:
  void registerStaticRoutes();
  void registerModernRoutes();
  void registerLegacyRoutes();
  void registerOtaRoute(const String& path);
  void registerEventStreamHook();

  // Route handlers shared by HTTP registration and manage().
  void routeStatus();
  void routeWifiScanStart();
  void routeWifiScanResults();
  void routeWifiStatus();
  void routeAccessPoint();
  void routeAccessPointSet();
  void routeWifiConfigure();
  void routeWifiForget();
  void routeRestart();
  void routeDiagnostics();
  void routeEvents();
  void routeCapabilities();
  void routeDeviceName();
  void routeDeviceNameSet();
#if defined(SMART_TIMER_HARDWARE_V2)
  void routeBluetooth();
  void routeBluetoothWifiMode(bool suspended);
#endif
  void routeBluetoothOtaBegin();
  void routeBluetoothOtaFinish();
  void routeBluetoothOtaAbort();
  void routeOtaDownload();
  void serviceUpdateDownload(uint32_t nowMs);
  void failBluetoothOta();
  // Firmware image pipeline shared by the multipart upload and Bluetooth.
  void otaResetUploadState();
  // quiet: hold other clients off for the upload (not for a V2 download,
  // whose own loop must keep answering status requests).
  bool otaBeginImage(uint32_t nowMs, bool quiet = true);
  void otaAcceptBytes(const uint8_t* data, size_t length, bool quiet = true);
  void otaCompleteImage();
  void otaAbortImage();

  void sendIndex();
  void sendAsset(const uint8_t* data, size_t length, const char* mimeType,
                 bool cache);
  void sendJson(int status, const String& body);
  void sendError(int status, const String& message,
                 const String& code = "request_failed");
  void sendOtaBusy();
  void sendStatus();
  String statusJson(uint32_t nowMs, const String* action = nullptr,
                    bool changed = false, bool duplicate = false,
                    const String* operationId = nullptr);
  void sendWifiStatus();
  void sendAccessPointConfig();
  void sendDiagnostics();
#if defined(SMART_TIMER_HARDWARE_V2)
  void sendBluetoothStatus();
  void handleBluetoothPairingStart();
  void handleBluetoothPairingCancel();
  void handleBluetoothPairingRevoke();
  void handleBluetoothAppUpdate(bool remove);
#endif
  void sendEvents();
  void sendCapabilities();
  void sendDeviceName();
  void sendMutationStatus(const String& action, bool changed,
                          bool duplicate, const String& operationId);
  void handleLockControlsMutation(bool requireSealedValue);
#if defined(SMART_TIMER_RELAY)
  // Experimental relay owner API (src/WebRelay.cpp, V2 only).
  void registerRelayRoutes();
  void sendRelayStatus(int status);
  void sendRelayResult(int result);
  uint32_t relayLiveLinks() const;
#endif
  // Deduplicates, applies the shared TimerCommandAuthority rules and responds.
  // Parameters must already be parsed into command.valueA/valueB.
  void handleTimerMutation(const char* action, TimerCommand command);
  void sendTimerCommandError(const char* action, TimerCommandResult result);

  bool readOperationId(String& operationId);
  String sessionId() const;

  bool allowRequest(bool mutation, bool interlockOwner = false);
  void sendSessionControls();
  void routeSessionControlsSet();
  bool validateRequestFields();
  void sendRequestFieldError();
  bool otaQuietActive(uint32_t nowMs) const;
  bool parseOtaMetadata(uint32_t& size, uint32_t& checksum) const;
  bool parseReleaseMetadata(uint32_t size, uint32_t checksum,
                            ReleaseUpdate::Descriptor& descriptor, bool& signedRelease) const;
  bool sameOrigin() const;
  bool requestFromAccessPoint();
  bool streamFromAccessPoint(WiFiClient& client);
  String requestValue(const String& name) const;
  static String jsonEscape(const String& input);
  static const char* stateName(uint8_t state);

  void handleOtaUpload();
  void handleOtaComplete();
  void handleOtaPrepare();
  void inspectFirmwareBytes(const uint8_t* data, size_t length);
  void scheduleWifiChange(const String& ssid, const String& password,
                          bool standalone);
  void observeState(uint32_t nowMs, const String* knownWifiState = nullptr);
  void serviceEventStreams(uint32_t nowMs);
  void closeEventStreams();
  struct EventStreamClient;
  enum class EventStreamCloseReason : uint8_t {
    None,
    PeerClosed,
    HeaderTooLarge,
    HeaderTimeout,
    ResponseWriteFailed,
    SendTimeout,
    OtaQuiet,
    HostRejected
  };
  void releaseEventStream(EventStreamClient& slot,
                          EventStreamCloseReason reason);
  static const char* eventStreamCloseReasonName(
      EventStreamCloseReason reason);
  uint8_t eventStreamClientCount();
#if defined(ESP8266)
  ESP8266WebServer::ClientFuture handleEventStreamHook(
      const String& method, const String& url, WiFiClient* client);
#else
  void handleEventStreamRequest();
#endif
  bool acceptEventStream(WiFiClient& client, bool headersComplete);
  void rejectEventStream(WiFiClient& client, int status,
                         const char* statusText, const String& body,
                         uint16_t retryAfterSeconds = 0);
  bool queueEventStreamStatus(EventStreamClient& slot, uint32_t nowMs,
                              const String& body);

  TimerController& timer_;
  LocalNetwork& network_;
  SmartTimerWebServer server_{80};
  HttpRequestFields requestFields_;
  bool requestIsJson_ = false;
  // Non-null while manage() runs a route: replies are captured, and the
  // decrypted secret is supplied as the request's password field.
  ManagementReply* capture_ = nullptr;
  const char* captureSecret_ = nullptr;
  size_t captureSecretLength_ = 0;
  bool bluetoothOtaActive_ = false;
  bool downloadInstallStarted_ = false;  // V2 on-device download is writing
  bool httpOtaRejectedDuringBluetooth_ = false;
#if defined(SMART_TIMER_HARDWARE_V2)
  bool ready_ = false;
#endif

#if defined(SMART_TIMER_HARDWARE_V2)
  static constexpr uint8_t kEventStreamClientLimit = 3;
#else
  // The V1's TCP stack allows five connections in total. Two viewers leave
  // room for page loads and commands; three left new connections refused
  // until one closed, so requests occasionally waited seconds for a retry.
  static constexpr uint8_t kEventStreamClientLimit = 2;
#endif
  static constexpr uint32_t kEventStreamRefreshMs = 15000;
  static constexpr uint32_t kOtaArmWindowMs = 5UL * 60UL * 1000UL;
  static constexpr uint32_t kIdentifyWindowMs = 5000;
  static constexpr uint32_t kOtaOtherClientRetryMs = 30000;
  static constexpr uint32_t kOtaUploadQuietWindowMs = 2UL * 60UL * 1000UL;
  struct EventStreamClient {
    WiFiClient client;
    uint32_t acceptedAtMs = 0;
    uint32_t lastSentAtMs = 0;
    uint32_t lastSentRevision = 0;
    String pendingWire;
    SseWriteCursor writeCursor;
    uint32_t pendingRevision = 0;
    uint16_t headerBytes = 0;
    uint16_t headerLineLength = 0;
    char headerLine[72] = {0};  // current line's start, to find Host (V1)
    uint8_t hostLines = 0;      // a request with two Host lines is refused
    bool hostTrusted = false;
    bool headersComplete = false;
    bool responseStarted = false;
  };
  EventStreamClient eventStreamClients_[kEventStreamClientLimit]{};
  uint32_t eventStreamAcceptedCount_ = 0;
  uint32_t eventStreamClosedCount_ = 0;
  EventStreamCloseReason lastEventStreamCloseReason_ =
      EventStreamCloseReason::None;

  uint32_t bootId_ = 0;
  bool (*interlockActive_)(void*) = nullptr;
  void* interlockContext_ = nullptr;
  uint32_t minimumObservedFreeHeapBytes_ = 0;
  // What held memory at the lowest observed point; locates V1 heap pressure.
  struct HeapLowWater {
    uint32_t freeBytes = 0, atMs = 0, ssePendingBytes = 0, tcpQueuedBytes = 0;
    uint8_t sseClients = 0, tcpActive = 0, tcpTimeWait = 0;
    char lastUri[20] = {0};
  } heapLowWater_;
  void noteHeapLowWater(uint32_t freeHeap);
  uint32_t stateRevision_ = 1;
  uint32_t observedTimerRevision_ = 0;
  String observedRevisionWifiState_;
  uint8_t observedAccessPointClients_ = 0;
  uint16_t observedStateFlags_ = 0;
  bool stateObservationInitialized_ = false;

  bool otaAllowed_ = false;
  bool otaSucceeded_ = false;
  bool otaInstallActive_ = false;
  String otaError_;
  uint32_t otaArmedUntilMs_ = 0;
  bool otaUnverifiedAllowed_ = false;
  bool otaUploadUnverifiedAllowed_ = false;
  uint8_t otaHeader_[16]{};
  uint8_t otaHeaderLength_ = 0;
  uint8_t otaMarkerMatched_ = 0;
  bool otaHeaderValid_ = false;
  bool otaMarkerFound_ = false;
  uint32_t otaExpectedSize_ = 0;
  uint32_t otaReceivedSize_ = 0;
  uint32_t otaExpectedCrc32_ = 0;
  uint32_t otaCrc32_ = 0xFFFFFFFFUL;
  uint32_t otaPreparedSize_ = 0;
  uint32_t otaPreparedCrc32_ = 0;
  uint32_t otaPrepareExpiresAtMs_ = 0;
  bool otaSignedRelease_ = false;
  bool otaPreparedSignedRelease_ = false;
  ReleaseUpdate::Descriptor otaRelease_{};
  uint8_t otaPreparedSha256_[32]{};
  ReleaseUpdate::ImageHash otaImageHash_;
  uint32_t otaQuietUntilMs_ = 0;
  uint8_t otaPendingChunk_[HTTP_UPLOAD_BUFLEN]{};
  size_t otaPendingChunkLength_ = 0;
  uint32_t pendingNetworkAtMs_ = 0;
  String pendingSsid_;
  String pendingWifiPassword_;
  bool pendingStandalone_ = false;
  uint32_t restartAtMs_ = 0;
  uint32_t identifyUntilMs_ = 0;
  String lastObservedWifiState_;
  uint32_t lastObservedWifiUnexpectedDisconnectCount_ = 0;
  uint32_t lastObservedWifiDhcpTimeoutCount_ = 0;

  TimerCommandAuthority commandAuthority_;
  uint32_t statusJsonFailureCount_ = 0;
  TimerOperationLedger operations_;
};
