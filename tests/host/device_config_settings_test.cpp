#include "DeviceConfig.h"
#include <EEPROM.h>
#include <cassert>
#include <cstring>
#include <iostream>

static void begin(DeviceConfig& config) {
#ifdef SMART_TIMER_HARDWARE_V2
  assert(config.begin());
#else
  config.begin();
#endif
}

int main() {
  DeviceConfig config;
  begin(config);
  assert(config.setWifi("Original WiFi", "original-password"));
  assert(config.setDeviceName("Original timer"));
  assert(config.setAccessPoint("Original AP", "original-password", false, false));
  const auto durable = fake::eeprom;
  fake::failEepromCommit = true;
  assert(!config.setWifi("Lost WiFi", "lost-password"));
  assert(!config.clearWifi());
  assert(!config.setDeviceName("Lost name"));
  assert(!config.setAccessPoint("Lost AP", "lost-password", false, false, true));
  assert(config.lastSaveFailed());
  assert(!strcmp(config.wifiSsid(), "Original WiFi"));
  assert(!strcmp(config.deviceName(), "Original timer"));
  assert(!strcmp(config.accessPointSsid(), "Original AP"));
  assert(fake::eeprom == durable);
  // A later successful, unrelated write must not flush failed settings.
  fake::failEepromCommit = false;
  assert(config.setDeviceName("Saved name"));
  assert(!config.lastSaveFailed());
  DeviceConfig reboot;
  begin(reboot);
  assert(!strcmp(reboot.deviceName(), "Saved name"));
  assert(!strcmp(reboot.wifiSsid(), "Original WiFi"));
  assert(!strcmp(reboot.accessPointSsid(), "Original AP"));
  assert(reboot.setAccessPoint("Saved AP", "saved-password", false, false, true));
  DeviceConfig standalone;
  begin(standalone);
  assert(!standalone.hasWifi());
  assert(!strcmp(standalone.accessPointSsid(), "Saved AP"));
  assert(standalone.factoryReset());
  DeviceConfig reset;
  begin(reset);
  assert(!reset.hasWifi() && !reset.accessPointSecured());
  assert(!strcmp(reset.deviceName(), "Smart Timer"));
  // Session switches (record at EEPROM offset 304: magic 4, version 2,
  // allowPause 1, allowControlLock 1, checksum 4). Never saved: both allowed.
  // Saved but unreadable: both off and reported, until the owner saves again.
  for (size_t i = 304; i < 304 + 12; ++i) fake::eeprom[i] = 0xFF;
  DeviceConfig neverSaved;
  begin(neverSaved);
  assert(neverSaved.pauseAllowed() && neverSaved.controlLockAllowed());
  assert(!neverSaved.sessionControlsRecovered());
  assert(neverSaved.setSessionControls(true, false));
  DeviceConfig saved;
  begin(saved);
  assert(saved.pauseAllowed() && !saved.controlLockAllowed() && !saved.sessionControlsRecovered());
  fake::eeprom[304 + 6] ^= 0x01;  // flip allowPause; the checksum no longer matches
  DeviceConfig corrupt;
  begin(corrupt);
  assert(!corrupt.pauseAllowed() && !corrupt.controlLockAllowed());
  assert(corrupt.sessionControlsRecovered());
  assert(corrupt.setSessionControls(false, false));  // same values still repair it
  assert(!corrupt.sessionControlsRecovered());
  DeviceConfig repaired;
  begin(repaired);
  assert(!repaired.pauseAllowed() && !repaired.controlLockAllowed());
  assert(!repaired.sessionControlsRecovered());
  assert(repaired.setSessionControls(true, true));
  std::cout << "Settings storage failures, buffer rollback and atomic AP transition passed\n";
}
