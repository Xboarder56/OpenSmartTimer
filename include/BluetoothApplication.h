#pragma once
#include "BlePairingPolicy.h"

#include <Arduino.h>

#include "TimerController.h"

class DeviceConfig;
class LocalNetwork;
class WebApplication;

#if defined(SMART_TIMER_HARDWARE_V2)
#include <atomic>
#include <freertos/FreeRTOS.h>

#include "BleControlProtocol.h"
#include "BleAppRegistry.h"
#include "BleManagementProtocol.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;
#endif

// V2-only BLE transport. Timer mutations are accepted only through the
// authenticated, current-boot control gate and are queued back to loop().
class BluetoothApplication {
 public:
  BluetoothApplication(TimerController& timer, DeviceConfig& config,
                       LocalNetwork& network);

  bool begin(uint32_t bootId);
  void loop(uint32_t nowMs, bool mutationsAllowed = true);

  static BluetoothApplication* active();

  bool initialized() const;
  bool enabled() const;
  bool configuredEnabled() const;
  bool setRadioEnabled(bool enabled, uint32_t bootId);
  bool disable();
  bool enableControl();
  void disableControl();
  bool startWebPairing(uint32_t nowMs, uint32_t bootId);
#if defined(SMART_TIMER_HARDWARE_V2)
  bool startAppPairing(uint32_t nowMs, uint32_t bootId,
                       uint32_t replaceId, const String& label);
  bool revokeApp(uint32_t id);
  const BleAppRegistry& apps() const;
  bool renameApp(uint32_t id, const String& label);
  uint32_t activeAppId() const { return controlAuthenticated_ ? authenticatedAppId_ : 0; }
  uint32_t appLastSeenMs(uint8_t slot) const { return slot < 3 ? appLastSeenMs_[slot] : 0; }
  // Settings over Bluetooth run WebApplication's shared route handlers.
  void setManagementHost(WebApplication* host) { managementHost_ = host; }
#endif
  bool startPhysicalPairing(uint32_t nowMs, uint32_t bootId);
  bool confirmPhysicalPairing(uint32_t nowMs);
  void cancelPairing();
  bool revokePairing();
  bool clearBondsForFactoryReset(uint32_t bootId);
  bool paired() const;
  bool pairingActive(uint32_t nowMs) const;
  bool pairingAwaitingConfirmation(uint32_t nowMs) const;
  uint32_t pairingRemainingMs(uint32_t nowMs) const;
  const char* pairingStateName(uint32_t nowMs) const;
  const char* pairingFailureName() const;
  bool pairingLedOn(uint32_t nowMs) const;
  bool connectionEncrypted() const;
  bool connectionBonded() const;
  bool identifying(uint32_t nowMs) const;
  bool identifyLedOn(uint32_t nowMs) const;
  bool controlEnabled() const;
  bool controlAuthenticated() const;
  uint32_t controlAuthenticationFailureCount() const;
  uint32_t authenticationBackoffRemainingMs(uint32_t nowMs) const;
  uint32_t controlCommandCount() const;
  const char* lastControlResultName() const;
  bool synchronized() const;
  bool advertising() const;
  void pauseAdvertising(bool paused);
  bool connected() const;
  bool subscribed() const;
  uint8_t connectionCount() const;
  uint8_t connectionLimit() const;
  uint32_t acceptedCount() const;
  uint32_t closedCount() const;
  uint32_t notificationCount() const;
  int32_t lastError() const;
  const char* lastErrorName() const;
  int32_t lastDisconnectReason() const;
  const char* lastDisconnectReasonName() const;
  int8_t connectionRssi() const;
  uint16_t connectionMtu() const;
  bool connectionMtuSufficient() const;
  bool bondCleanupPending() const;
  int32_t bondCleanupLastError() const;
  const char* advertisedName() const;
  const char* protocolName() const;

