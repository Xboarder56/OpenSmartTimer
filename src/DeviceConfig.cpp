#include "DeviceConfig.h"

#include <EEPROM.h>
#if defined(SMART_TIMER_HARDWARE_V2)
#include <nvs.h>
#endif

#include <cstring>

namespace {
constexpr size_t kEepromSize = 512;

// ESP32's EEPROM compatibility layer stores a blob in a named NVS namespace.
// Keep OpenSmartTimer isolated from vendor/future application settings. ESP8266
// has no named namespace; its default object still maps to the same fixed
// EEPROM sector used by existing V1 OpenSmartTimer releases.
#if defined(ESP32)
EEPROMClass openSmartTimerStorage("opensmarttimer");
#else
EEPROMClass openSmartTimerStorage;
#endif
}

#if defined(SMART_TIMER_HARDWARE_V2)
bool DeviceConfig::begin() {
#else
void DeviceConfig::begin() {
#endif
  static_assert(kAccessPointOffset + sizeof(StoredAccessPoint) <= kControlsOffset,
                "EEPROM records overlap");
  static_assert(kControlsOffset + sizeof(StoredControls) <= kEepromSize,
                "EEPROM records exceed allocated storage");
#if defined(SMART_TIMER_HARDWARE_V2)
  static_assert(kBluetoothOffset + sizeof(StoredBluetooth) <= kEepromSize,
                "V2 Bluetooth record exceeds allocated storage");
#endif
#if defined(SMART_TIMER_HARDWARE_V2)
  storageHealthy_ = false;
  if (!openSmartTimerStorage.begin(kEepromSize)) {
    // Keep enough in-memory defaults to offer a recovery AP, but report the
    // storage failure so a newly installed V2 OTA image is never confirmed.
    createDefaults();
    createIdentityDefaults();
    createAccessPointDefaults();
    recoverControlsOff();  // the owner's saved choice is unknown: fewer holds
    return false;
  }
  storageHealthy_ = true;
#else
  // Preserve the already field-tested V1 ESP8266 storage path exactly.
  openSmartTimerStorage.begin(kEepromSize);
#endif
  openSmartTimerStorage.get(0, data_);
  const uint32_t storedChecksum = data_.checksum;
  const uint32_t actualChecksum =
      checksum(reinterpret_cast<const uint8_t*>(&data_),
               sizeof(data_) - sizeof(data_.checksum));
  if (data_.magic != kMagic || data_.version != kVersion ||
      storedChecksum != actualChecksum) {
    createDefaults();
    save();
  }

  openSmartTimerStorage.get(kIdentityOffset, identity_);
  const uint32_t identityStoredChecksum = identity_.checksum;
  const uint32_t identityActualChecksum =
      checksum(reinterpret_cast<const uint8_t*>(&identity_),
               sizeof(identity_) - sizeof(identity_.checksum));
  if (identity_.magic != kIdentityMagic ||
      identity_.version != kIdentityVersion ||
      identity_.deviceName[sizeof(identity_.deviceName) - 1] != '\0' ||
      identityStoredChecksum != identityActualChecksum) {
    createIdentityDefaults();
    saveIdentity();
  }

  openSmartTimerStorage.get(kAccessPointOffset, accessPoint_);
  const uint32_t accessPointStoredChecksum = accessPoint_.checksum;
  const uint32_t accessPointActualChecksum =
      checksum(reinterpret_cast<const uint8_t*>(&accessPoint_),
               sizeof(accessPoint_) - sizeof(accessPoint_.checksum));
  if (accessPoint_.magic != kAccessPointMagic ||
      accessPoint_.version != kAccessPointVersion ||
      accessPoint_.useDeviceName > 1 ||
      accessPoint_.ssid[sizeof(accessPoint_.ssid) - 1] != '\0' ||
      accessPoint_.password[sizeof(accessPoint_.password) - 1] != '\0' ||
      accessPointStoredChecksum != accessPointActualChecksum) {
    createAccessPointDefaults();
    saveAccessPoint();
  }
  openSmartTimerStorage.get(kControlsOffset, controls_);
  controlsRecovered_ = false;
  if (controls_.magic != kControlsMagic) {
    // Never saved (first boot or an upgrade): keep the long-standing
    // behaviour. Nothing is written until the owner changes a switch.
    createControlsDefaults();
  } else if (controls_.version != kControlsVersion ||
             controls_.allowPause > 1 || controls_.allowControlLock > 1 ||
             controls_.checksum != checksum(reinterpret_cast<const uint8_t*>(&controls_),
                                            sizeof(controls_) - sizeof(controls_.checksum))) {
    // A saved choice that no longer reads back: turn both off (pause can hold
    // the lock indefinitely; locking blocks software release) and report it.
    recoverControlsOff();
  }
#if defined(SMART_TIMER_HARDWARE_V2)
  static_assert(kControlsOffset + sizeof(StoredControls) <= kBluetoothOffset,
                "EEPROM records overlap");
  openSmartTimerStorage.get(kBluetoothOffset, bluetooth_);
  const uint32_t bluetoothStoredChecksum = bluetooth_.checksum;
  const uint32_t bluetoothActualChecksum =
      checksum(reinterpret_cast<const uint8_t*>(&bluetooth_),
               sizeof(bluetooth_) - sizeof(bluetooth_.checksum));
  if (bluetooth_.magic != kBluetoothMagic ||
      bluetooth_.version != kBluetoothVersion || bluetooth_.enabled > 1 ||
      bluetooth_.paired > 1 ||
      bluetoothStoredChecksum != bluetoothActualChecksum) {
    createBluetoothDefaults();
    saveBluetooth();
  }
  storageHealthy_ = loadBluetoothApps() && storageHealthy_;
  return storageHealthy_;
#endif
}

bool DeviceConfig::hasWifi() const { return data_.wifiSsid[0] != '\0'; }

const char* DeviceConfig::wifiSsid() const { return data_.wifiSsid; }

const char* DeviceConfig::wifiPassword() const { return data_.wifiPassword; }

const char* DeviceConfig::deviceName() const { return identity_.deviceName; }

bool DeviceConfig::accessPointUsesDeviceName() const {
  return accessPoint_.useDeviceName != 0;
}

const char* DeviceConfig::accessPointSsid() const {
  return accessPoint_.ssid;
}

const char* DeviceConfig::accessPointPassword() const {
  return accessPoint_.password;
}

bool DeviceConfig::accessPointSecured() const {
  return accessPoint_.password[0] != '\0';
}

String DeviceConfig::hostName() const {
  String hostname;
  bool previousWasSeparator = false;
  for (size_t index = 0; identity_.deviceName[index] != '\0'; ++index) {
    char character = identity_.deviceName[index];
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
    const bool alphaNumeric =
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9');
    if (alphaNumeric) {
      hostname += character;
      previousWasSeparator = false;
    } else if (!previousWasSeparator && hostname.length() != 0) {
      hostname += '-';
      previousWasSeparator = true;
    }
    if (hostname.length() >= 24) {
      break;
    }
  }
  while (hostname.endsWith("-")) {
    hostname.remove(hostname.length() - 1);
  }
  if (hostname == "smart-timer") {
    hostname = "smarttimer";
  }
  return hostname.length() == 0 ? String("smarttimer") : hostname;
}

bool DeviceConfig::setWifi(const String& ssid, const String& password) {
  const StoredConfig previous = data_;
  ssid.substring(0, sizeof(data_.wifiSsid) - 1).toCharArray(
      data_.wifiSsid, sizeof(data_.wifiSsid));
  password.substring(0, sizeof(data_.wifiPassword) - 1).toCharArray(
      data_.wifiPassword, sizeof(data_.wifiPassword));
  if (save()) return true;
  data_ = previous;
  openSmartTimerStorage.put(0, data_);
  return false;
}

bool DeviceConfig::clearWifi() {
  return setWifi("", "");
}

bool DeviceConfig::setDeviceName(const String& name) {
  lastSaveFailed_ = false;
  String normalized = name;
  normalized.trim();
  if (normalized.length() == 0 || normalized.length() > 32) {
    return false;
  }
  for (size_t index = 0; index < normalized.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(normalized[index]);
    if (character < 0x20 || character > 0x7E) {
      return false;
    }
  }
  const StoredIdentity previous = identity_;
  normalized.toCharArray(identity_.deviceName, sizeof(identity_.deviceName));
  if (saveIdentity()) return true;
  identity_ = previous;
  openSmartTimerStorage.put(kIdentityOffset, identity_);
  return false;
}

bool DeviceConfig::setAccessPoint(const String& ssid, const String& password,
                                  bool useDeviceName,
                                  bool keepExistingPassword, bool clearStation) {
  lastSaveFailed_ = false;
  String normalizedSsid = ssid;
  normalizedSsid.trim();
  const String effectivePassword =
      keepExistingPassword && accessPointSecured()
          ? String(accessPoint_.password)
          : password;
  if ((!useDeviceName &&
       (normalizedSsid.length() == 0 || normalizedSsid.length() > 32 ||
        !printableAscii(normalizedSsid))) ||
      (effectivePassword.length() != 0 &&
       (effectivePassword.length() < 8 || effectivePassword.length() > 63 ||
        !printableAscii(effectivePassword)))) {
    return false;
  }

  const StoredAccessPoint previous = accessPoint_;
  const StoredConfig previousStation = data_;
  accessPoint_.useDeviceName = useDeviceName ? 1 : 0;
  if (useDeviceName) {
    accessPoint_.ssid[0] = '\0';
  } else {
    normalizedSsid.toCharArray(accessPoint_.ssid,
                               sizeof(accessPoint_.ssid));
  }
  effectivePassword.toCharArray(accessPoint_.password,
                                sizeof(accessPoint_.password));
  // AP settings and switching to standalone share one EEPROM blob commit.
  if (clearStation) {
    memset(data_.wifiSsid, 0, sizeof(data_.wifiSsid));
    memset(data_.wifiPassword, 0, sizeof(data_.wifiPassword));
    data_.checksum = checksum(reinterpret_cast<const uint8_t*>(&data_),
                              sizeof(data_) - sizeof(data_.checksum));
    openSmartTimerStorage.put(0, data_);
  }
  if (saveAccessPoint()) return true;
  accessPoint_ = previous;
  data_ = previousStation;
  openSmartTimerStorage.put(kAccessPointOffset, accessPoint_);
  openSmartTimerStorage.put(0, data_);
  return false;
}

#if defined(SMART_TIMER_HARDWARE_V2)
bool DeviceConfig::bluetoothEnabled() const {
  return bluetooth_.enabled != 0;
}

bool DeviceConfig::bluetoothPaired() const {
  return bluetoothApps_.count() != 0;
}

bool DeviceConfig::copyBluetoothKey(uint8_t* destination,
                                    size_t length) const {
  if (destination == nullptr || length != sizeof(bluetooth_.key) ||
      !bluetoothPaired()) {
    return false;
  }
  for (const auto& app : bluetoothApps_.apps) if (app.id) {
    std::memcpy(destination, app.key, 16);
    return true;
  }
  return false;
}

bool DeviceConfig::setBluetoothEnabled(bool enabled) {
  if (bluetoothEnabled() == enabled) return true;
  const StoredBluetooth previous = bluetooth_;
  bluetooth_.enabled = enabled ? 1 : 0;
  if (saveBluetooth()) {
    return true;
  }
  bluetooth_ = previous;
  openSmartTimerStorage.put(kBluetoothOffset, bluetooth_);
  return false;
}

bool DeviceConfig::saveBluetoothPairing(const uint8_t* key, size_t length) {
  return length == 16 && saveBluetoothApp(bluetoothApps_.empty(), key,
                                          "Approved app", 0, nullptr);
}

bool DeviceConfig::clearBluetoothPairing() {
  BleAppRegistry next = bluetoothApps_;
  for (auto& app : next.apps) app = {};
  return saveBluetoothApps(next);
}

bool DeviceConfig::saveBluetoothApp(int slot, const uint8_t* key,
                                    const char* label, uint8_t peerType,
                                    const uint8_t* peerAddress) {
  BleAppRegistry next = bluetoothApps_;
  return next.put(slot, key, label, peerType, peerAddress) && saveBluetoothApps(next);
}

bool DeviceConfig::removeBluetoothApp(uint32_t id) {
  BleAppRegistry next = bluetoothApps_;
  const int slot = next.find(id);
  if (slot < 0) return false;
  next.apps[slot] = {};
  return saveBluetoothApps(next);
}

bool DeviceConfig::renameBluetoothApp(uint32_t id, const String& label) {
  const int slot = bluetoothApps_.find(id);
  if (slot < 0 || !BleAppRegistry::validLabel(label.c_str())) return false;
  BleAppRegistry next = bluetoothApps_;
  memset(next.apps[slot].label, 0, sizeof(next.apps[slot].label));
  memcpy(next.apps[slot].label, label.c_str(), label.length());
  return saveBluetoothApps(next);
}

bool DeviceConfig::bindBluetoothAppPeer(uint32_t id, uint8_t type,
                                       const uint8_t* address) {
  const int slot = bluetoothApps_.find(id);
  if (slot < 0 || !address) return false;
  BleAppRegistry next = bluetoothApps_;
  next.apps[slot].peerKnown = 1;
  next.apps[slot].peerType = type;
  memcpy(next.apps[slot].peerAddress, address, 6);
  return saveBluetoothApps(next);
}

bool DeviceConfig::saveBluetoothApps(BleAppRegistry next) {
  if (!next.valid()) return false;
  next.checksum = checksum(reinterpret_cast<const uint8_t*>(&next),
                           offsetof(BleAppRegistry, checksum));
  nvs_handle_t handle;
  if (nvs_open("opensmarttimer", NVS_READWRITE, &handle) != ESP_OK) return false;
  // NVS blob replacement is atomic. Publish RAM only after durable commit.
  esp_err_t result = nvs_set_blob(handle, "ble_apps", &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return false;
  bluetoothApps_ = next;
  return true;
}

bool DeviceConfig::loadBluetoothApps() {
  nvs_handle_t handle;
  if (nvs_open("opensmarttimer", NVS_READWRITE, &handle) != ESP_OK) return false;
  BleAppRegistry stored{};
  size_t size = sizeof(stored);
  const esp_err_t result = nvs_get_blob(handle, "ble_apps", &stored, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    if (bluetooth_.paired && !stored.put(0, bluetooth_.key, "Existing app", 0, nullptr)) return false;
    if (!saveBluetoothApps(stored)) return false;
  } else if (result != ESP_OK || size != sizeof(stored) || !stored.valid() ||
             stored.checksum != checksum(reinterpret_cast<const uint8_t*>(&stored),
                                          offsetof(BleAppRegistry, checksum))) {
    // Never silently fall back to an old, potentially revoked legacy key.
    return false;
  } else {
    bluetoothApps_ = stored;
  }
  if (bluetoothApps_.resetPending) {
    factoryReset();
    if (bluetoothApps_.resetPending) return false;
  }
  // Retire the old credential only after the registry is authoritative. Old
  // firmware can still offer Wi-Fi/AP, but a BLE downgrade requires pairing.
  if (bluetooth_.paired) {
    memset(bluetooth_.key, 0, sizeof(bluetooth_.key));
    bluetooth_.paired = 0;
    if (!saveBluetooth()) return false;
  }
  return true;
}
#endif

void DeviceConfig::recoverControlsOff() {
  createControlsDefaults();
  controls_.allowPause = 0;
  controls_.allowControlLock = 0;
  controlsRecovered_ = true;
}

bool DeviceConfig::setSessionControls(bool allowPause, bool allowControlLock) {
  // After a recovery, always write so the damaged record is replaced.
  if (!controlsRecovered_ && pauseAllowed() == allowPause &&
      controlLockAllowed() == allowControlLock) {
    return true;
  }
  const StoredControls previous = controls_;
  controls_.allowPause = allowPause ? 1 : 0;
  controls_.allowControlLock = allowControlLock ? 1 : 0;
  if (saveControls()) {
    controlsRecovered_ = false;
    return true;
  }
  controls_ = previous;
  openSmartTimerStorage.put(kControlsOffset, controls_);
  return false;
}

bool DeviceConfig::factoryReset() {
#if defined(SMART_TIMER_HARDWARE_V2)
  BleAppRegistry resetting{};
  resetting.resetPending = 1;
  // Journal reset intent before either store changes. A boot interrupted
  // between the NVS and EEPROM commits completes the reset, not half of it.
  if (!saveBluetoothApps(resetting)) { storageHealthy_ = false; lastSaveFailed_ = true; return false; }
#endif
  const StoredConfig previousData = data_;
  const StoredIdentity previousIdentity = identity_;
  const StoredAccessPoint previousAp = accessPoint_;
  const StoredControls previousControls = controls_;
#if defined(SMART_TIMER_HARDWARE_V2)
  const StoredBluetooth previousBluetooth = bluetooth_;
#endif
  // Prepare every record before the single commit so a physical factory reset
  // does not leave a mixture of old and default settings between commits.
  createDefaults();
  createIdentityDefaults();
  createAccessPointDefaults();
  createControlsDefaults();
#if defined(SMART_TIMER_HARDWARE_V2)
  createBluetoothDefaults();
#endif

  controls_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&controls_),
               sizeof(controls_) - sizeof(controls_.checksum));
  openSmartTimerStorage.put(kControlsOffset, controls_);
  data_.checksum = checksum(reinterpret_cast<const uint8_t*>(&data_),
                            sizeof(data_) - sizeof(data_.checksum));
  identity_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&identity_),
               sizeof(identity_) - sizeof(identity_.checksum));
  accessPoint_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&accessPoint_),
               sizeof(accessPoint_) - sizeof(accessPoint_.checksum));
  openSmartTimerStorage.put(0, data_);
  openSmartTimerStorage.put(kIdentityOffset, identity_);
  openSmartTimerStorage.put(kAccessPointOffset, accessPoint_);
