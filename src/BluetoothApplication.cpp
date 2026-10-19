#include "BluetoothApplication.h"
#include "BleAccessPolicy.h"

#include "DeviceConfig.h"
#include "Hardware.h"
#include "LocalNetwork.h"
#include "Platform.h"
#include "generated_build_identity.h"

#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

#if defined(SMART_TIMER_HARDWARE_V2)

#include <cstring>

#include <mbedtls/sha256.h>

extern "C" {
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_l2cap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

extern "C" void ble_store_config_init(void);

namespace {
// Canonical UUIDs are documented in docs/BLUETOOTH.md. NimBLE's initializer
// stores 128-bit UUID bytes in little-endian order.
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x61, 0x59, 0x62, 0xd3, 0xb3, 0xbf, 0x73, 0x9b,
    0xdf, 0x44, 0x3d, 0x2f, 0xf4, 0x26, 0xba, 0x70);
const ble_uuid128_t kProtocolInfoUuid = BLE_UUID128_INIT(
    0xed, 0xa8, 0x19, 0x1c, 0x36, 0xfe, 0xde, 0xbb,
    0x9f, 0x49, 0x82, 0x72, 0x5b, 0x6f, 0x82, 0x8d);
const ble_uuid128_t kLiveStatusUuid = BLE_UUID128_INIT(
    0x63, 0xab, 0x79, 0x6d, 0xd8, 0x42, 0xc6, 0xac,
    0x4e, 0x43, 0xfe, 0xd4, 0x5e, 0x3d, 0xa9, 0xd1);
const ble_uuid128_t kCapabilitiesUuid = BLE_UUID128_INIT(
    0x5a, 0x3c, 0x97, 0x1e, 0x8a, 0x44, 0x73, 0xae,
    0x91, 0x42, 0x19, 0xe6, 0x38, 0x74, 0xb2, 0x6f);
const ble_uuid128_t kSystemInfoUuid = BLE_UUID128_INIT(
    0x44, 0x11, 0x3b, 0x59, 0x49, 0x77, 0x85, 0x92,
    0x21, 0x49, 0x4f, 0x46, 0x99, 0xf2, 0x0d, 0xf0);
const ble_uuid128_t kEventUuid = BLE_UUID128_INIT(
    0x0c, 0x67, 0x7a, 0x42, 0x19, 0x52, 0xa5, 0x92,
    0x34, 0x4b, 0xa0, 0x67, 0xbb, 0x21, 0x74, 0x91);
const ble_uuid128_t kControlChallengeUuid = BLE_UUID128_INIT(
    0x64, 0x1f, 0xd9, 0x18, 0x5d, 0x11, 0x07, 0xba,
    0xeb, 0x42, 0xbb, 0xb1, 0x10, 0x07, 0xb3, 0xea);
const ble_uuid128_t kControlCommandUuid = BLE_UUID128_INIT(
    0xc5, 0xbd, 0x9c, 0xde, 0x4f, 0xc8, 0x15, 0xa3,
    0x51, 0x4d, 0x24, 0x72, 0x0f, 0x98, 0xa0, 0x1f);
const ble_uuid128_t kControlResultUuid = BLE_UUID128_INIT(
    0xdb, 0xb6, 0x9e, 0x5d, 0x32, 0x14, 0xf5, 0x97,
    0x60, 0x46, 0xad, 0xf2, 0xa1, 0xb9, 0xf3, 0x2d);
const ble_uuid128_t kPairingSecretUuid = BLE_UUID128_INIT(
    0x91, 0xf0, 0xb4, 0x2c, 0x63, 0xc5, 0xd8, 0xa1,
    0x2b, 0x4f, 0x70, 0x35, 0xe1, 0x8c, 0x6d, 0x54);

const ble_uuid128_t kPairingNicknameUuid = BLE_UUID128_INIT(
    0x12, 0x76, 0x45, 0xaa, 0x6b, 0x4e, 0x85, 0xb7,
    0x91, 0x41, 0x79, 0x03, 0x4d, 0x72, 0xaf, 0x63);

// Management extension (docs/BLUETOOTH_MANAGEMENT.md).
const ble_uuid128_t kManagementRequestUuid = BLE_UUID128_INIT(
    0xba, 0x95, 0x11, 0xc5, 0xe6, 0x8b, 0xed, 0x9e,
    0xd0, 0x4c, 0xa0, 0x44, 0x45, 0x62, 0x54, 0x4b);
const ble_uuid128_t kManagementResponseUuid = BLE_UUID128_INIT(
    0xf9, 0x60, 0x3f, 0x01, 0x24, 0x57, 0x97, 0xa5,
    0xa6, 0x46, 0xf4, 0x25, 0x61, 0x2e, 0xb5, 0x93);

ble_gatt_chr_def kCharacteristics[] = {
    {
        &kProtocolInfoUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_READ,
        0,
        nullptr,
        nullptr,
    },
    {
        &kCapabilitiesUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_READ,
        0,
        nullptr,
        nullptr,
    },
    {
        &kSystemInfoUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC),
        0,
        nullptr,
        nullptr,
    },
    {
        &kLiveStatusUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC |
                                        BLE_GATT_CHR_F_NOTIFY),
        0,
        nullptr,
        nullptr,
    },
    {
        &kEventUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC |
                                        BLE_GATT_CHR_F_NOTIFY),
        0,
        nullptr,
        nullptr,
    },
    {
        &kControlChallengeUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC),
        0,
        nullptr,
        nullptr,
    },
    {
        &kControlCommandUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_WRITE |
                                        BLE_GATT_CHR_F_WRITE_ENC),
        0,
        nullptr,
        nullptr,
    },
    {
        &kControlResultUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC |
                                        BLE_GATT_CHR_F_NOTIFY),
        0,
        nullptr,
        nullptr,
    },
    {
        &kPairingSecretUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ |
                                        BLE_GATT_CHR_F_READ_ENC),
        0,
        nullptr,
        nullptr,
    },
    {
        &kPairingNicknameUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                        BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC),
        0,
        nullptr,
        nullptr,
    },
    // Appended after Pairing nickname so earlier handles do not move.
    {
        &kManagementRequestUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                                        BLE_GATT_CHR_F_WRITE_ENC),
        0,
        nullptr,
        nullptr,
    },
    {
        &kManagementResponseUuid.u,
        BluetoothApplication::gattAccess,
        nullptr,
        nullptr,
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                        BLE_GATT_CHR_F_NOTIFY),
        0,
        nullptr,
        nullptr,
    },
    {},
};

ble_gatt_svc_def kServices[] = {
    {
        BLE_GATT_SVC_TYPE_PRIMARY,
        &kServiceUuid.u,
        nullptr,
        kCharacteristics,
    },
    {},
};

uint8_t stateCode(TimerState state) {
  switch (state) {
    case TimerState::Running:
      return 1;
    case TimerState::Paused:
      return 2;
    case TimerState::Open:
    default:
      return 0;
  }
}

uint8_t releaseReasonCode(ReleaseReason reason) {
  switch (reason) {
    case ReleaseReason::Expired:
      return 1;
    case ReleaseReason::User:
      return 2;
    case ReleaseReason::Emergency:
      return 3;
    case ReleaseReason::AdjustedToZero:
      return 4;
    case ReleaseReason::ShareLinkExpired:
      return 5;
    case ReleaseReason::Boot:
    default:
      return 0;
  }
}

uint8_t eventCode(TimerEventType type) {
  // Stable ost-ble/1 assignments. Keep this explicit even though the internal
  // enum currently uses the same values, so future internal refactors cannot
  // silently change the public wire format.
  switch (type) {
    case TimerEventType::Boot: return 0;
    case TimerEventType::Started: return 1;
    case TimerEventType::RemainingSet: return 2;
    case TimerEventType::Paused: return 3;
    case TimerEventType::Resumed: return 4;
    case TimerEventType::Adjusted: return 5;
    case TimerEventType::ControlsLocked: return 6;
    case TimerEventType::Expired: return 7;
    case TimerEventType::OpenedByUser: return 8;
    case TimerEventType::EmergencyRelease: return 9;
    case TimerEventType::EmergencyInputActivated: return 10;
    case TimerEventType::EmergencyInputCleared: return 11;
    case TimerEventType::AdjustedToZero: return 12;
    case TimerEventType::WifiConnecting: return 13;
    case TimerEventType::WifiConnected: return 14;
    case TimerEventType::WifiFailed: return 15;
    case TimerEventType::WifiStandalone: return 16;
    case TimerEventType::WifiDisconnected: return 17;
    case TimerEventType::WifiDhcpTimeout: return 18;
    case TimerEventType::OtaStarted: return 19;
    case TimerEventType::OtaSucceeded: return 20;
    case TimerEventType::OtaFailed: return 21;
    case TimerEventType::RestartRequested: return 22;
    case TimerEventType::DeviceRenamed: return 23;
    case TimerEventType::IdentifyStarted: return 24;
    case TimerEventType::AccessPointConfigured: return 25;
    case TimerEventType::BluetoothPairingStarted: return 26;
    case TimerEventType::BluetoothPairingCompleted: return 27;
    case TimerEventType::BluetoothPairingRevoked: return 28;
    case TimerEventType::ShareLinkExpired: return 29;
    default: return 255;
  }
}
}  // namespace

BluetoothApplication* BluetoothApplication::active_ = nullptr;

BluetoothApplication::BluetoothApplication(TimerController& timer,
                                           DeviceConfig& config,
                                           LocalNetwork& network)
    : timer_(timer), config_(config), network_(network), controlProtocol_(timer) {
  active_ = this;
}