 private:
#if defined(SMART_TIMER_HARDWARE_V2)
  static constexpr size_t kStatusFrameSize = 20;
  static constexpr size_t kSystemInfoFrameSize = 44;
  // V2 was never published, so its final capability contract is deliberately
  // strict: clients can validate every variable-sized binary frame rather
  // than inferring sizes from a firmware revision.
  static constexpr size_t kCapabilitiesFrameSize = 24;
  static constexpr size_t kEventFrameSize = 20;
  static constexpr uint32_t kFrameRefreshMs = 250;
  static constexpr uint32_t kNotificationRefreshMs = 15000;
  static constexpr uint32_t kSystemInfoRefreshMs = 5000;
  static constexpr uint32_t kConnectionRssiRefreshMs = 5000;
  static constexpr size_t kAuthenticationFrameSize = 20;
  static constexpr size_t kPairingCommitFrameSize = 20;
  // Authentication/commit use a two-byte header and four-byte client nonce.
  static constexpr size_t kAuthenticationMacSize = kAuthenticationFrameSize - 6;
  static_assert(kAuthenticationMacSize == 14 &&
                kPairingCommitFrameSize - 6 == kAuthenticationMacSize,
                "Authentication and pairing commit must use 112-bit tags");
  static constexpr size_t kControlFrameSize =
      BleControlProtocol::kCommandFrameSize;
  static constexpr size_t kControlResultFrameSize =
      BleControlProtocol::kResultFrameSize;
  static constexpr size_t kControlKeySize = 16;
  static constexpr size_t kSessionKeySize = 32;
  static constexpr size_t kControlChallengeSize = 12;
  static constexpr size_t kControlAuthenticatedPrefixSize = 24;
  static constexpr size_t kControlMacSize = 8;
  static constexpr uint8_t kMaximumAuthenticationFailures = 5;
  static constexpr uint32_t kAuthenticationRetryMs = 500;
  static constexpr uint32_t kAuthenticationBackoffMs = 30000;
  static constexpr uint32_t kUnauthenticatedConnectionTimeoutMs = 30000;
  // The timer accepts one connection and stops advertising while connected.
  // An authenticated app that stops reading or writing (a suspended browser
  // tab, a crashed app) is dropped so another approved phone can connect.
  // Timer notifications do not count; any GATT read or write from the peer
  // does. Dropping a connection never changes the timer or its lock output.
  static constexpr uint32_t kAuthenticatedIdleTimeoutMs = 5UL * 60UL * 1000UL;
  static constexpr uint32_t kPairingWindowMs = 120000;
  static constexpr uint32_t kPairingSecretReadWindowMs = 60000;
  static constexpr uint32_t kBondCleanupRetryMs = 5000;
  // After the radio is turned off, wait for the link to close before
  // handing the stack's memory back.
  static constexpr uint32_t kStackReleaseDelayMs = 2000;
  static constexpr uint32_t kIdentifyWindowMs = 5000;
  static constexpr uint16_t kRequiredControlMtu = 35;

  static constexpr uint8_t kOperationCapabilities = 0xff;
  static constexpr uint8_t kSecurityCapabilities = 0xff;

  using PairingState = BlePairingPolicy::State;

  enum class PairingFailure : uint8_t {
    None,
    TimedOut,
    EncryptionFailed,
    Disconnected,
    StorageFailed,
    CommitInvalid,
    TableFull,
  };

 public:
  // NimBLE requires plain function callbacks. They are public only so the
  // static GATT table can name them; application code must not call them.
  static int gattAccess(uint16_t connHandle, uint16_t attrHandle,
                        ble_gatt_access_ctxt* context, void* argument);
  static int gapEvent(ble_gap_event* event, void* argument);
  static void hostTask(void* argument);
  static void hostReset(int reason);
  static void hostSync();

