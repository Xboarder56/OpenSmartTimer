#pragma once

#include <Arduino.h>
#if defined(SMART_TIMER_HARDWARE_V2)
#include "BleAppRegistry.h"
#endif

class DeviceConfig {
 public:
#if defined(SMART_TIMER_HARDWARE_V2)
  bool begin();
#else
  void begin();
#endif

  bool hasWifi() const;
  const char* wifiSsid() const;
  const char* wifiPassword() const;
  const char* deviceName() const;
  String hostName() const;
  bool accessPointUsesDeviceName() const;
  const char* accessPointSsid() const;
  const char* accessPointPassword() const;
  bool accessPointSecured() const;

  bool setWifi(const String& ssid, const String& password);
  bool clearWifi();
  bool lastSaveFailed() const { return lastSaveFailed_; }
  bool setDeviceName(const String& name);
  bool setAccessPoint(const String& ssid, const String& password,
                      bool useDeviceName, bool keepExistingPassword,
                      bool clearStation = false);
#if defined(SMART_TIMER_HARDWARE_V2)
  bool bluetoothEnabled() const;
  bool bluetoothPaired() const;
  bool copyBluetoothKey(uint8_t* destination, size_t length) const;
  bool setBluetoothEnabled(bool enabled);
  bool saveBluetoothPairing(const uint8_t* key, size_t length);
  bool clearBluetoothPairing();
  const BleAppRegistry& bluetoothApps() const { return bluetoothApps_; }
  bool saveBluetoothApp(int slot, const uint8_t* key, const char* label,
                        uint8_t peerType, const uint8_t* peerAddress);
  bool removeBluetoothApp(uint32_t id);
  bool renameBluetoothApp(uint32_t id, const String& label);
  bool bindBluetoothAppPeer(uint32_t id, uint8_t type, const uint8_t* address);
#endif
  // Owner switches: may a session be paused, and may its controls be locked.
  // Both are allowed until the owner saves otherwise.
  bool pauseAllowed() const { return controls_.allowPause != 0; }
  bool controlLockAllowed() const { return controls_.allowControlLock != 0; }
  bool setSessionControls(bool allowPause, bool allowControlLock);
  // True when a saved switch record could not be read back (or V2 storage
  // failed): both switches are then off until the owner saves them again.
  bool sessionControlsRecovered() const { return controlsRecovered_; }
  bool factoryReset();

 private:
  static constexpr uint32_t kMagic = 0x53544D52UL;
  static constexpr uint16_t kVersion = 2;

  struct __attribute__((packed)) StoredConfig {
    uint32_t magic;
    uint16_t version;
    char wifiSsid[33];
    char wifiPassword[65];
    uint32_t checksum;
  };

  // Kept in a separate EEPROM record so firmware upgrades never invalidate or
  // erase the already-provisioned Wi-Fi record above.
  static constexpr uint32_t kIdentityMagic = 0x53544944UL;
  static constexpr uint16_t kIdentityVersion = 1;
  static constexpr size_t kIdentityOffset = 128;

  struct __attribute__((packed)) StoredIdentity {
    uint32_t magic;
    uint16_t version;
    char deviceName[33];
    uint32_t checksum;
  };

  // AP settings are isolated from both the station credentials and identity
  // records. Missing records migrate safely to a generated, open recovery AP.
  static constexpr uint32_t kAccessPointMagic = 0x53544150UL;
  static constexpr uint16_t kAccessPointVersion = 1;
  static constexpr size_t kAccessPointOffset = 192;

  struct __attribute__((packed)) StoredAccessPoint {
    uint32_t magic;
    uint16_t version;
    uint8_t useDeviceName;
    char ssid[33];
    char password[64];
    uint32_t checksum;
  };

  // Session-control switches have their own record in the gap between the
  // access-point record and the V2 Bluetooth record, so no existing record
  // moves on either model. A timer without a valid record, which is every
  // earlier build, allows both controls as before.
  static constexpr uint32_t kControlsMagic = 0x53544354UL;
  static constexpr uint16_t kControlsVersion = 1;
  static constexpr size_t kControlsOffset = 304;

  struct __attribute__((packed)) StoredControls {
    uint32_t magic;
    uint16_t version;
    uint8_t allowPause;
    uint8_t allowControlLock;
    uint32_t checksum;
  };

#if defined(SMART_TIMER_HARDWARE_V2)
  // V2 BLE settings are deliberately outside every V1 record. The ESP8266
  // never reads, writes, or reserves this offset, preserving its tested
  // EEPROM-emulation layout byte for byte.
  static constexpr uint32_t kBluetoothMagic = 0x5354424CUL;
  static constexpr uint16_t kBluetoothVersion = 1;
  static constexpr size_t kBluetoothOffset = 320;
  static constexpr size_t kBluetoothKeySize = 16;

  struct __attribute__((packed)) StoredBluetooth {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t paired;
    uint8_t key[kBluetoothKeySize];
    uint32_t checksum;
  };
#endif

  void createDefaults();
  bool save();
  void createIdentityDefaults();
  bool saveIdentity();
  void createAccessPointDefaults();
  bool saveAccessPoint();
  void createControlsDefaults();
  bool saveControls();
  bool commitStorage();
#if defined(SMART_TIMER_HARDWARE_V2)
  void createBluetoothDefaults();
  bool saveBluetooth();
  bool loadBluetoothApps();
  bool saveBluetoothApps(BleAppRegistry next);
#endif
  static bool printableAscii(const String& value);
  static uint32_t checksum(const uint8_t* bytes, size_t length);

  StoredConfig data_{};
  StoredIdentity identity_{};
  StoredAccessPoint accessPoint_{};
  StoredControls controls_{kControlsMagic, kControlsVersion, 1, 1, 0};
  bool controlsRecovered_ = false;
  void recoverControlsOff();
  bool lastSaveFailed_ = false;
#if defined(SMART_TIMER_HARDWARE_V2)
  StoredBluetooth bluetooth_{};
  BleAppRegistry bluetoothApps_{};
  bool storageHealthy_ = false;
#endif
};