bool BluetoothApplication::begin(uint32_t bootId) {
  if (initialized_) {
    enabled_ = true;
    releaseStackAtMs_ = 0;
    loadPersistentControlKey();
    return !synchronized_ || advertising_ || connected_ || startAdvertising();
  }
  if (initAttempted_) {
    return false;
  }
  initAttempted_ = true;
  enabled_ = true;
  bootId_ = bootId;

  const String shortId = Platform::shortDeviceId();
  snprintf(advertisedName_, sizeof(advertisedName_), "OST-%s",
           shortId.c_str());
  const String deviceName = config_.deviceName();
  const String hostName = config_.hostName();
  const String accessPointName = network_.accessPointName();
  String escapedDeviceName;
  String escapedAccessPointName;
  escapedDeviceName.reserve(deviceName.length() * 2);
  escapedAccessPointName.reserve(accessPointName.length() * 2);
  for (size_t index = 0; index < deviceName.length(); ++index) {
    const char character = deviceName[index];
    if (character == '"' || character == '\\') escapedDeviceName += '\\';
    escapedDeviceName += character;
  }
  for (size_t index = 0; index < accessPointName.length(); ++index) {
    const char character = accessPointName[index];
    if (character == '"' || character == '\\') escapedAccessPointName += '\\';
    escapedAccessPointName += character;
  }
  snprintf(protocolInfo_, sizeof(protocolInfo_),
           "{\"protocol\":\"ost-ble/1\",\"firmware\":\"%s\"," 
           "\"model\":\"MagBoundV2\",\"device_id\":\"%s\"," 
           "\"device_name\":\"%s\",\"hostname\":\"%s\"," 
           "\"ap_ssid\":\"%s\",\"network_mode\":\"%s\"," 
           "\"boot_id\":\"%lx\",\"build_id\":\"%s\"," 
           "\"control\":\"bonded_hmac\"," 
           "\"approved_app_limit\":3,\"pairing_nickname\":true,\"required_mtu\":%u,"
           "\"management\":1,\"management_max_frame\":%u}",
           SMART_TIMER_VERSION, Platform::deviceId().c_str(),
           escapedDeviceName.c_str(), hostName.c_str(),
           escapedAccessPointName.c_str(),
           config_.hasWifi() ? "wifi" : "ap",
           static_cast<unsigned long>(bootId_), SMART_TIMER_BUILD_ID,
           kRequiredControlMtu,
           static_cast<unsigned>(BleManagement::kMaxFrameBytes));

  controlProtocol_.begin(bootId_);
  memset(capabilitiesFrame_, 0, sizeof(capabilitiesFrame_));
  capabilitiesFrame_[0] = BleControlProtocol::kProtocolVersion;
  capabilitiesFrame_[1] = kOperationCapabilities;
  capabilitiesFrame_[2] = kSecurityCapabilities;
  capabilitiesFrame_[3] = connectionLimit();
  writeUint32Le(capabilitiesFrame_ + 4, Hardware::kMaximumDurationMs);
  writeUint32Le(capabilitiesFrame_ + 8, kNotificationRefreshMs);
  writeUint32Le(capabilitiesFrame_ + 12, kPairingWindowMs);
  capabilitiesFrame_[16] = kControlFrameSize;
  capabilitiesFrame_[17] = kControlResultFrameSize;
  capabilitiesFrame_[18] = kStatusFrameSize;
  capabilitiesFrame_[19] = TimerController::eventCapacity();
  capabilitiesFrame_[20] = kEventFrameSize;
  capabilitiesFrame_[21] = kSystemInfoFrameSize;
  capabilitiesFrame_[22] = kControlMacSize;
  capabilitiesFrame_[23] = kAuthenticationMacSize;
  memset(controlResultFrame_, 0, sizeof(controlResultFrame_));
  controlResultFrame_[0] = BleControlProtocol::kProtocolVersion;
  controlResultFrame_[4] =
      static_cast<uint8_t>(BleControlResultCode::Unauthorized);
  writeUint32Le(controlResultFrame_ + 8, bootId_);
  rotateControlChallenge();
  updateControlChallengeFrame();
  const uint32_t nowMs = millis();
  updateStatusFrame(nowMs);
  updateSystemInfoFrame(nowMs);
  lastSystemInfoAtMs_ = nowMs == 0 ? 1 : nowMs;
  if (timer_.eventCount() != 0) {
    updateEventFrame(timer_.eventAt(timer_.eventCount() - 1), false);
  }
  loadPersistentControlKey();

  const esp_err_t initResult = nimble_port_init();
  if (initResult != ESP_OK) {
    enabled_ = false;
    lastError_ = initResult;
    return false;
  }
  // Larger ATT packets let a 512-byte management frame travel in three
  // fragments. The framework default preference is 128.
  ble_att_set_preferred_mtu(247);
  ble_hs_cfg.reset_cb = hostReset;
  ble_hs_cfg.sync_cb = hostSync;
  // Link encryption and bonding protect transport privacy. Because this
  // no-display device can only use Just Works, the independently provisioned
  // application key remains the authority for status and mutations.
  // Never let NimBLE's round-robin overflow handler evict an approved bond.
  ble_hs_cfg.store_status_cb = [](ble_store_status_event* event, void*) {
    return event->event_code == BLE_STORE_EVENT_OVERFLOW ? BLE_HS_ENOMEM : 0;
  };
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  // Unknown normal connections may read protocol metadata but cannot create a
  // persistent bond. The physical pairing ceremony temporarily enables
  // bonding for its single post-window connection.
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                               BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;

  ble_store_config_init();

  ble_svc_gap_init();
  ble_svc_gatt_init();

  kCharacteristics[2].val_handle = &systemInfoValueHandle_;
  kCharacteristics[3].val_handle = &statusValueHandle_;
  kCharacteristics[4].val_handle = &eventValueHandle_;
  kCharacteristics[5].val_handle = &controlChallengeValueHandle_;
  kCharacteristics[6].val_handle = &controlCommandValueHandle_;
  kCharacteristics[7].val_handle = &controlResultValueHandle_;
  kCharacteristics[8].val_handle = &pairingSecretValueHandle_;
  kCharacteristics[9].val_handle = &pairingNicknameValueHandle_;
  kCharacteristics[10].val_handle = &managementRequestValueHandle_;
  kCharacteristics[11].val_handle = &managementResponseValueHandle_;
  int result = ble_gatts_count_cfg(kServices);
  if (result == 0) {
    result = ble_gatts_add_svcs(kServices);
  }
  if (result == 0) {
    result = ble_svc_gap_device_name_set(advertisedName_);
  }
  if (result != 0) {
    enabled_ = false;
    lastError_ = result;
    return false;
  }

  initialized_ = true;
  nimble_port_freertos_init(hostTask);
  return true;
}

void BluetoothApplication::serviceNetworkRecovery() {
  // A latched disconnect also covers a new connection arriving before loop().
  // Recovery must run even while the timer is locked or Bluetooth is disabled.
  const bool disconnected = networkRestorePending_.exchange(false);
  if (network_.wifiSuspended() && (disconnected || !connected_) &&
      !network_.setWifiSuspended(false)) {
    networkRestorePending_.store(true);
  }
}

void BluetoothApplication::loop(uint32_t nowMs, bool mutationsAllowed) {
  serviceNetworkRecovery();
  if (initialized_ && !enabled_ && releaseStackAtMs_ != 0 && !connected_ &&
      static_cast<int32_t>(nowMs - releaseStackAtMs_) >= 0) {
    releaseStack();
  }
  if (!initialized_ || !enabled_) {
    return;
  }
  if (managementDisconnectAtMs_ != 0 &&
      static_cast<int32_t>(nowMs - managementDisconnectAtMs_) >= 0) {
    managementTerminalReplyReady_ = false;
    if (connected_) ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
    // Keep all writes denied until the disconnect callback clears the drain.
    return;
  }
  if (disableRadioAtMs_ != 0 && static_cast<int32_t>(nowMs - disableRadioAtMs_) >= 0) {
    // "Turn Bluetooth off" requested over Bluetooth: its reply went out first.
    disableRadioAtMs_ = 0;
    disable();  // The setting was saved before acknowledging the request.
    return;
  }
  // One-time binding of a migrated legacy key. NVS writes stay on loop(), not
  // the NimBLE callback task. Until this succeeds cleanup conserves old bonds.
  const uint32_t activeId = activeAppId();
  const int migratedSlot = config_.bluetoothApps().find(activeId);
  if (mutationsAllowed && migratedSlot >= 0 && !config_.bluetoothApps().apps[migratedSlot].peerKnown &&
      (nextLegacyBindAtMs_ == 0 || static_cast<int32_t>(nowMs - nextLegacyBindAtMs_) >= 0)) {
    nextLegacyBindAtMs_ = nowMs + kBondCleanupRetryMs;
    ble_gap_conn_desc descriptor{};
    if (ble_gap_conn_find(connectionHandle_, &descriptor) == 0 &&
        config_.bindBluetoothAppPeer(activeId, descriptor.peer_id_addr.type,
                                     descriptor.peer_id_addr.val)) {
      loadPersistentControlKey(true);
      preserveCurrentBond_ = true;
      bondCleanupPending_ = true;
    }
  }
  if (pairingState_ != PairingState::Idle &&
      pairingState_ != PairingState::Complete &&
      pairingState_ != PairingState::Failed && pairingUntilMs_ != 0 &&
      static_cast<int32_t>(nowMs - pairingUntilMs_) >= 0) {
    pairingFailure_ = PairingFailure::TimedOut;
    cancelPairing();
    pairingState_ = PairingState::Failed;
  }
  if ((pairingState_ == PairingState::SecretReady ||
       pairingState_ == PairingState::WaitingForCommit) &&
      pairingSecretUntilMs_ != 0 &&
      static_cast<int32_t>(nowMs - pairingSecretUntilMs_) >= 0) {
    pairingFailure_ = PairingFailure::TimedOut;
    cancelPairing();
    pairingState_ = PairingState::Failed;
  }
  if (pairingState_ == PairingState::Failed && physicalPairing_) {
    const PairingFailure failure = pairingFailure_;
    cancelPairing();
    pairingFailure_ = failure;
    pairingState_ = PairingState::Failed;
  }
  if (!mutationsAllowed && pairingActive(nowMs)) {
    cancelPairing();
  }
  if (!timer_.isOpen() && pairingActive(nowMs)) {
    cancelPairing();
  }
  processPairingCommit(nowMs);
  processControlCommand(nowMs, mutationsAllowed);
  // Management requests are answered even during OTA or a pending restart:
  // the shared route handlers apply those gates themselves, and a Bluetooth
  // firmware update needs its chunks.
  processManagementRequest(nowMs);
  if (!mutationsAllowed) {
    // OTA, restart, and network transitions quiet periodic BLE work as well as
    // rejecting mutations. A queued command still receives the deterministic
    // SystemTransition result above, but status/event traffic cannot compete
    // with the recovery-critical operation.
    return;
  }

  if (bondCleanupPending_ && managementDisconnectAtMs_ == 0 &&
      (nextBondCleanupAtMs_ == 0 ||
       static_cast<int32_t>(nowMs - nextBondCleanupAtMs_) >= 0)) {
    attemptBondCleanup(nowMs);
  }
  // Advertising and a Wi-Fi scan share one radio. With both running, the
  // station stayed off its channel for the whole scan and HTTP stalled for
  // nine seconds. A pairing window keeps advertising; a scan is short.
  const bool holdAdvertising = advertisingPaused_ && !pairingActive(nowMs);
  if (holdAdvertising && advertising_ && !connected_) {
    const int result = ble_gap_adv_stop();
    if (result == 0 || result == BLE_HS_EALREADY) advertising_ = false;
  }
  if (!connected_ && synchronized_ && !advertising_ && !holdAdvertising &&
      authenticationBackoffRemainingMs(nowMs) == 0) {
    startAdvertising();
  }
  if (connected_ && !pairingActive(nowMs) && !controlAuthenticated_ &&
      connectionAcceptedAtMs_ != 0 &&
      nowMs - connectionAcceptedAtMs_ >=
          kUnauthenticatedConnectionTimeoutMs) {
    ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
    connectionAcceptedAtMs_ = nowMs == 0 ? 1 : nowMs;
  }
  if (BlePairingPolicy::authenticatedConnectionIdle(
          connected_, controlAuthenticated_, pairingActive(nowMs),
          lastPeerActivityMs_, nowMs, kAuthenticatedIdleTimeoutMs)) {
    ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
    // Retry the terminate after another full window if it did not complete.
    lastPeerActivityMs_ = nowMs == 0 ? 1 : nowMs;
  }
  if (lastFrameAtMs_ != 0 && nowMs - lastFrameAtMs_ < kFrameRefreshMs) {
    return;
  }
  lastFrameAtMs_ = nowMs == 0 ? 1 : nowMs;
  updateStatusFrame(nowMs);
  if (lastSystemInfoAtMs_ == 0 ||
      nowMs - lastSystemInfoAtMs_ >= kSystemInfoRefreshMs) {
    updateSystemInfoFrame(nowMs);
    lastSystemInfoAtMs_ = nowMs == 0 ? 1 : nowMs;
  }

  const uint32_t eventRevision = timer_.eventRevision();
  const bool eventChanged = eventRevision != lastStatusEventRevision_;
  const bool periodic = lastNotificationAtMs_ == 0 ||
                        nowMs - lastNotificationAtMs_ >=
                            kNotificationRefreshMs;
  if ((eventChanged || periodic || statusNotificationRequested_) &&
      sendStatusNotification(nowMs, statusNotificationRequested_)) {
    lastStatusEventRevision_ = eventRevision;
    statusNotificationRequested_ = false;
  }

  if (eventSubscribed_ && controlAuthenticated_ &&
      timer_.eventCount() != 0) {
    const uint32_t oldestId = timer_.eventAt(0).id;
    const uint32_t latestId = timer_.eventAt(timer_.eventCount() - 1).id;
    if (eventReplayRequested_ || nextEventNotificationId_ == 0) {
      nextEventNotificationId_ = oldestId;
      eventReplayRequested_ = false;
    }
    if (nextEventNotificationId_ <= latestId) {
      const bool truncated = nextEventNotificationId_ < oldestId;
      TimerEvent event = timer_.eventAt(0);
      for (uint8_t index = 0; index < timer_.eventCount(); ++index) {
        const TimerEvent candidate = timer_.eventAt(index);
        if (candidate.id >= nextEventNotificationId_) {
          event = candidate;
          break;
        }
      }
      updateEventFrame(event, truncated);
      if (sendEventNotification()) {
        nextEventNotificationId_ = event.id + 1;
      }
    }
  } else if (timer_.eventCount() != 0) {
    updateEventFrame(timer_.eventAt(timer_.eventCount() - 1), false);
  }

  if (connected_ &&
      (lastConnectionRssiAtMs_ == 0 ||
       nowMs - lastConnectionRssiAtMs_ >= kConnectionRssiRefreshMs)) {
    int8_t rssi = 127;
    const int result = ble_gap_conn_rssi(connectionHandle_, &rssi);
    if (result == 0) {
      connectionRssi_ = rssi;
      lastConnectionRssiAtMs_ = nowMs == 0 ? 1 : nowMs;
    }
  }
}