#if defined(SMART_TIMER_HARDWARE_V2)
  bluetooth_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&bluetooth_),
               sizeof(bluetooth_) - sizeof(bluetooth_.checksum));
  openSmartTimerStorage.put(kBluetoothOffset, bluetooth_);
#endif
  if (!commitStorage()) {
    data_ = previousData;
    identity_ = previousIdentity;
    accessPoint_ = previousAp;
    controls_ = previousControls;
    openSmartTimerStorage.put(kControlsOffset, controls_);
    openSmartTimerStorage.put(0, data_);
    openSmartTimerStorage.put(kIdentityOffset, identity_);
    openSmartTimerStorage.put(kAccessPointOffset, accessPoint_);
#if defined(SMART_TIMER_HARDWARE_V2)
    bluetooth_ = previousBluetooth;
    openSmartTimerStorage.put(kBluetoothOffset, bluetooth_);
#endif
    return false;
  }
#if defined(SMART_TIMER_HARDWARE_V2)
  resetting.resetPending = 0;
  if (!saveBluetoothApps(resetting)) { storageHealthy_ = false; lastSaveFailed_ = true; return false; }
#endif
  return true;
}

void DeviceConfig::createDefaults() {
  std::memset(&data_, 0, sizeof(data_));
  data_.magic = kMagic;
  data_.version = kVersion;
}

