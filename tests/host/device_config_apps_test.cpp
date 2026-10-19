#include "DeviceConfig.h"
#include <EEPROM.h>
#include <nvs.h>
#include <iostream>

static uint32_t crc(const uint8_t* bytes, size_t count) {
  uint32_t value = 0xffffffff;
  while (count--) {
    value ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
  }
  return ~value;
}

int main() {
  DeviceConfig config;
  assert(config.begin());
  config.setWifi("Preserved WiFi", "test-password");
  assert(config.setDeviceName("Bench timer"));
  assert(config.setAccessPoint("Bench AP", "ap-password", false, false));
  assert(config.setBluetoothEnabled(true));
  // Simulate the prior firmware's valid one-key record, without a registry.
  struct __attribute__((packed)) Legacy {
    uint32_t magic = 0x5354424c; uint16_t version = 1;
    uint8_t enabled = 1, paired = 1; uint8_t key[16]{7}; uint32_t checksum = 0;
  } legacy;
  legacy.checksum = crc(reinterpret_cast<const uint8_t*>(&legacy), 24);
  memcpy(fake::eeprom.data() + 320, &legacy, sizeof(legacy));
  fake::durable.clear();
  const auto networkBefore = fake::eeprom;
  fake::failSet = true;
  DeviceConfig failedMigration;
  assert(!failedMigration.begin());
  assert(fake::eeprom == networkBefore); // old credential retained on failure
  fake::failSet = false;
  DeviceConfig migrated;
  assert(migrated.begin() && migrated.bluetoothPaired());
  assert(!memcmp(migrated.bluetoothApps().apps[0].key, legacy.key, 16));
  assert(fake::eeprom[327] == 0); // retired legacy paired flag
  assert(!memcmp(fake::eeprom.data(), networkBefore.data(), 320));
  uint8_t secondKey[16]{8}, newKey[16]{9}, peer[6]{1};
  assert(migrated.saveBluetoothApp(1, secondKey, "Phone", 0, peer));
  const uint32_t oldId = migrated.bluetoothApps().apps[1].id;
  fake::failCommit = true;
  assert(!migrated.saveBluetoothApp(1, newKey, "Replacement", 0, peer));
  assert(migrated.bluetoothApps().apps[1].id == oldId);
  assert(!migrated.removeBluetoothApp(oldId));
  assert(!migrated.renameBluetoothApp(oldId, "Failed rename"));
  DeviceConfig reboot;
  assert(reboot.begin());
  assert(reboot.bluetoothApps().apps[1].id == oldId);
  assert(!memcmp(reboot.bluetoothApps().apps[1].key, secondKey, 16));
  fake::failCommit = false;
  assert(reboot.saveBluetoothApp(1, newKey, "Replacement", 0, peer));
  assert(reboot.bluetoothApps().apps[1].id != oldId);
  assert(reboot.removeBluetoothApp(reboot.bluetoothApps().apps[1].id));
  assert(reboot.bluetoothApps().count() == 1);
  DeviceConfig afterRevoke;
  assert(afterRevoke.begin() && afterRevoke.bluetoothApps().count() == 1);
  assert(!strcmp(afterRevoke.wifiSsid(), "Preserved WiFi"));
  assert(!strcmp(afterRevoke.deviceName(), "Bench timer"));
  assert(!strcmp(afterRevoke.accessPointSsid(), "Bench AP"));
  fake::failEepromCommit = true;
  afterRevoke.factoryReset();
  assert(afterRevoke.bluetoothApps().resetPending == 1);
  fake::failEepromCommit = false;
  DeviceConfig reset;
  assert(reset.begin() && !reset.bluetoothPaired() && !reset.bluetoothEnabled() && !reset.hasWifi());
  assert(reset.bluetoothApps().resetPending == 0);
  assert(reset.bluetoothApps().nextId == 1);
  // Corruption fails closed, never re-importing an older record.
  fake::durable[0] ^= 1;
  DeviceConfig corrupt;
  assert(!corrupt.begin() && !corrupt.bluetoothPaired());
  std::cout << "DeviceConfig migration, atomic failure, restart, reset and Wi-Fi preservation tests passed\n";
}