BluetoothApplication* BluetoothApplication::active() { return active_; }

bool BluetoothApplication::initialized() const { return initialized_; }
bool BluetoothApplication::enabled() const { return enabled_; }
bool BluetoothApplication::configuredEnabled() const {
  return config_.bluetoothEnabled();
}
bool BluetoothApplication::setRadioEnabled(bool enabled, uint32_t bootId) {
  if (!config_.setBluetoothEnabled(enabled)) {
    return false;
  }
  return enabled ? begin(bootId) : disable();
}
bool BluetoothApplication::disable() {
  enabled_ = false;
  disableControl();
  cancelPairing();
  if (!initialized_) {
    return true;
  }

  bool success = true;
  if (advertising_) {
    const int result = ble_gap_adv_stop();
    if (result != 0 && result != BLE_HS_EALREADY) {
      lastError_ = result;
      success = false;
    }
  }
  if (connected_) {
    const int result = ble_gap_terminate(connectionHandle_,
                                         BLE_ERR_REM_USER_CONN_TERM);
    if (result != 0 && result != BLE_HS_EALREADY) {
      lastError_ = result;
      success = false;
    }
  }

  advertising_ = false;
  releaseStackAtMs_ = (millis() + kStackReleaseDelayMs) | 1;
  return success;
}

// A stopped radio otherwise keeps the stack's heap until restart, which left
// the relay without room to reconnect. Same order as the Arduino BLE library.
// Runs on loop(), never on the host task; begin() starts the stack again.
void BluetoothApplication::releaseStack() {
  releaseStackAtMs_ = 0;
  if (nimble_port_stop() != 0) {
    releaseStackAtMs_ = (millis() + kStackReleaseDelayMs) | 1;
    return;
  }
  // Let the host task leave nimble_port_run() and delete itself.
  vTaskDelay(pdMS_TO_TICKS(50));
  nimble_port_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  synchronized_ = false;
  advertising_ = false;
  initialized_ = false;
  initAttempted_ = false;
}
bool BluetoothApplication::enableControl() {
  return startWebPairing(millis(), bootId_);
}
void BluetoothApplication::disableControl() {
  portENTER_CRITICAL(&controlMux_);
  controlEnabled_ = false;
  controlAuthenticated_ = false;
  authenticatedAppId_ = 0;
  authorizationGeneration_ = authorizationGeneration_ + 1;
  pendingControl_ = false;
  pendingControlAuthorized_ = false;
  pendingControlConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  lastAuthenticationAttemptAtMs_ = 0;
  lastCommandCounter_ = 0;
  memset(lastAuthenticatedCommand_, 0, sizeof(lastAuthenticatedCommand_));
  clearControlKey();
  clearSessionKey();
  portEXIT_CRITICAL(&controlMux_);
  rotateControlChallenge();
  updateControlChallengeFrame();
}
bool BluetoothApplication::startWebPairing(uint32_t nowMs,
                                           uint32_t bootId) {
  // Starting from the web and starting with the physical hold deliberately use
  // one ceremony: connect, encrypt, briefly press Reset, then let the app read
  // its credential. The web page never displays or transports the app key.
  return startPhysicalPairing(nowMs, bootId);
}

bool BluetoothApplication::startPhysicalPairing(uint32_t nowMs,
                                                uint32_t bootId) {
  return startAppPairing(nowMs, bootId, 0, "");
}

bool BluetoothApplication::startAppPairing(uint32_t nowMs, uint32_t bootId,
                                           uint32_t replaceId, const String& label) {
  if (pairingActive(nowMs)) return false;
  const int slot = replaceId ? config_.bluetoothApps().find(replaceId)
                             : config_.bluetoothApps().empty();
  if (slot < 0) {
    pairingFailure_ = PairingFailure::TableFull;
    pairingState_ = PairingState::Failed;
    return false;
  }
  const String effectiveLabel = label.length() ? label : String("Device ") + String(slot + 1);
  if (!BleAppRegistry::validLabel(effectiveLabel.c_str())) return false;
  if (!timer_.isOpen() || !setRadioEnabled(true, bootId) ||
      !initializePairing(nowMs, true, bootId)) {
    return false;
  }
  pairingAppSlot_ = slot;
  pairingReplaceId_ = replaceId;
  pairingLabelExplicit_ = label.length() != 0;
  effectiveLabel.toCharArray(pairingAppLabel_, sizeof(pairingAppLabel_));
  timer_.recordEvent(TimerEventType::BluetoothPairingStarted, nowMs,
                     static_cast<int32_t>(kPairingWindowMs));
  return true;
}

bool BluetoothApplication::confirmPhysicalPairing(uint32_t nowMs) {
  if (!timer_.isOpen() || !physicalPairing_ ||
      pairingState_ != PairingState::WaitingForConfirmation ||
      !connected_ || !connectionEncrypted_ || !connectionBonded_ ||
      pairingConnectionHandle_ != connectionHandle_) {
    return false;
  }
  pairingState_ = PairingState::SecretReady;
  pairingSecretAvailable_ = true;
  pairingSecretUntilMs_ = nowMs + kPairingSecretReadWindowMs;
  // Give the app the full post-approval window even if the user pressed near
  // the end of the initial two-minute connection/approval window.
  pairingUntilMs_ = pairingSecretUntilMs_;
  return true;
}

void BluetoothApplication::cancelPairing() {
  if (initialized_) {
    ble_hs_cfg.sm_bonding = 0;
  }
  physicalPairing_ = false;
  pairingAppSlot_ = -1;
  pairingReplaceId_ = 0;
  pairingLabelExplicit_ = false;
  memset(pairingAppLabel_, 0, sizeof(pairingAppLabel_));
  // Canceled/failed candidates must not consume the spare bond indefinitely.
  preserveCurrentBond_ = true;
  bondCleanupPending_ = initialized_;
  nextBondCleanupAtMs_ = 0;
  pairingSecretAvailable_ = false;
  pairingCommitConfirmed_ = false;
  pairingState_ = PairingState::Idle;
  pairingUntilMs_ = 0;
  pairingSecretUntilMs_ = 0;
  pairingConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  pendingPairingCommit_ = false;
  pendingPairingCommitConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  memset(pendingPairingCommitFrame_, 0,
         sizeof(pendingPairingCommitFrame_));
  volatile uint8_t* key = pendingPairingKey_;
  for (size_t index = 0; index < sizeof(pendingPairingKey_); ++index) {
    key[index] = 0;
  }
}

bool BluetoothApplication::revokePairing() {
  const bool configCleared = config_.clearBluetoothPairing();
  if (!configCleared) return false;
  if (managementDispatchActive_ && connected_) deferManagementDisconnect();
  disableControl();
  cancelPairing();
  if (configCleared) {
    if (connected_ && managementDisconnectAtMs_ == 0)
      ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
    preserveCurrentBond_ = false;
    bondCleanupPending_ = initialized_;
    nextBondCleanupAtMs_ = 0;
    if (initialized_ && managementDisconnectAtMs_ == 0) {
      attemptBondCleanup(millis());
    }
    timer_.recordEvent(TimerEventType::BluetoothPairingRevoked, millis());
  }
  return configCleared;
}

bool BluetoothApplication::revokeApp(uint32_t id) {
  if (pairingActive(millis()) || !config_.removeBluetoothApp(id)) return false;
  const bool wasActive = activeAppId() == id;
  if (wasActive && managementDispatchActive_ && connected_) deferManagementDisconnect();
  loadPersistentControlKey(true);
  if (wasActive && connected_ && managementDisconnectAtMs_ == 0)
    ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
  preserveCurrentBond_ = true;
  bondCleanupPending_ = initialized_;
  nextBondCleanupAtMs_ = 0;
  if (initialized_ && managementDisconnectAtMs_ == 0) attemptBondCleanup(millis());
  timer_.recordEvent(TimerEventType::BluetoothPairingRevoked, millis());
  return true;
}

const BleAppRegistry& BluetoothApplication::apps() const { return config_.bluetoothApps(); }
bool BluetoothApplication::renameApp(uint32_t id, const String& label) {
  if (pairingActive(millis()) || !config_.renameBluetoothApp(id, label)) return false;
  loadPersistentControlKey(true);
  return true;
}

bool BluetoothApplication::clearBondsForFactoryReset(uint32_t bootId) {
  if (!initialized_ && !begin(bootId)) {
    return false;
  }
  preserveCurrentBond_ = false;
  bondCleanupPending_ = true;
  nextBondCleanupAtMs_ = 0;
  return attemptBondCleanup(millis());
}

bool BluetoothApplication::paired() const { return config_.bluetoothPaired(); }

bool BluetoothApplication::pairingActive(uint32_t nowMs) const {
  return pairingUntilMs_ != 0 &&
         static_cast<int32_t>(pairingUntilMs_ - nowMs) > 0 &&
         pairingState_ != PairingState::Idle &&
         pairingState_ != PairingState::Complete &&
         pairingState_ != PairingState::Failed;
}

bool BluetoothApplication::pairingAwaitingConfirmation(uint32_t nowMs) const {
  return pairingActive(nowMs) &&
         pairingState_ == PairingState::WaitingForConfirmation;
}

uint32_t BluetoothApplication::pairingRemainingMs(uint32_t nowMs) const {
  return pairingActive(nowMs) ? pairingUntilMs_ - nowMs : 0;
}

const char* BluetoothApplication::pairingStateName(uint32_t nowMs) const {
  if (!pairingActive(nowMs)) {
    return pairingState_ == PairingState::Failed ? "failed" :
           pairingState_ == PairingState::Complete ? "complete" : "idle";
  }
  switch (pairingState_) {
    case PairingState::WaitingForConnection:
      return "waiting_for_connection";
    case PairingState::WaitingForEncryption:
      return "securing_connection";
    case PairingState::WaitingForConfirmation:
      return "waiting_for_button";
    case PairingState::SecretReady:
      return "secret_ready";
    case PairingState::WaitingForCommit:
      return "waiting_for_commit";
    case PairingState::Committing:
      return "committing";
    default:
      return "idle";
  }
}

const char* BluetoothApplication::pairingFailureName() const {
  switch (pairingFailure_) {
    case PairingFailure::TableFull:
      return "app_table_full";
    case PairingFailure::TimedOut:
      return "timed_out";
    case PairingFailure::EncryptionFailed:
      return "encryption_failed";
    case PairingFailure::Disconnected:
      return "disconnected";
    case PairingFailure::StorageFailed:
      return "storage_failed";
    case PairingFailure::CommitInvalid:
      return "invalid_commit";
    case PairingFailure::None:
    default:
      return "none";
  }
}

bool BluetoothApplication::pairingLedOn(uint32_t nowMs) const {
  if (!pairingActive(nowMs)) {
    return false;
  }
  const uint32_t phase = nowMs % 1600UL;
  if (pairingState_ == PairingState::WaitingForConfirmation) {
    return phase < 300UL || (phase >= 500UL && phase < 575UL);
  }
  if (pairingState_ == PairingState::SecretReady) {
    return (phase % 240UL) < 120UL;
  }
  if (pairingState_ == PairingState::WaitingForCommit ||
      pairingState_ == PairingState::Committing) {
    return (phase % 320UL) < 160UL;
  }
  return phase < 100UL;
}

bool BluetoothApplication::connectionEncrypted() const {
  return connectionEncrypted_;
}

bool BluetoothApplication::connectionBonded() const {
  return connectionBonded_;
}

bool BluetoothApplication::identifying(uint32_t nowMs) const {
  return identifyUntilMs_ != 0 &&
         static_cast<int32_t>(identifyUntilMs_ - nowMs) > 0;
}