void DeviceConfig::createIdentityDefaults() {
  std::memset(&identity_, 0, sizeof(identity_));
  identity_.magic = kIdentityMagic;
  identity_.version = kIdentityVersion;
  String("Smart Timer").toCharArray(identity_.deviceName,
                                     sizeof(identity_.deviceName));
}

void DeviceConfig::createAccessPointDefaults() {
  std::memset(&accessPoint_, 0, sizeof(accessPoint_));
  accessPoint_.magic = kAccessPointMagic;
  accessPoint_.version = kAccessPointVersion;
  accessPoint_.useDeviceName = 1;
}

#if defined(SMART_TIMER_HARDWARE_V2)
void DeviceConfig::createBluetoothDefaults() {
  std::memset(&bluetooth_, 0, sizeof(bluetooth_));
  bluetooth_.magic = kBluetoothMagic;
  bluetooth_.version = kBluetoothVersion;
}
#endif

void DeviceConfig::createControlsDefaults() {
  controls_ = StoredControls{kControlsMagic, kControlsVersion, 1, 1, 0};
}

bool DeviceConfig::saveControls() {
  controls_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&controls_),
               sizeof(controls_) - sizeof(controls_.checksum));
  openSmartTimerStorage.put(kControlsOffset, controls_);
  return commitStorage();
}