 private:
  void onHostReset(int reason);
  void serviceNetworkRecovery();
  void onHostSync();
  int onGapEvent(ble_gap_event* event);
  int onGattAccess(uint16_t connHandle, uint16_t attrHandle,
                   ble_gatt_access_ctxt* context);
  bool startAdvertising();
  void releaseStack();
  void rotateControlChallenge();
  void updateControlChallengeFrame();
  bool authenticateControl(const uint8_t* frame, uint32_t nowMs);
  bool authenticateCommand(const uint8_t* frame);
  bool queuePairingCommit(uint16_t connHandle, const uint8_t* frame);
  bool queueControlCommand(uint16_t connHandle, const uint8_t* frame);
  void processControlCommand(uint32_t nowMs, bool mutationsAllowed);
  void processPairingCommit(uint32_t nowMs);
  bool sendControlResultNotification();
  bool sendEventNotification();
  void updateEventFrame(const TimerEvent& event, bool truncated);
  // Management extension (docs/BLUETOOTH_MANAGEMENT.md, BluetoothManagement.cpp).
  int onManagementWrite(uint16_t connHandle, ble_gatt_access_ctxt* context);
  int onManagementRead(uint16_t connHandle, ble_gatt_access_ctxt* context);
  void processManagementRequest(uint32_t nowMs);
  void storeManagementReply(uint8_t opcode, uint16_t requestId, uint32_t counter,
                            int status, const String& body, const uint8_t* responseKey = nullptr);
  bool publishManagementSegment(uint16_t offset, uint32_t counter, const uint8_t* responseKey = nullptr);
  void sendManagementReady();
  void clearManagementState();
  static bool hmacParts(const uint8_t* key, size_t keyLength,
                        const uint8_t* first, size_t firstLength,
                        const uint8_t* second, size_t secondLength,
                        const uint8_t* third, size_t thirdLength, uint8_t* output);
  void clearControlKey();
  void clearSessionKey();
  void clearConnectionAuthorization();
  void failAuthentication(uint32_t nowMs);
  bool loadPersistentControlKey(bool preserveSession = false);
  bool generatePairingKey(uint8_t* destination);
  bool initializePairing(uint32_t nowMs, bool physical, uint32_t bootId);
  bool validatePairingCommit(const uint8_t* frame) const;
  void finishPhysicalPairing(uint32_t nowMs);
  bool connectionPeerIsBonded(uint16_t connectionHandle) const;
  bool retainsApprovedPeer(uint8_t type, const uint8_t* address) const;
  bool rememberCurrentPeerForCleanup();
  bool attemptBondCleanup(uint32_t nowMs);
  void refreshConnectionSecurity();
  static bool controlHmac(const uint8_t* key, size_t keyLength,
                          const uint8_t* message, size_t messageLength,
                          uint8_t* output);
  static bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                                size_t length);
  void updateStatusFrame(uint32_t nowMs);
  void updateSystemInfoFrame(uint32_t nowMs);
  bool sendStatusNotification(uint32_t nowMs, bool force);
  void copyStatusFrame(uint8_t* destination);
  void copySystemInfoFrame(uint8_t* destination);
  static void writeUint32Le(uint8_t* destination, uint32_t value);

  static BluetoothApplication* active_;
  portMUX_TYPE frameMux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE controlMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t statusFrame_[kStatusFrameSize]{};
  uint8_t systemInfoFrame_[kSystemInfoFrameSize]{};
  uint8_t capabilitiesFrame_[kCapabilitiesFrameSize]{};
  uint8_t eventFrame_[kEventFrameSize]{};
  uint8_t controlChallengeFrame_[kAuthenticationFrameSize]{};
  uint8_t controlResultFrame_[kControlResultFrameSize]{};
  uint8_t pendingControlFrame_[kControlFrameSize]{};
  uint8_t controlKey_[kControlKeySize]{};
  BleAppRegistry::App controlApps_[BleAppRegistry::kCapacity]{};
  uint32_t appLastSeenMs_[BleAppRegistry::kCapacity]{};
  uint8_t appResults_[BleAppRegistry::kCapacity][kControlResultFrameSize]{};
  uint32_t appResultIds_[BleAppRegistry::kCapacity]{};
  uint32_t knownAppIds_[BleAppRegistry::kCapacity]{};
  uint32_t nextLegacyBindAtMs_ = 0;
  volatile uint32_t authenticatedAppId_ = 0;
  volatile uint8_t authenticatedAppSlot_ = 0;
  volatile uint32_t authorizationGeneration_ = 0;
  uint32_t pendingControlAppId_ = 0;
  uint8_t pendingControlAppSlot_ = 0;
  uint32_t pendingControlGeneration_ = 0;
  int pairingAppSlot_ = -1;
  uint32_t pairingReplaceId_ = 0;
  char pairingAppLabel_[BleAppRegistry::kLabelBytes + 1]{};
  bool pairingLabelExplicit_ = false;
  uint16_t pairingNicknameValueHandle_ = 0;
  uint8_t pendingPairingKey_[kControlKeySize]{};
  uint8_t sessionKey_[kSessionKeySize]{};
  WebApplication* managementHost_ = nullptr;
  uint16_t managementRequestValueHandle_ = 0;
  uint16_t managementResponseValueHandle_ = 0;
  volatile bool managementResponseSubscribed_ = false;
  BleManagement::Assembler managementAssembler_;  // BLE host task only.
  // GATT access scratch for fragments and reads (BLE host task only).
  uint8_t managementScratch_[BleManagement::kFragmentHeaderBytes + BleManagement::kMaxFrameBytes]{};
  // One authenticated request waiting for loop(); guarded by controlMux_.
  volatile bool pendingManagement_ = false;
  uint8_t pendingManagementFrame_[BleManagement::kMaxFrameBytes]{};
  size_t pendingManagementLength_ = 0;
  uint32_t pendingManagementGeneration_ = 0;
  uint32_t pendingManagementAppId_ = 0;
  uint8_t pendingManagementAppSlot_ = 0;
  // Exact-retry allowance for the most recent management frame.
  uint32_t lastManagementCounter_ = 0;
  uint8_t lastManagementDigest_[32]{};
  // Most recent reply (loop()), and its current segment as served by GATT reads.
  String managementBody_;
  uint16_t managementStatus_ = 0;
  uint8_t managementOpcode_ = 0;
  uint16_t managementRequestId_ = 0;
  uint8_t managementResponseValue_[BleManagement::kMaxFrameBytes]{};
  size_t managementResponseLength_ = 0;
  uint32_t managementReplyGeneration_ = 0;
  // Remembered mutation results per approved-app slot, for exact retries.
  struct ManagementResult {
    uint32_t appId = 0;
    uint16_t requestId = 0;
    uint8_t digest[8]{};
    uint16_t status = 0;
    String body;
  };
  static constexpr uint8_t kManagementResultCapacity = 4;
  ManagementResult managementResults_[BleAppRegistry::kCapacity][kManagementResultCapacity];
  uint8_t managementResultCursor_[BleAppRegistry::kCapacity]{};
  uint32_t disableRadioAtMs_ = 0;
  uint32_t releaseStackAtMs_ = 0;
  bool advertisingPaused_ = false;
  bool managementDispatchActive_ = false;  // main loop only
  volatile uint32_t managementDisconnectAtMs_ = 0;
  volatile bool managementTerminalReplyReady_ = false;
  uint16_t managementTerminalConnection_ = 0xffff;
  void deferManagementDisconnect();
  bool managementReceiptReadable(uint16_t connection, uint32_t nowMs) const;
  uint8_t lastAuthenticatedCommand_[kControlFrameSize]{};
  uint8_t pendingPairingCommitFrame_[kPairingCommitFrameSize]{};
  uint8_t preservedBondAddress_[6]{};
  uint8_t preservedBondAddressType_ = 0;
  char protocolInfo_[512]{};
  char advertisedName_[24]{};
  volatile bool initAttempted_ = false;
  volatile bool initialized_ = false;
  volatile bool enabled_ = false;
  volatile bool synchronized_ = false;
  volatile bool advertising_ = false;
  volatile bool connected_ = false;
  // Host callbacks only mark recovery; Wi-Fi operations run on loop().
  std::atomic<bool> networkRestorePending_{false};
  volatile bool subscribed_ = false;
  volatile bool controlResultSubscribed_ = false;
  volatile bool eventSubscribed_ = false;
  volatile bool controlEnabled_ = false;
  volatile bool controlAuthenticated_ = false;
  volatile bool connectionEncrypted_ = false;
  volatile bool connectionBonded_ = false;
  volatile bool physicalPairing_ = false;
  volatile bool pairingSecretAvailable_ = false;
  volatile bool pairingCommitConfirmed_ = false;
  volatile PairingState pairingState_ = PairingState::Idle;
  volatile PairingFailure pairingFailure_ = PairingFailure::None;
  volatile bool pendingControl_ = false;
  volatile bool pendingControlAuthorized_ = false;
  volatile bool pendingPairingCommit_ = false;
  volatile bool statusNotificationRequested_ = false;
  volatile bool eventReplayRequested_ = false;
  volatile bool bondCleanupPending_ = false;
  volatile bool preserveCurrentBond_ = false;
  volatile uint16_t connectionHandle_ = 0xffff;
  volatile uint16_t pendingControlConnectionHandle_ = 0xffff;
  volatile uint16_t pendingPairingCommitConnectionHandle_ = 0xffff;
  volatile uint32_t acceptedCount_ = 0;
  volatile uint32_t closedCount_ = 0;
  volatile uint32_t notificationCount_ = 0;
  volatile uint32_t controlAuthenticationFailureCount_ = 0;
  volatile uint32_t controlCommandCount_ = 0;
  volatile uint32_t lastCommandCounter_ = 0;
  volatile int32_t lastError_ = 0;
  volatile int32_t lastDisconnectReason_ = 0;
  volatile int8_t connectionRssi_ = 127;
  volatile uint16_t connectionMtu_ = 23;
  volatile int32_t bondCleanupLastError_ = 0;
  uint16_t statusValueHandle_ = 0;
  uint16_t systemInfoValueHandle_ = 0;
  uint16_t eventValueHandle_ = 0;
  uint16_t controlChallengeValueHandle_ = 0;
  uint16_t controlCommandValueHandle_ = 0;
  uint16_t controlResultValueHandle_ = 0;
  uint16_t pairingSecretValueHandle_ = 0;
  uint32_t bootId_ = 0;
  uint32_t lastFrameAtMs_ = 0;
  uint32_t lastSystemInfoAtMs_ = 0;
  uint32_t lastNotificationAtMs_ = 0;
  uint32_t lastStatusEventRevision_ = 0;
  uint32_t nextEventNotificationId_ = 0;
  uint32_t lastConnectionRssiAtMs_ = 0;
  uint32_t lastAuthenticationAttemptAtMs_ = 0;
  uint32_t authenticationBackoffUntilMs_ = 0;
  uint32_t connectionAcceptedAtMs_ = 0;
  // Written by the NimBLE host task on each GATT access; read by loop().
  volatile uint32_t lastPeerActivityMs_ = 0;
  uint32_t pairingUntilMs_ = 0;
  uint32_t pairingSecretUntilMs_ = 0;
  uint32_t pairingAcceptAfterCount_ = 0;
  uint32_t nextBondCleanupAtMs_ = 0;
  uint32_t identifyUntilMs_ = 0;
  uint16_t pairingConnectionHandle_ = 0xffff;
  BleControlResultCode lastControlResultCode_ =
      BleControlResultCode::Unauthorized;
#endif

  TimerController& timer_;
  DeviceConfig& config_;
  LocalNetwork& network_;
#if defined(SMART_TIMER_HARDWARE_V2)
  BleControlProtocol controlProtocol_;
#endif
};