bool BluetoothApplication::identifyLedOn(uint32_t nowMs) const {
  return identifying(nowMs) && (nowMs % 180UL) < 90UL;
}
bool BluetoothApplication::controlEnabled() const { return controlEnabled_; }
bool BluetoothApplication::controlAuthenticated() const {
  return controlAuthenticated_;
}
uint32_t BluetoothApplication::controlAuthenticationFailureCount() const {
  return controlAuthenticationFailureCount_;
}
uint32_t BluetoothApplication::authenticationBackoffRemainingMs(
    uint32_t nowMs) const {
  return authenticationBackoffUntilMs_ != 0 &&
                 static_cast<int32_t>(authenticationBackoffUntilMs_ - nowMs) > 0
             ? authenticationBackoffUntilMs_ - nowMs
             : 0;
}
uint32_t BluetoothApplication::controlCommandCount() const {
  return controlCommandCount_;
}
const char* BluetoothApplication::lastControlResultName() const {
  return BleControlProtocol::resultName(lastControlResultCode_);
}
bool BluetoothApplication::synchronized() const { return synchronized_; }
bool BluetoothApplication::advertising() const { return advertising_; }
void BluetoothApplication::pauseAdvertising(bool paused) {
  advertisingPaused_ = paused;
}
bool BluetoothApplication::connected() const { return connected_; }
bool BluetoothApplication::subscribed() const { return subscribed_; }
uint8_t BluetoothApplication::connectionCount() const {
  return connected_ ? 1 : 0;
}
uint8_t BluetoothApplication::connectionLimit() const { return 1; }
uint32_t BluetoothApplication::acceptedCount() const { return acceptedCount_; }
uint32_t BluetoothApplication::closedCount() const { return closedCount_; }
uint32_t BluetoothApplication::notificationCount() const {
  return notificationCount_;
}
int32_t BluetoothApplication::lastError() const { return lastError_; }
const char* BluetoothApplication::lastErrorName() const {
  switch (lastError_) {
    case 0: return "none";
    case BLE_HS_ENOMEM: return "out_of_memory";
    case BLE_HS_EBUSY: return "busy";
    case BLE_HS_EALREADY: return "already_in_progress";
    case BLE_HS_ENOTCONN: return "not_connected";
    case BLE_HS_ETIMEOUT: return "timeout";
    case BLE_HS_EAUTHEN: return "authentication_failed";
    case BLE_HS_EAUTHOR: return "authorization_failed";
    case BLE_HS_EENCRYPT: return "encryption_failed";
    default: return "unknown";
  }
}
int32_t BluetoothApplication::lastDisconnectReason() const {
  return lastDisconnectReason_;
}
const char* BluetoothApplication::lastDisconnectReasonName() const {
  switch (lastDisconnectReason_) {
    case 0:
      return "none";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):
      return "connection_timeout";
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM):
      return "remote_user_terminated";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL):
      return "local_host_terminated";
    default:
      return "unknown";
  }
}
int8_t BluetoothApplication::connectionRssi() const {
  return connectionRssi_;
}
uint16_t BluetoothApplication::connectionMtu() const {
  return connectionMtu_;
}
bool BluetoothApplication::connectionMtuSufficient() const {
  return connectionMtu_ >= kRequiredControlMtu;
}
bool BluetoothApplication::bondCleanupPending() const {
  return bondCleanupPending_;
}
int32_t BluetoothApplication::bondCleanupLastError() const {
  return bondCleanupLastError_;
}
const char* BluetoothApplication::advertisedName() const {
  return advertisedName_;
}
const char* BluetoothApplication::protocolName() const { return "ost-ble/1"; }

int BluetoothApplication::gattAccess(uint16_t connHandle, uint16_t attrHandle,
                                     ble_gatt_access_ctxt* context,
                                     void*) {
  return active_ == nullptr ? BLE_ATT_ERR_UNLIKELY
                            : active_->onGattAccess(connHandle, attrHandle,
                                                    context);
}

int BluetoothApplication::gapEvent(ble_gap_event* event, void*) {
  return active_ == nullptr ? 0 : active_->onGapEvent(event);
}

void BluetoothApplication::hostTask(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void BluetoothApplication::hostReset(int reason) {
  if (active_ != nullptr) {
    active_->onHostReset(reason);
  }
}

void BluetoothApplication::hostSync() {
  if (active_ != nullptr) {
    active_->onHostSync();
  }
}

void BluetoothApplication::onHostReset(int reason) {
  networkRestorePending_.store(true);
  synchronized_ = false;
  advertising_ = false;
  connected_ = false;
  subscribed_ = false;
  eventSubscribed_ = false;
  controlResultSubscribed_ = false;
  clearConnectionAuthorization();
  connectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  connectionEncrypted_ = false;
  connectionBonded_ = false;
  connectionRssi_ = 127;
  connectionMtu_ = 23;
  connectionAcceptedAtMs_ = 0;
  if (physicalPairing_) {
    pairingFailure_ = PairingFailure::Disconnected;
    cancelPairing();
    pairingState_ = PairingState::Failed;
  }
  lastError_ = reason;
}

void BluetoothApplication::onHostSync() {
  const int addressResult = ble_hs_util_ensure_addr(0);
  if (addressResult != 0) {
    lastError_ = addressResult;
    return;
  }
  synchronized_ = true;
  if (enabled_) {
    startAdvertising();
  }
}

int BluetoothApplication::onGapEvent(ble_gap_event* event) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      advertising_ = false;
      if (event->connect.status == 0) {
        const uint32_t nowMs = millis();
        connected_ = true;
        subscribed_ = false;
        eventSubscribed_ = false;
        controlResultSubscribed_ = false;
        managementResponseSubscribed_ = false;
        clearConnectionAuthorization();
        connectionEncrypted_ = false;
        connectionBonded_ = false;
        connectionHandle_ = event->connect.conn_handle;
        acceptedCount_ = acceptedCount_ + 1;
        connectionAcceptedAtMs_ = nowMs == 0 ? 1 : nowMs;
        lastPeerActivityMs_ = connectionAcceptedAtMs_;
        connectionRssi_ = 127;
        connectionMtu_ = 23;
        lastConnectionRssiAtMs_ = 0;
        lastError_ = 0;
        rotateControlChallenge();
        updateControlChallengeFrame();
        if (!enabled_) {
          ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
        } else {
          const bool pairingConnection =
              physicalPairing_ && pairingActive(nowMs) &&
              acceptedCount_ > pairingAcceptAfterCount_;
          const bool alreadyBonded =
              connectionPeerIsBonded(connectionHandle_);
          const bool knownController = !pairingConnection &&
              controlEnabled_ && alreadyBonded;
          bool initiateSecurity = false;
          if (pairingConnection) {
            // A browser can lose its application key while the OS retains
            // this bond. Reuse that encrypted link for physical re-approval.
            // Connecting/authenticating alone never reveals the pending key
            // or replaces the old controller: Reset + Pairing Commit remain
            // required. Close other apps before selecting the intended peer.
            pairingConnectionHandle_ = connectionHandle_;
            pairingState_ = PairingState::WaitingForEncryption;
            initiateSecurity = true;
          } else if (knownController) {
            initiateSecurity = true;
          }
          // Unknown normal connections remain metadata-only and are closed by
          // the bounded unauthenticated timeout. Only an existing bond or the
          // physically selected pairing connection starts security here.
          if (initiateSecurity) {
            refreshConnectionSecurity();
            const int securityResult =
                ble_gap_security_initiate(connectionHandle_);
            if (securityResult != 0 && securityResult != BLE_HS_EALREADY) {
              lastError_ = securityResult;
              if (pairingConnection) {
                pairingFailure_ = PairingFailure::EncryptionFailed;
                pairingState_ = PairingState::Failed;
              }
            }
          }
        }
      } else {
        lastError_ = event->connect.status;
        startAdvertising();
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
      networkRestorePending_.store(true);
      connected_ = false;
      subscribed_ = false;
      eventSubscribed_ = false;
      controlResultSubscribed_ = false;
      managementResponseSubscribed_ = false;
      const uint16_t disconnectedHandle = connectionHandle_;
      clearConnectionAuthorization();
      connectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
      connectionEncrypted_ = false;
      connectionBonded_ = false;
      lastDisconnectReason_ = event->disconnect.reason;
      closedCount_ = closedCount_ + 1;
      connectionRssi_ = 127;
      connectionMtu_ = 23;
      connectionAcceptedAtMs_ = 0;
      lastPeerActivityMs_ = 0;
      lastConnectionRssiAtMs_ = 0;
      rotateControlChallenge();
      updateControlChallengeFrame();
      if (physicalPairing_ && pairingActive(millis()) &&
          pairingConnectionHandle_ == disconnectedHandle) {
        if (pairingState_ == PairingState::Committing ||
            pendingPairingCommit_) {
          pairingFailure_ = PairingFailure::Disconnected;
          cancelPairing();
          pairingState_ = PairingState::Failed;
        } else {
          pairingState_ = PairingState::WaitingForConnection;
          pairingSecretAvailable_ = false;
          pairingConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
          pairingSecretUntilMs_ = 0;
          pairingAcceptAfterCount_ = acceptedCount_;
        }
      }
      startAdvertising();
      return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.conn_handle == connectionHandle_) {
        if (event->enc_change.status != 0) {
          lastError_ = event->enc_change.status;
          if (physicalPairing_ && pairingActive(millis())) {
            pairingFailure_ = PairingFailure::EncryptionFailed;
            pairingState_ = PairingState::Failed;
          }
          return 0;
        }
        refreshConnectionSecurity();
        if (physicalPairing_ && pairingActive(millis()) &&
            pairingConnectionHandle_ == connectionHandle_) {
          if (connectionEncrypted_ && connectionBonded_) {
            pairingState_ = PairingState::WaitingForConfirmation;
          } else {
            pairingFailure_ = PairingFailure::EncryptionFailed;
            pairingState_ = PairingState::Failed;
          }
        }
      }
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      if (!physicalPairing_ || !pairingActive(millis())) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
      }
      ble_gap_conn_desc descriptor{};
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                            &descriptor) != 0) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
      }
      if (retainsApprovedPeer(descriptor.peer_id_addr.type,
                              descriptor.peer_id_addr.val)) {
        ble_gap_terminate(event->repeat_pairing.conn_handle,
                          BLE_ERR_REM_USER_CONN_TERM);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
      }
      ble_store_util_delete_peer(&descriptor.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertising_ = false;
      startAdvertising();
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == statusValueHandle_) {
        subscribed_ = event->subscribe.cur_notify != 0;
        if (subscribed_ && controlAuthenticated_) {
          statusNotificationRequested_ = true;
        }
      } else if (event->subscribe.attr_handle == eventValueHandle_) {
        eventSubscribed_ = event->subscribe.cur_notify != 0;
        if (eventSubscribed_ && controlAuthenticated_) {
          eventReplayRequested_ = true;
          nextEventNotificationId_ = 0;
        }
      } else if (event->subscribe.attr_handle == controlResultValueHandle_) {
        controlResultSubscribed_ = event->subscribe.cur_notify != 0;
      } else if (event->subscribe.attr_handle == managementResponseValueHandle_) {
        managementResponseSubscribed_ = event->subscribe.cur_notify != 0;
      }
      return 0;

    case BLE_GAP_EVENT_MTU:
      if (event->mtu.conn_handle == connectionHandle_ &&
          event->mtu.channel_id == BLE_L2CAP_CID_ATT) {
        connectionMtu_ = event->mtu.value;
      }
      return 0;

    default:
      return 0;
  }
}

void BluetoothApplication::rotateControlChallenge() {
  uint8_t challenge[kControlChallengeSize];
  for (size_t offset = 0; offset < sizeof(challenge); offset += 4) {
    writeUint32Le(challenge + offset, Platform::random32());
  }
  portENTER_CRITICAL(&controlMux_);
  memcpy(controlChallengeFrame_ + 8, challenge, sizeof(challenge));
  portEXIT_CRITICAL(&controlMux_);
}

void BluetoothApplication::updateControlChallengeFrame() {
  portENTER_CRITICAL(&controlMux_);
  controlChallengeFrame_[0] = BleControlProtocol::kProtocolVersion;
  controlChallengeFrame_[1] =
      (controlEnabled_ ? 0x01 : 0) | (controlAuthenticated_ ? 0x02 : 0) |
      (pairingCommitConfirmed_ ? 0x04 : 0);
  controlChallengeFrame_[2] = static_cast<uint8_t>(
      controlAuthenticationFailureCount_ > 255
          ? 255
          : controlAuthenticationFailureCount_);
  controlChallengeFrame_[3] = kMaximumAuthenticationFailures;
  writeUint32Le(controlChallengeFrame_ + 4, bootId_);
  portEXIT_CRITICAL(&controlMux_);
}