bool DeviceConfig::commitStorage() {
  const bool committed = openSmartTimerStorage.commit();
  lastSaveFailed_ = !committed;
#if defined(SMART_TIMER_HARDWARE_V2)
  storageHealthy_ = committed && storageHealthy_;
#endif
  return committed;
}

bool DeviceConfig::save() {
  data_.checksum = checksum(reinterpret_cast<const uint8_t*>(&data_),
                            sizeof(data_) - sizeof(data_.checksum));
  openSmartTimerStorage.put(0, data_);
  return commitStorage();
}

bool DeviceConfig::saveIdentity() {
  identity_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&identity_),
               sizeof(identity_) - sizeof(identity_.checksum));
  openSmartTimerStorage.put(kIdentityOffset, identity_);
  return commitStorage();
}

bool DeviceConfig::saveAccessPoint() {
  accessPoint_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&accessPoint_),
               sizeof(accessPoint_) - sizeof(accessPoint_.checksum));
  openSmartTimerStorage.put(kAccessPointOffset, accessPoint_);
  return commitStorage();
}

#if defined(SMART_TIMER_HARDWARE_V2)
bool DeviceConfig::saveBluetooth() {
  bluetooth_.checksum =
      checksum(reinterpret_cast<const uint8_t*>(&bluetooth_),
               sizeof(bluetooth_) - sizeof(bluetooth_.checksum));
  openSmartTimerStorage.put(kBluetoothOffset, bluetooth_);
  return commitStorage();
}
#endif

bool DeviceConfig::printableAscii(const String& value) {
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if (character < 0x20 || character > 0x7E) {
      return false;
    }
  }
  return true;
}

uint32_t DeviceConfig::checksum(const uint8_t* bytes, size_t length) {
  uint32_t value = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    value ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^ (0xEDB88320UL & -(value & 1UL));
    }
  }
  return ~value;
}
