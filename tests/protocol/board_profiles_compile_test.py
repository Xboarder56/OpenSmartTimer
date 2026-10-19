"""Compile target contracts independently of Arduino; reject crossed targets."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
V1 = "SMART_TIMER_TARGET_MAGBOUND_V1_ESP8266"
V2 = "SMART_TIMER_TARGET_MAGBOUND_V2_ESP32C3"
HW2 = "SMART_TIMER_HARDWARE_V2"
SOURCE = r'''
#include "BoardProfile.h"
#include "OtaImageValidation.h"
static_assert(Board::Flash::kPhysicalBytes == 0x400000, "physical flash");
static_assert(Board::Profile::kLockActiveHigh && Board::Profile::kInputActiveHigh, "polarity");
#ifdef SMART_TIMER_HARDWARE_V2
static_assert(Board::Profile::kLockPinA == 3 && Board::Profile::kLockPinB == 3, "V2 lock");
static_assert(Board::Profile::kStatusPin == 5 && Board::Profile::kLockIndicatorPin == 6, "V2 LEDs");
static_assert(Board::Profile::kConfigPin == 4 && Board::Profile::kEmergencyPin == 0 && Board::Profile::kAuxiliaryInputPin == 1, "V2 inputs");
static_assert(Board::Flash::kMaximumImageBytes == 0x140000 && Board::Flash::kImageChipId == 5, "V2 OTA");
static_assert(Board::Flash::kImageMode == 2 && Board::Flash::kImageSizeFrequency == 0x2f, "V2 header");
static_assert(Board::Flash::kNvsOffset == 0x9000 && Board::Flash::kNvsBytes == 0x5000, "NVS");
static_assert(Board::Flash::kOtaDataOffset == 0xe000 && Board::Flash::kOtaDataBytes == 0x2000, "OTA data");
static_assert(Board::Flash::kApp0Offset == 0x10000 && Board::Flash::kApp1Offset == 0x150000, "OTA slots");
#else
static_assert(Board::Profile::kLockPinA == 4 && Board::Profile::kLockPinB == 13, "V1 locks");
static_assert(Board::Profile::kStatusPin == 12 && Board::Profile::kEmergencyPin == 10 && Board::Profile::kConfigPin == 5, "V1 GPIO");
static_assert(Board::Flash::kMaximumImageBytes == 1044464 && Board::Flash::kApplicationOffset == 0x1000, "V1 OTA");
static_assert(Board::Flash::kImageMode == 3 && Board::Flash::kImageSizeFrequency == 0x40, "V1 DOUT header");
#endif
'''


def check(defines, succeeds):
    result = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Werror",
                             "-fsyntax-only", "-x", "c++", "-I", str(ROOT / "include"),
                             *[f"-D{x}=1" for x in defines], "-"],
                            input=SOURCE, text=True, capture_output=True)
    assert (result.returncode == 0) == succeeds, (defines, result.stderr)


for definitions in [[], [HW2], [V1], [V2, HW2],
                    [V1, "ARDUINO", "ESP8266"],
                    [V2, HW2, "ARDUINO", "ESP32", "CONFIG_IDF_TARGET_ESP32C3"]]:
    check(definitions, True)
for definitions in [[V1, V2], [V1, HW2], [V2], ["ARDUINO", "ESP8266"],
                    [V1, "ARDUINO", "ESP32"], [V2, HW2, "ARDUINO", "ESP8266"],
                    [V2, HW2, "ARDUINO", "ESP32"]]:
    check(definitions, False)
print("Board/flash contracts: both products compile; invalid target/toolchain combinations rejected")