bool BluetoothApplication::authenticateControl(const uint8_t* frame,
                                                uint32_t nowMs) {
  if (!connected_ || !connectionEncrypted_ || !connectionBonded_ ||
      !controlEnabled_ ||
      (lastAuthenticationAttemptAtMs_ != 0 &&
       nowMs - lastAuthenticationAttemptAtMs_ < kAuthenticationRetryMs)) {
    return false;
  }
  lastAuthenticationAttemptAtMs_ = nowMs == 0 ? 1 : nowMs;

  static constexpr char kLabel[] = "OST-AUTH-v1";
  static constexpr size_t kLabelLength = sizeof(kLabel) - 1;
  uint8_t message[kLabelLength + 4 + kControlChallengeSize + 4]{};
  memcpy(message, kLabel, kLabelLength);
  writeUint32Le(message + kLabelLength, bootId_);
  uint8_t key[kControlKeySize]{};
  BleAppRegistry::App candidates[BleAppRegistry::kCapacity]{};
  uint32_t generation;
  portENTER_CRITICAL(&controlMux_);
  memcpy(message + kLabelLength + 4, controlChallengeFrame_ + 8,
         kControlChallengeSize);
  memcpy(candidates, controlApps_, sizeof(candidates));
  generation = authorizationGeneration_;
  portEXIT_CRITICAL(&controlMux_);
  memcpy(message + kLabelLength + 4 + kControlChallengeSize, frame + 2, 4);

  uint8_t digest[32]{};
  ble_gap_conn_desc peer{};
  const bool havePeer = ble_gap_conn_find(connectionHandle_, &peer) == 0;
  // Always evaluate all slots, including empty slots. A failed request counts
  // once, not once per candidate. Ordinary commands use only the session key.
  const int selected = BleAppRegistry::authenticate(candidates, peer.peer_id_addr.type,
      havePeer ? peer.peer_id_addr.val : nullptr, [&](const uint8_t* candidateKey) {
        const bool computed = controlHmac(candidateKey, kControlKeySize, message, sizeof(message), digest);
        const bool matched = constantTimeEqual(digest, frame + 6, kAuthenticationMacSize);
        return computed && matched;
      });
  const uint32_t appId = selected >= 0 ? candidates[selected].id : 0;
  if (selected >= 0) memcpy(key, candidates[selected].key, sizeof(key));
  memset(candidates, 0, sizeof(candidates));
  const bool accepted = selected >= 0 && generation == authorizationGeneration_ &&
                        controlEnabled_ && connected_ && connectionEncrypted_ &&
                        connectionBonded_;
  memset(digest, 0, sizeof(digest));
  if (accepted) {
    static constexpr char kSessionLabel[] = "OST-SESSION-v1";
    static constexpr size_t kSessionLabelLength = sizeof(kSessionLabel) - 1;
    uint8_t sessionMessage[kSessionLabelLength + 4 +
                           kControlChallengeSize + 4]{};
    memcpy(sessionMessage, kSessionLabel, kSessionLabelLength);
    memcpy(sessionMessage + kSessionLabelLength,
           message + kLabelLength, 4 + kControlChallengeSize + 4);
    uint8_t sessionKey[kSessionKeySize]{};
    const bool sessionDerived =
        controlHmac(key, sizeof(key), sessionMessage,
                    sizeof(sessionMessage), sessionKey);
    memset(sessionMessage, 0, sizeof(sessionMessage));
    if (!sessionDerived) {
      memset(key, 0, sizeof(key));
      memset(message, 0, sizeof(message));
      return false;
    }
    portENTER_CRITICAL(&controlMux_);
    if (generation != authorizationGeneration_ || controlApps_[selected].id != appId) {
      portEXIT_CRITICAL(&controlMux_);
      memset(sessionKey, 0, sizeof(sessionKey));
      memset(key, 0, sizeof(key));
      memset(message, 0, sizeof(message));
      return false;
    }
    authenticatedAppId_ = appId;
    authenticatedAppSlot_ = selected;
    authorizationGeneration_ = authorizationGeneration_ + 1;
    pendingControl_ = false;
    pendingControlAuthorized_ = false;
    appLastSeenMs_[selected] = nowMs ? nowMs : 1;
    memset(controlResultFrame_, 0, sizeof(controlResultFrame_));
    controlResultFrame_[0] = BleControlProtocol::kProtocolVersion;
    controlResultFrame_[4] = static_cast<uint8_t>(BleControlResultCode::Unauthorized);
    writeUint32Le(controlResultFrame_ + 8, bootId_);
    if (appResultIds_[selected] == appId)
      memcpy(controlResultFrame_, appResults_[selected], sizeof(controlResultFrame_));
    memcpy(sessionKey_, sessionKey, sizeof(sessionKey_));
    lastCommandCounter_ = 0;
    memset(lastAuthenticatedCommand_, 0,
           sizeof(lastAuthenticatedCommand_));
    controlAuthenticated_ = true;
    controlAuthenticationFailureCount_ = 0;
    portEXIT_CRITICAL(&controlMux_);
    memset(sessionKey, 0, sizeof(sessionKey));
    memset(key, 0, sizeof(key));
    memset(message, 0, sizeof(message));
    clearManagementState();  // Never serve a previous session's reply.
    rotateControlChallenge();
    updateControlChallengeFrame();
    statusNotificationRequested_ = true;
    eventReplayRequested_ = true;
    nextEventNotificationId_ = 0;
    return true;
  }

  memset(key, 0, sizeof(key));
  memset(message, 0, sizeof(message));
  failAuthentication(nowMs);
  return false;
}

bool BluetoothApplication::authenticateCommand(const uint8_t* frame) {
  if (frame == nullptr || !connected_ || !connectionEncrypted_ ||
      !connectionBonded_ || !controlEnabled_ || !controlAuthenticated_) {
    return false;
  }
  const uint32_t counter =
      static_cast<uint32_t>(frame[20]) |
      static_cast<uint32_t>(frame[21]) << 8 |
      static_cast<uint32_t>(frame[22]) << 16 |
      static_cast<uint32_t>(frame[23]) << 24;
  if (counter == 0) {
    return false;
  }

  static constexpr char kLabel[] = "OST-CMD-v1";
  static constexpr size_t kLabelLength = sizeof(kLabel) - 1;
  uint8_t message[kLabelLength + kControlAuthenticatedPrefixSize]{};
  memcpy(message, kLabel, kLabelLength);
  memcpy(message + kLabelLength, frame, kControlAuthenticatedPrefixSize);
  uint8_t sessionKey[kSessionKeySize]{};
  portENTER_CRITICAL(&controlMux_);
  memcpy(sessionKey, sessionKey_, sizeof(sessionKey));
  const uint32_t previousCounter = lastCommandCounter_;
  const bool exactRetry = counter == previousCounter &&
      memcmp(frame, lastAuthenticatedCommand_, kControlFrameSize) == 0;
  portEXIT_CRITICAL(&controlMux_);

  uint8_t digest[32]{};
  const bool valid =
      controlHmac(sessionKey, sizeof(sessionKey), message,
                  sizeof(message), digest) &&
      constantTimeEqual(digest, frame + kControlAuthenticatedPrefixSize,
                        kControlMacSize) &&
      (counter > previousCounter || exactRetry);
  memset(sessionKey, 0, sizeof(sessionKey));
  memset(message, 0, sizeof(message));
  memset(digest, 0, sizeof(digest));
  if (!valid) {
    failAuthentication(millis());
    return false;
  }

  if (!exactRetry) {
    portENTER_CRITICAL(&controlMux_);
    lastCommandCounter_ = counter;
    memcpy(lastAuthenticatedCommand_, frame,
           sizeof(lastAuthenticatedCommand_));
    portEXIT_CRITICAL(&controlMux_);
  }
  return true;
}

bool BluetoothApplication::queueControlCommand(uint16_t connHandle,
                                               const uint8_t* frame) {
  if (!connected_ || connHandle != connectionHandle_ ||
      !connectionMtuSufficient() ||
      !authenticateCommand(frame)) {
    return false;
  }
  bool accepted = false;
  portENTER_CRITICAL(&controlMux_);
  if (!pendingControl_) {
    memcpy(pendingControlFrame_, frame, kControlFrameSize);
    pendingControlConnectionHandle_ = connHandle;
    pendingControlAppId_ = authenticatedAppId_;
    pendingControlAppSlot_ = authenticatedAppSlot_;
    pendingControlGeneration_ = authorizationGeneration_;
    pendingControlAuthorized_ = controlEnabled_ && controlAuthenticated_;
    pendingControl_ = true;
    accepted = true;
  }
  portEXIT_CRITICAL(&controlMux_);
  return accepted;
}

bool BluetoothApplication::queuePairingCommit(uint16_t connHandle,
                                              const uint8_t* frame) {
  if (frame == nullptr || !connected_ || !connectionEncrypted_ ||
      !connectionBonded_ ||
      connHandle != connectionHandle_ ||
      connHandle != pairingConnectionHandle_ || !physicalPairing_ ||
      (pairingState_ != PairingState::SecretReady &&
       pairingState_ != PairingState::WaitingForCommit) ||
      !validatePairingCommit(frame)) {
    pairingFailure_ = PairingFailure::CommitInvalid;
    return false;
  }
  bool accepted = false;
  portENTER_CRITICAL(&controlMux_);
  if (!pendingPairingCommit_) {
    memcpy(pendingPairingCommitFrame_, frame,
           sizeof(pendingPairingCommitFrame_));
    pendingPairingCommitConnectionHandle_ = connHandle;
    pendingPairingCommit_ = true;
    pairingState_ = PairingState::Committing;
    accepted = true;
  }
  portEXIT_CRITICAL(&controlMux_);
  return accepted;
}

void BluetoothApplication::processControlCommand(uint32_t nowMs,
                                                 bool mutationsAllowed) {
  uint8_t frame[kControlFrameSize]{};
  bool authorized = false;
  uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
  uint32_t appId = 0, generation = 0;
  uint8_t appSlot = 0;
  portENTER_CRITICAL(&controlMux_);
  if (!pendingControl_) {
    portEXIT_CRITICAL(&controlMux_);
    return;
  }
  memcpy(frame, pendingControlFrame_, sizeof(frame));
  authorized = pendingControlAuthorized_;
  connHandle = pendingControlConnectionHandle_;
  appId = pendingControlAppId_;
  appSlot = pendingControlAppSlot_;
  generation = pendingControlGeneration_;
  pendingControl_ = false;
  pendingControlAuthorized_ = false;
  pendingControlConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  portEXIT_CRITICAL(&controlMux_);

  authorized = authorized && connected_ && connHandle == connectionHandle_ &&
      controlAuthenticated_ && generation == authorizationGeneration_ &&
      appId == authenticatedAppId_ && config_.bluetoothApps().find(appId) == appSlot;
#if defined(SMART_TIMER_HARDWARE_V2)
  timer_.setBluetoothEventAppId(authorized ? appId : 0);
#endif
  const BleControlExecution execution = controlProtocol_.execute(
      frame, authorized, nowMs, Platform::random32(), mutationsAllowed, appId, appSlot);
  timer_.setBluetoothEventAppId(0);
  if (execution.code == BleControlResultCode::Applied && execution.changed &&
      !execution.duplicate &&
      frame[1] == static_cast<uint8_t>(BleControlOpcode::Identify)) {
    identifyUntilMs_ = nowMs + kIdentifyWindowMs;
  }
  // Publish a current readable snapshot before acknowledging the mutation.
  // A client reading immediately after the result must not see the prior
  // 250 ms cached frame. For a duplicate, this is current state, not the
  // historical state stored in the operation result.
  updateStatusFrame(nowMs);
  portENTER_CRITICAL(&controlMux_);
  if (authorized) {
    appResultIds_[appSlot] = appId;
    memcpy(appResults_[appSlot], execution.frame, sizeof(controlResultFrame_));
    appLastSeenMs_[appSlot] = nowMs ? nowMs : 1;
  }
  const bool sameSession = generation == authorizationGeneration_ && appId == authenticatedAppId_;
  if (sameSession) memcpy(controlResultFrame_, execution.frame, sizeof(controlResultFrame_));
  lastControlResultCode_ = execution.code;
  portEXIT_CRITICAL(&controlMux_);
  controlCommandCount_ = controlCommandCount_ + 1;
  if (sameSession) sendControlResultNotification();
  statusNotificationRequested_ = true;
}

void BluetoothApplication::processPairingCommit(uint32_t nowMs) {
  uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
  portENTER_CRITICAL(&controlMux_);
  if (!pendingPairingCommit_) {
    portEXIT_CRITICAL(&controlMux_);
    return;
  }
  connHandle = pendingPairingCommitConnectionHandle_;
  pendingPairingCommit_ = false;
  pendingPairingCommitConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  memset(pendingPairingCommitFrame_, 0,
         sizeof(pendingPairingCommitFrame_));
  portEXIT_CRITICAL(&controlMux_);

  if (!timer_.isOpen() || !physicalPairing_ || !connected_ ||
      !connectionEncrypted_ || connHandle != connectionHandle_ ||
      connHandle != pairingConnectionHandle_) {
    pairingFailure_ = PairingFailure::Disconnected;
    cancelPairing();
    pairingState_ = PairingState::Failed;
    return;
  }

  const bool peerRemembered = rememberCurrentPeerForCleanup();
  const auto& apps = config_.bluetoothApps();
  if (!peerRemembered || pairingAppSlot_ < 0 ||
      apps.apps[pairingAppSlot_].id != pairingReplaceId_ ||
      !config_.saveBluetoothApp(pairingAppSlot_, pendingPairingKey_, pairingAppLabel_,
                                preservedBondAddressType_, preservedBondAddress_) ||
      !loadPersistentControlKey()) {
    pairingFailure_ = PairingFailure::StorageFailed;
    cancelPairing();
    pairingState_ = PairingState::Failed;
    return;
  }

  finishPhysicalPairing(nowMs);
  // Pairing Commit is accepted in the main loop after the write response was
  // queued. Publish an explicit connection-local confirmation bit, and start
  // a fresh authentication grace period so a careful human approval cannot
  // make the just-committed connection immediately time out.
  pairingCommitConfirmed_ = true;
  connectionAcceptedAtMs_ = nowMs == 0 ? 1 : nowMs;
  updateControlChallengeFrame();
  preserveCurrentBond_ = peerRemembered;
  bondCleanupPending_ = peerRemembered;
  nextBondCleanupAtMs_ = 0;
  if (bondCleanupPending_) {
    attemptBondCleanup(nowMs);
  }
  statusNotificationRequested_ = true;
  eventReplayRequested_ = true;
  nextEventNotificationId_ = 0;
}

bool BluetoothApplication::sendControlResultNotification() {
  if (!connected_ || !connectionEncrypted_ || !controlAuthenticated_ ||
      !controlResultSubscribed_ ||
      controlResultValueHandle_ == 0 ||
      connectionHandle_ == BLE_HS_CONN_HANDLE_NONE) {
    return false;
  }
  uint8_t frame[kControlResultFrameSize];
  portENTER_CRITICAL(&controlMux_);
  memcpy(frame, controlResultFrame_, sizeof(frame));
  portEXIT_CRITICAL(&controlMux_);
  os_mbuf* packet = ble_hs_mbuf_from_flat(frame, sizeof(frame));
  if (packet == nullptr) {
    lastError_ = BLE_HS_ENOMEM;
    return false;
  }
  const int result = ble_gatts_notify_custom(
      connectionHandle_, controlResultValueHandle_, packet);
  if (result != 0) {
    if (result != BLE_HS_ENOTCONN) {
      lastError_ = result;
    }
    return false;
  }
  notificationCount_ = notificationCount_ + 1;
  return true;
}

void BluetoothApplication::clearControlKey() {
  for (BleAppRegistry::App& app : controlApps_) app = BleAppRegistry::App{};
  volatile uint8_t* key = controlKey_;
  for (size_t index = 0; index < sizeof(controlKey_); ++index) {
    key[index] = 0;
  }
}

void BluetoothApplication::clearSessionKey() {
  volatile uint8_t* key = sessionKey_;
  for (size_t index = 0; index < sizeof(sessionKey_); ++index) {
    key[index] = 0;
  }
}

void BluetoothApplication::clearConnectionAuthorization() {
  portENTER_CRITICAL(&controlMux_);
  controlAuthenticated_ = false;
  authenticatedAppId_ = 0;
  authorizationGeneration_ = authorizationGeneration_ + 1;
  pendingControl_ = false;
  pendingControlAuthorized_ = false;
  pendingControlConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  pendingPairingCommit_ = false;
  pendingPairingCommitConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  memset(pendingControlFrame_, 0, sizeof(pendingControlFrame_));
  memset(pendingPairingCommitFrame_, 0,
         sizeof(pendingPairingCommitFrame_));
  memset(lastAuthenticatedCommand_, 0,
         sizeof(lastAuthenticatedCommand_));
  lastCommandCounter_ = 0;
  lastAuthenticationAttemptAtMs_ = 0;
  pairingCommitConfirmed_ = false;
  clearSessionKey();
  portEXIT_CRITICAL(&controlMux_);
  clearManagementState();
  statusNotificationRequested_ = false;
  eventReplayRequested_ = false;
  nextEventNotificationId_ = 0;
}

void BluetoothApplication::failAuthentication(uint32_t nowMs) {
  portENTER_CRITICAL(&controlMux_);
  controlAuthenticated_ = false;
  authenticatedAppId_ = 0;
  authorizationGeneration_ = authorizationGeneration_ + 1;
  pendingControl_ = false;
  pendingControlAuthorized_ = false;
  clearSessionKey();
  memset(lastAuthenticatedCommand_, 0,
         sizeof(lastAuthenticatedCommand_));
  lastCommandCounter_ = 0;
  controlAuthenticationFailureCount_ =
      controlAuthenticationFailureCount_ + 1;
  const bool backoff = controlAuthenticationFailureCount_ >=
                       kMaximumAuthenticationFailures;
  portEXIT_CRITICAL(&controlMux_);
  rotateControlChallenge();
  updateControlChallengeFrame();
  if (backoff) {
    authenticationBackoffUntilMs_ = nowMs + kAuthenticationBackoffMs;
    if (connected_) {
      ble_gap_terminate(connectionHandle_, BLE_ERR_REM_USER_CONN_TERM);
    }
  }
}

bool BluetoothApplication::loadPersistentControlKey(bool preserveSession) {
  portENTER_CRITICAL(&controlMux_);
  for (uint8_t slot = 0; slot < BleAppRegistry::kCapacity; ++slot) {
    const auto& next = config_.bluetoothApps().apps[slot];
    if (knownAppIds_[slot] != next.id) {
      appLastSeenMs_[slot] = 0;
      appResultIds_[slot] = 0;
      memset(appResults_[slot], 0, sizeof(appResults_[slot]));
    }
    knownAppIds_[slot] = next.id;
    controlApps_[slot] = next;
  }
  controlEnabled_ = enabled_ && config_.bluetoothPaired();
  const bool retain = preserveSession && controlEnabled_ &&
      config_.bluetoothApps().find(authenticatedAppId_) >= 0;
  if (!retain) {
    authenticatedAppId_ = 0;
    authorizationGeneration_ = authorizationGeneration_ + 1;
    controlAuthenticated_ = false;
    pendingControl_ = false;
    pendingControlAuthorized_ = false;
    lastCommandCounter_ = 0;
    memset(lastAuthenticatedCommand_, 0, sizeof(lastAuthenticatedCommand_));
    clearSessionKey();
  }
  portEXIT_CRITICAL(&controlMux_);
  if (!retain) {
    rotateControlChallenge();
    updateControlChallengeFrame();
  }
  return controlEnabled_;
}

bool BluetoothApplication::generatePairingKey(uint8_t* destination) {
  if (destination == nullptr) {
    return false;
  }
  uint8_t nonzero = 0;
  // V2 reaches this path only after nimble_port_init(), so esp_random() is
  // backed by the initialized RF entropy source rather than early-boot PRNG
  // state.
  for (size_t offset = 0; offset < kControlKeySize; offset += 4) {
    const uint32_t value = Platform::random32();
    writeUint32Le(destination + offset, value);
    nonzero |= destination[offset] | destination[offset + 1] |
               destination[offset + 2] | destination[offset + 3];
  }
  return nonzero != 0;
}

bool BluetoothApplication::initializePairing(uint32_t nowMs, bool physical,
                                             uint32_t bootId) {
  if (!timer_.isOpen() || (!initialized_ && !begin(bootId)) ||
      !enabled_ || !generatePairingKey(pendingPairingKey_)) {
    return false;
  }
  // Reclaim only orphan bonds before admitting another candidate. Approved
  // peers (including shared phone bonds) remain intact throughout replacement.
  preserveCurrentBond_ = true;
  if (!attemptBondCleanup(nowMs)) return false;
  physicalPairing_ = physical;
  ble_hs_cfg.sm_bonding = 1;
  pairingFailure_ = PairingFailure::None;
  pairingSecretAvailable_ = false;
  pairingCommitConfirmed_ = false;
  pairingUntilMs_ = nowMs + kPairingWindowMs;
  pairingSecretUntilMs_ = 0;
  pairingConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  pairingAcceptAfterCount_ = acceptedCount_;
  pairingState_ = PairingState::WaitingForConnection;
  // A controller that was connected before the user opened the pairing window
  // can never become its target. Free the single slot and require a fresh
  // post-window connection.
  if (connected_) {
    const int result = ble_gap_terminate(connectionHandle_,
                                         BLE_ERR_REM_USER_CONN_TERM);
    if (result != 0 && result != BLE_HS_EALREADY) {
      lastError_ = result;
      pairingFailure_ = PairingFailure::Disconnected;
      pairingState_ = PairingState::Failed;
      return false;
    }
  }
  return true;
}

bool BluetoothApplication::validatePairingCommit(const uint8_t* frame) const {
  if (frame == nullptr ||
      frame[0] != BleControlProtocol::kProtocolVersion ||
      frame[1] != static_cast<uint8_t>(BleControlOpcode::PairingCommit)) {
    return false;
  }
  static constexpr char kLabel[] = "OST-PAIR-COMMIT-v1";
  static constexpr size_t kLabelLength = sizeof(kLabel) - 1;
  uint8_t message[kLabelLength + 4 + 4]{};
  memcpy(message, kLabel, kLabelLength);
  writeUint32Le(message + kLabelLength, bootId_);
  memcpy(message + kLabelLength + 4, frame + 2, 4);
  uint8_t digest[32]{};
  const bool valid = controlHmac(pendingPairingKey_,
                                 sizeof(pendingPairingKey_), message,
                                 sizeof(message), digest) &&
                     constantTimeEqual(digest, frame + 6, kAuthenticationMacSize);
  memset(message, 0, sizeof(message));
  memset(digest, 0, sizeof(digest));
  return valid;
}

void BluetoothApplication::finishPhysicalPairing(uint32_t nowMs) {
  ble_hs_cfg.sm_bonding = 0;
  memset(pendingPairingKey_, 0, sizeof(pendingPairingKey_));
  pairingSecretAvailable_ = false;
  physicalPairing_ = false;
  pairingState_ = PairingState::Complete;
  pairingUntilMs_ = 0;
  pairingSecretUntilMs_ = 0;
  pairingConnectionHandle_ = BLE_HS_CONN_HANDLE_NONE;
  pairingFailure_ = PairingFailure::None;
  timer_.recordEvent(TimerEventType::BluetoothPairingCompleted, nowMs);
}

bool BluetoothApplication::connectionPeerIsBonded(
    uint16_t connectionHandle) const {
  ble_gap_conn_desc descriptor{};
  if (ble_gap_conn_find(connectionHandle, &descriptor) != 0) {
    return false;
  }
  ble_addr_t peers[4]{};
  int peerCount = 0;
  if (ble_store_util_bonded_peers(peers, &peerCount, 4) != 0) {
    return false;
  }
  for (int index = 0; index < peerCount; ++index) {
    if (ble_addr_cmp(&peers[index], &descriptor.peer_id_addr) == 0) {
      return true;
    }
  }
  return false;
}

bool BluetoothApplication::retainsApprovedPeer(uint8_t type, const uint8_t* address) const {
  bool retained = false;
  portENTER_CRITICAL(&controlMux_);
  for (const auto& app : controlApps_) {
    if (app.id && (!app.peerKnown || (app.peerType == type && !memcmp(app.peerAddress, address, 6)))) retained = true;
  }
  portEXIT_CRITICAL(&controlMux_);
  return retained;
}

bool BluetoothApplication::rememberCurrentPeerForCleanup() {
  ble_gap_conn_desc descriptor{};
  if (!connected_ ||
      ble_gap_conn_find(connectionHandle_, &descriptor) != 0) {
    return false;
  }
  preservedBondAddressType_ = descriptor.peer_id_addr.type;
  memcpy(preservedBondAddress_, descriptor.peer_id_addr.val,
         sizeof(preservedBondAddress_));
  return true;
}

bool BluetoothApplication::attemptBondCleanup(uint32_t nowMs) {
  if (!initialized_) {
    bondCleanupPending_ = false;
    bondCleanupLastError_ = 0;
    return true;
  }
  int result = 0;
  if (!preserveCurrentBond_) {
    result = ble_store_clear();
  } else {
    ble_addr_t peers[4]{};
    int peerCount = 0;
    result = ble_store_util_bonded_peers(peers, &peerCount, 4);
    for (int index = 0; result == 0 && index < peerCount; ++index) {
      const bool preserved = config_.bluetoothApps().retainsPeer(
          peers[index].type, peers[index].val);
      if (!preserved) {
        result = ble_store_util_delete_peer(&peers[index]);
      }
    }
  }
  bondCleanupLastError_ = result;
  bondCleanupPending_ = result != 0;
  nextBondCleanupAtMs_ = result == 0 ? 0 : nowMs + kBondCleanupRetryMs;
  if (result == 0) {
    preserveCurrentBond_ = false;
  }
  return result == 0;
}

void BluetoothApplication::refreshConnectionSecurity() {
  if (!connected_ || connectionHandle_ == BLE_HS_CONN_HANDLE_NONE) {
    connectionEncrypted_ = false;
    connectionBonded_ = false;
    return;
  }
  ble_gap_conn_desc descriptor{};
  if (ble_gap_conn_find(connectionHandle_, &descriptor) != 0) {
    connectionEncrypted_ = false;
    connectionBonded_ = false;
    return;
  }
  connectionEncrypted_ = descriptor.sec_state.encrypted != 0;
  connectionBonded_ = descriptor.sec_state.bonded != 0;
}

bool BluetoothApplication::controlHmac(const uint8_t* key,
                                       size_t keyLength,
                                       const uint8_t* message,
                                       size_t messageLength,
                                       uint8_t* output) {
  uint8_t innerPad[64];
  uint8_t outerPad[64];
  for (size_t index = 0; index < sizeof(innerPad); ++index) {
    const uint8_t keyByte = index < keyLength ? key[index] : 0;
    innerPad[index] = keyByte ^ 0x36;
    outerPad[index] = keyByte ^ 0x5c;
  }
  uint8_t innerDigest[32]{};
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool success = mbedtls_sha256_starts(&context, 0) == 0 &&
                 mbedtls_sha256_update(&context, innerPad,
                                       sizeof(innerPad)) == 0 &&
                 mbedtls_sha256_update(&context, message,
                                       messageLength) == 0 &&
                 mbedtls_sha256_finish(&context, innerDigest) == 0;
  mbedtls_sha256_free(&context);
  if (success) {
    mbedtls_sha256_init(&context);
    success = mbedtls_sha256_starts(&context, 0) == 0 &&
              mbedtls_sha256_update(&context, outerPad,
                                    sizeof(outerPad)) == 0 &&
              mbedtls_sha256_update(&context, innerDigest,
                                    sizeof(innerDigest)) == 0 &&
              mbedtls_sha256_finish(&context, output) == 0;
    mbedtls_sha256_free(&context);
  }
  memset(innerPad, 0, sizeof(innerPad));
  memset(outerPad, 0, sizeof(outerPad));
  memset(innerDigest, 0, sizeof(innerDigest));
  return success;
}

void BluetoothApplication::updateEventFrame(const TimerEvent& event,
                                            bool truncated) {
  uint8_t frame[kEventFrameSize]{};
  frame[0] = BleControlProtocol::kProtocolVersion;
  frame[1] = eventCode(event.type);
  frame[2] = stateCode(event.state);
  if (event.sealed) {
    frame[3] |= 0x01;
  }
  if (event.emergencyInputActive) {
    frame[3] |= 0x02;
  }
  if (truncated) {
    frame[3] |= 0x04;
  }
  writeUint32Le(frame + 4, event.id);
  writeUint32Le(frame + 8, event.atMs);
  writeUint32Le(frame + 12, static_cast<uint32_t>(event.valueMs));
  writeUint32Le(frame + 16, event.sessionSequence);
  portENTER_CRITICAL(&frameMux_);
  memcpy(eventFrame_, frame, sizeof(eventFrame_));
  portEXIT_CRITICAL(&frameMux_);
}

bool BluetoothApplication::sendEventNotification() {
  if (!connected_ || !connectionEncrypted_ || !controlAuthenticated_ ||
      !eventSubscribed_ || eventValueHandle_ == 0 ||
      connectionHandle_ == BLE_HS_CONN_HANDLE_NONE) {
    return false;
  }
  uint8_t frame[kEventFrameSize]{};
  portENTER_CRITICAL(&frameMux_);
  memcpy(frame, eventFrame_, sizeof(frame));
  portEXIT_CRITICAL(&frameMux_);
  os_mbuf* packet = ble_hs_mbuf_from_flat(frame, sizeof(frame));
  if (packet == nullptr) {
    lastError_ = BLE_HS_ENOMEM;
    return false;
  }
  const int result = ble_gatts_notify_custom(
      connectionHandle_, eventValueHandle_, packet);
  if (result != 0) {
    if (result != BLE_HS_ENOTCONN) {
      lastError_ = result;
    }
    return false;
  }
  notificationCount_ = notificationCount_ + 1;
  return true;
}

bool BluetoothApplication::constantTimeEqual(const uint8_t* left,
                                             const uint8_t* right,
                                             size_t length) {
  uint8_t difference = 0;
  for (size_t index = 0; index < length; ++index) {
    difference |= left[index] ^ right[index];
  }
  return difference == 0;
}

int BluetoothApplication::onGattAccess(uint16_t connHandle,
                                       uint16_t attrHandle,
                                       ble_gatt_access_ctxt* context) {
  if (managementDisconnectAtMs_ != 0 && attrHandle != managementResponseValueHandle_)
    return BleAccessPolicy::kApplicationAuthorizationRequired;
  if (connHandle == connectionHandle_) {
    const uint32_t accessMs = millis();
    lastPeerActivityMs_ = accessMs == 0 ? 1 : accessMs;
  }
  if (attrHandle == managementRequestValueHandle_) {
    return context->op == BLE_GATT_ACCESS_OP_WRITE_CHR
               ? onManagementWrite(connHandle, context)
               : BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  if (attrHandle == managementResponseValueHandle_) {
    return context->op == BLE_GATT_ACCESS_OP_READ_CHR
               ? onManagementRead(connHandle, context)
               : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }
  if (attrHandle == pairingNicknameValueHandle_) {
    // Nicknames carry no authority. They are writable only for the pending,
    // physically approved connection, never for an existing approval. The
    // main-loop Pairing Commit persists name and key in the same transaction.
    const auto pendingReadable = [&]() {
      return BlePairingPolicy::secretReadable(pairingState_, physicalPairing_,
          pairingSecretAvailable_, connHandle == pairingConnectionHandle_,
          connectionEncrypted_, connectionBonded_, millis(), pairingUntilMs_, pairingSecretUntilMs_);
    };
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      char nickname[BleAppRegistry::kLabelBytes + 1]{};
      uint16_t length = 0;
      if (ble_hs_mbuf_to_flat(context->om, nickname, BleAppRegistry::kLabelBytes, &length) != 0 ||
          !length || memchr(nickname, 0, length) || !BleAppRegistry::validLabel(nickname))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      portENTER_CRITICAL(&controlMux_);
      // A queued commit closes all further nickname writes.
      if (!BlePairingPolicy::nicknameWritable(pendingReadable(), pairingLabelExplicit_)) {
        portEXIT_CRITICAL(&controlMux_);
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
      }
      memcpy(pairingAppLabel_, nickname, sizeof(nickname));
      portEXIT_CRITICAL(&controlMux_);
      return 0;
    }
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t value[2 + BleAppRegistry::kLabelBytes + 1]{};
    value[0] = 1; // nickname record version, independent of command protocol
    portENTER_CRITICAL(&controlMux_);
    const bool pending = pendingReadable();
    if (!pending && (!controlAuthenticated_ || !connectionEncrypted_ || connHandle != connectionHandle_ ||
        authenticatedAppSlot_ >= BleAppRegistry::kCapacity || !authenticatedAppId_ ||
        controlApps_[authenticatedAppSlot_].id != authenticatedAppId_)) {
      portEXIT_CRITICAL(&controlMux_);
      return BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
    }
    value[1] = pending ? (0x02 | (pairingLabelExplicit_ ? 0x01 : 0)) : 0;
    if (pending) memcpy(value + 2, pairingAppLabel_, sizeof(pairingAppLabel_));
    else memcpy(value + 2, controlApps_[authenticatedAppSlot_].label, sizeof(pairingAppLabel_));
    portEXIT_CRITICAL(&controlMux_);
    return os_mbuf_append(context->om, value, 2 + strlen(reinterpret_cast<char*>(value + 2))) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (attrHandle != controlCommandValueHandle_) {
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    uint8_t frame[kControlFrameSize]{};
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(context->om, frame, sizeof(frame), &copied) != 0 ||
        copied < 2 || frame[0] != BleControlProtocol::kProtocolVersion) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (frame[1] == static_cast<uint8_t>(BleControlOpcode::Authenticate)) {
      if (copied != kAuthenticationFrameSize) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
      return authenticateControl(frame, millis())
                 ? 0
                 : BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
    }
    if (frame[1] == static_cast<uint8_t>(BleControlOpcode::PairingCommit)) {
      if (copied != kPairingCommitFrameSize) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
      return queuePairingCommit(connHandle, frame)
                 ? 0
                 : BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
    }
    if (copied != kControlFrameSize) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return queueControlCommand(connHandle, frame)
               ? 0
               : !controlAuthenticated_
                     ? BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_)
                     : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  if (ble_uuid_cmp(context->chr->uuid, &kProtocolInfoUuid.u) == 0) {
    return os_mbuf_append(context->om, protocolInfo_, strlen(protocolInfo_)) ==
                   0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ble_uuid_cmp(context->chr->uuid, &kCapabilitiesUuid.u) == 0) {
    return os_mbuf_append(context->om, capabilitiesFrame_,
                          sizeof(capabilitiesFrame_)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (attrHandle == pairingSecretValueHandle_) {
    refreshConnectionSecurity();
    if (!BlePairingPolicy::secretReadable(pairingState_, physicalPairing_,
            pairingSecretAvailable_, connHandle == pairingConnectionHandle_,
            connectionEncrypted_, connectionBonded_, millis(),
            pairingUntilMs_, pairingSecretUntilMs_)) {
      return BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
    }
    uint8_t key[kControlKeySize]{};
    memcpy(key, pendingPairingKey_, sizeof(key));
    const bool appended = os_mbuf_append(context->om, key, sizeof(key)) == 0;
    memset(key, 0, sizeof(key));
    if (!appended) {
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    // The key remains pending until the app proves it received and stored it
    // with PairingCommit. Re-reading returns the same pending key, so a lost
    // ATT response cannot invalidate the previously approved controller.
    pairingState_ = PairingState::WaitingForCommit;
    return 0;
  }
  if (attrHandle == controlChallengeValueHandle_) {
    if (!connectionEncrypted_ || !connectionBonded_) {
      return BLE_ATT_ERR_INSUFFICIENT_ENC;
    }
    uint8_t frame[kAuthenticationFrameSize]{};
    portENTER_CRITICAL(&controlMux_);
    memcpy(frame, controlChallengeFrame_, sizeof(frame));
    portEXIT_CRITICAL(&controlMux_);
    return os_mbuf_append(context->om, frame, sizeof(frame)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (!connectionEncrypted_ || !controlAuthenticated_) {
    return BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
  }
  if (attrHandle == statusValueHandle_) {
    uint8_t frame[kStatusFrameSize];
    copyStatusFrame(frame);
    return os_mbuf_append(context->om, frame, sizeof(frame)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (attrHandle == systemInfoValueHandle_) {
    uint8_t frame[kSystemInfoFrameSize];
    copySystemInfoFrame(frame);
    return os_mbuf_append(context->om, frame, sizeof(frame)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (attrHandle == eventValueHandle_) {
    uint8_t frame[kEventFrameSize]{};
    portENTER_CRITICAL(&frameMux_);
    memcpy(frame, eventFrame_, sizeof(frame));
    portEXIT_CRITICAL(&frameMux_);
    return os_mbuf_append(context->om, frame, sizeof(frame)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (attrHandle == controlResultValueHandle_) {
    uint8_t frame[kControlResultFrameSize]{};
    portENTER_CRITICAL(&controlMux_);
    memcpy(frame, controlResultFrame_, sizeof(frame));
    portEXIT_CRITICAL(&controlMux_);
    return os_mbuf_append(context->om, frame, sizeof(frame)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

bool BluetoothApplication::startAdvertising() {
  if (!enabled_ || !synchronized_ || connected_ ||
      authenticationBackoffRemainingMs(millis()) != 0) {
    return false;
  }

  uint8_t ownAddressType = 0;
  int result = ble_hs_id_infer_auto(0, &ownAddressType);
  if (result != 0) {
    lastError_ = result;
    return false;
  }

  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid);
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  result = ble_gap_adv_set_fields(&fields);
  if (result != 0) {
    lastError_ = result;
    return false;
  }

  ble_hs_adv_fields response{};
  response.name = reinterpret_cast<uint8_t*>(advertisedName_);
  response.name_len = strlen(advertisedName_);
  response.name_is_complete = 1;
  result = ble_gap_adv_rsp_set_fields(&response);
  if (result != 0) {
    lastError_ = result;
    return false;
  }

  ble_gap_adv_params parameters{};
  parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
  parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
  result = ble_gap_adv_start(ownAddressType, nullptr, BLE_HS_FOREVER,
                             &parameters, gapEvent, nullptr);
  if (result != 0) {
    lastError_ = result;
    return false;
  }
  advertising_ = true;
  lastError_ = 0;
  return true;
}

void BluetoothApplication::updateStatusFrame(uint32_t nowMs) {
  const TimerSnapshot snapshot = timer_.snapshot(nowMs);
  uint8_t frame[kStatusFrameSize]{};
  frame[0] = BleControlProtocol::kProtocolVersion;
  frame[1] = stateCode(snapshot.state);
  if (snapshot.sealed) {
    frame[2] |= 0x01;
  }
  if (snapshot.emergencyInputActive) {
    frame[2] |= 0x02;
  }
  if (snapshot.state != TimerState::Open) {
    frame[2] |= 0x04;
  }
  // Set when the owner switched the control off; clear means allowed, which
  // is also what an app written before these bits existed assumes.
  if (!snapshot.pauseAllowed) {
    frame[2] |= 0x08;
  }
  if (!snapshot.sealAllowed) {
    frame[2] |= 0x10;
  }
  frame[3] = releaseReasonCode(snapshot.lastReleaseReason);
  writeUint32Le(frame + 4, snapshot.remainingMs);
  writeUint32Le(frame + 8, snapshot.sessionDurationMs);
  writeUint32Le(frame + 12, timer_.eventRevision());
  writeUint32Le(frame + 16, timer_.sessionSequence());

  portENTER_CRITICAL(&frameMux_);
  memcpy(statusFrame_, frame, sizeof(frame));
  portEXIT_CRITICAL(&frameMux_);
}

void BluetoothApplication::updateSystemInfoFrame(uint32_t nowMs) {
  uint8_t frame[kSystemInfoFrameSize]{};
  frame[0] = BleControlProtocol::kProtocolVersion;
  frame[1] = static_cast<uint8_t>(esp_reset_reason());
  const String wifiState = network_.stationState(nowMs);
  frame[2] = wifiState == "disabled" ? 5
             : wifiState == "standalone" ? 0
             : wifiState == "connecting" ? 1
             : wifiState == "connected" ? 2
             : wifiState == "failed" ? 3
             : 4;
  if (network_.stationConnected()) frame[3] |= 0x01;
  if (network_.accessPointRunning()) frame[3] |= 0x02;
  if (network_.standaloneMode()) frame[3] |= 0x04;
  if (network_.stationRetryPaused()) frame[3] |= 0x08;
  if (connected_) frame[3] |= 0x10;
  if (controlAuthenticated_) frame[3] |= 0x20;
  writeUint32Le(frame + 4, nowMs);
  writeUint32Le(frame + 8, Platform::freeHeapBytes());
  writeUint32Le(frame + 12, Platform::largestFreeBlockBytes());
  writeUint32Le(frame + 16, Platform::minimumFreeHeapBytes());
  writeUint32Le(frame + 20, Platform::sketchSizeBytes());
  writeUint32Le(frame + 24, Platform::freeSketchSpaceBytes());
  writeUint32Le(frame + 28, Platform::flashSizeBytes());
  if (network_.stationConnected()) {
    const IPAddress address = WiFi.localIP();
    for (uint8_t index = 0; index < 4; ++index) frame[32 + index] = address[index];
  }
  frame[36] = static_cast<uint8_t>(network_.stationRssi());
  frame[37] = network_.accessPointClients();
  frame[38] = network_.accessPointClientLimit();
  frame[39] = Platform::heapFragmentationPercent();
  frame[40] = static_cast<uint8_t>(connectionMtu_);
  frame[41] = static_cast<uint8_t>(connectionMtu_ >> 8);
  frame[42] = static_cast<uint8_t>(connectionRssi_);

  portENTER_CRITICAL(&frameMux_);
  memcpy(systemInfoFrame_, frame, sizeof(frame));
  portEXIT_CRITICAL(&frameMux_);
}

bool BluetoothApplication::sendStatusNotification(uint32_t nowMs,
                                                   bool force) {
  if (!connected_ || !connectionEncrypted_ || !controlAuthenticated_ ||
      !subscribed_ || statusValueHandle_ == 0 ||
      connectionHandle_ == BLE_HS_CONN_HANDLE_NONE) {
    return false;
  }
  if (!force && lastNotificationAtMs_ != 0 &&
      nowMs - lastNotificationAtMs_ < kFrameRefreshMs) {
    return false;
  }

  uint8_t frame[kStatusFrameSize];
  copyStatusFrame(frame);
  os_mbuf* packet = ble_hs_mbuf_from_flat(frame, sizeof(frame));
  if (packet == nullptr) {
    lastError_ = BLE_HS_ENOMEM;
    return false;
  }
  const int result = ble_gatts_notify_custom(connectionHandle_,
                                              statusValueHandle_, packet);
  if (result != 0) {
    if (result != BLE_HS_ENOTCONN) {
      lastError_ = result;
    }
    return false;
  }
  lastNotificationAtMs_ = nowMs == 0 ? 1 : nowMs;
  notificationCount_ = notificationCount_ + 1;
  return true;
}

void BluetoothApplication::copyStatusFrame(uint8_t* destination) {
  portENTER_CRITICAL(&frameMux_);
  memcpy(destination, statusFrame_, sizeof(statusFrame_));
  portEXIT_CRITICAL(&frameMux_);
}

void BluetoothApplication::copySystemInfoFrame(uint8_t* destination) {
  portENTER_CRITICAL(&frameMux_);
  memcpy(destination, systemInfoFrame_, sizeof(systemInfoFrame_));
  portEXIT_CRITICAL(&frameMux_);
}

void BluetoothApplication::writeUint32Le(uint8_t* destination,
                                         uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

#else

BluetoothApplication::BluetoothApplication(TimerController& timer,
                                           DeviceConfig& config,
                                           LocalNetwork& network)
    : timer_(timer), config_(config), network_(network) {}
bool BluetoothApplication::begin(uint32_t) { return false; }
void BluetoothApplication::loop(uint32_t, bool) {}
BluetoothApplication* BluetoothApplication::active() { return nullptr; }
bool BluetoothApplication::initialized() const { return false; }
bool BluetoothApplication::enabled() const { return false; }
bool BluetoothApplication::configuredEnabled() const { return false; }
bool BluetoothApplication::setRadioEnabled(bool, uint32_t) { return false; }
bool BluetoothApplication::disable() { return true; }
bool BluetoothApplication::enableControl() { return false; }
void BluetoothApplication::disableControl() {}
bool BluetoothApplication::startWebPairing(uint32_t, uint32_t) { return false; }
bool BluetoothApplication::startPhysicalPairing(uint32_t, uint32_t) {
  return false;
}
bool BluetoothApplication::confirmPhysicalPairing(uint32_t) { return false; }
void BluetoothApplication::cancelPairing() {}
bool BluetoothApplication::revokePairing() { return false; }
bool BluetoothApplication::clearBondsForFactoryReset(uint32_t) { return false; }
bool BluetoothApplication::paired() const { return false; }
bool BluetoothApplication::pairingActive(uint32_t) const { return false; }
bool BluetoothApplication::pairingAwaitingConfirmation(uint32_t) const {
  return false;
}
uint32_t BluetoothApplication::pairingRemainingMs(uint32_t) const { return 0; }
const char* BluetoothApplication::pairingStateName(uint32_t) const {
  return "unsupported";
}
const char* BluetoothApplication::pairingFailureName() const {
  return "unsupported";
}
bool BluetoothApplication::pairingLedOn(uint32_t) const { return false; }
bool BluetoothApplication::connectionEncrypted() const { return false; }
bool BluetoothApplication::connectionBonded() const { return false; }
bool BluetoothApplication::identifying(uint32_t) const { return false; }
bool BluetoothApplication::identifyLedOn(uint32_t) const { return false; }
bool BluetoothApplication::controlEnabled() const { return false; }
bool BluetoothApplication::controlAuthenticated() const { return false; }
uint32_t BluetoothApplication::controlAuthenticationFailureCount() const {
  return 0;
}
uint32_t BluetoothApplication::authenticationBackoffRemainingMs(
    uint32_t) const {
  return 0;
}
uint32_t BluetoothApplication::controlCommandCount() const { return 0; }
const char* BluetoothApplication::lastControlResultName() const {
  return "unsupported";
}
bool BluetoothApplication::synchronized() const { return false; }
bool BluetoothApplication::advertising() const { return false; }
void BluetoothApplication::pauseAdvertising(bool) {}
bool BluetoothApplication::connected() const { return false; }
bool BluetoothApplication::subscribed() const { return false; }
uint8_t BluetoothApplication::connectionCount() const { return 0; }
uint8_t BluetoothApplication::connectionLimit() const { return 0; }
uint32_t BluetoothApplication::acceptedCount() const { return 0; }
uint32_t BluetoothApplication::closedCount() const { return 0; }
uint32_t BluetoothApplication::notificationCount() const { return 0; }
int32_t BluetoothApplication::lastError() const { return 0; }
const char* BluetoothApplication::lastErrorName() const { return "none"; }
int32_t BluetoothApplication::lastDisconnectReason() const { return 0; }
const char* BluetoothApplication::lastDisconnectReasonName() const {
  return "unsupported";
}
int8_t BluetoothApplication::connectionRssi() const { return 127; }
uint16_t BluetoothApplication::connectionMtu() const { return 0; }
bool BluetoothApplication::connectionMtuSufficient() const { return false; }
bool BluetoothApplication::bondCleanupPending() const { return false; }
int32_t BluetoothApplication::bondCleanupLastError() const { return 0; }
const char* BluetoothApplication::advertisedName() const { return ""; }
const char* BluetoothApplication::protocolName() const { return "unsupported"; }

#endif
