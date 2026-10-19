#!/usr/bin/env python3
"""Fail-fast checks for both OpenSmartTimer application images."""

from __future__ import annotations

import hashlib
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
VERSION_MATCH = re.search(
    r"(?m)^firmware_version\s*=\s*([0-9A-Za-z][0-9A-Za-z.-]*)\s*$",
    PLATFORMIO,
)
if VERSION_MATCH is None:
    raise RuntimeError("platformio.ini must define one firmware_version")
VERSION = VERSION_MATCH.group(1)

V1_FIRMWARE = ROOT / ".pio/build/v1/magbound_v1_esp8266" / f"OpenSmartTimer-{VERSION}-MagBoundV1-ESP8266.bin"
V2_FIRMWARE = ROOT / ".pio/build/v2/magbound_v2_esp32c3" / f"OpenSmartTimer-{VERSION}-MagBoundV2-ESP32C3.bin"
V2_PARTITIONS = ROOT / ".pio/build/v2/magbound_v2_esp32c3/partitions.bin"
WEB_SOURCE = "\n".join(path.read_text(encoding="utf-8") for path in sorted((ROOT / "src").glob("Web*.cpp")))
WEB_HEADER = (ROOT / "include/WebApplication.h").read_text(encoding="utf-8")
HARDWARE_HEADER = (ROOT / "include/Hardware.h").read_text(encoding="utf-8")
V1_PROFILE = (ROOT / "include/boards/MagBoundV1Esp8266.h").read_text(encoding="utf-8")
V2_PROFILE = (ROOT / "include/boards/MagBoundV2Esp32C3.h").read_text(encoding="utf-8")
PLATFORM_HEADER = (ROOT / "include/Platform.h").read_text(encoding="utf-8")
DEVICE_CONFIG_SOURCE = (ROOT / "src/DeviceConfig.cpp").read_text(encoding="utf-8")
LOCAL_NETWORK_SOURCE = (ROOT / "src/LocalNetwork.cpp").read_text(encoding="utf-8")
MAIN_SOURCE = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
BLUETOOTH_SOURCE = (ROOT / "src/BluetoothApplication.cpp").read_text(encoding="utf-8")
BLUETOOTH_HEADER = (ROOT / "include/BluetoothApplication.h").read_text(encoding="utf-8")
BLE_MANAGEMENT_SOURCE = (ROOT / "src/BluetoothManagement.cpp").read_text(encoding="utf-8")
BLE_MANAGEMENT_HEADER = (ROOT / "include/BleManagementProtocol.h").read_text(encoding="utf-8")
WEB_MANAGEMENT_SOURCE = (ROOT / "src/WebManagement.cpp").read_text(encoding="utf-8")
BLE_CONTROL_SOURCE = (ROOT / "src/BleControlProtocol.cpp").read_text(encoding="utf-8")
TIMER_AUTHORITY_SOURCE = (ROOT / "src/TimerCommandAuthority.cpp").read_text(encoding="utf-8")
TIMER_AUTHORITY_HEADER = (ROOT / "include/TimerCommandAuthority.h").read_text(encoding="utf-8")
WEB_HTML = (ROOT / "web/index.html").read_text(encoding="utf-8")
WEB_APP = (ROOT / "web/app.js").read_text(encoding="utf-8")

V1_MARKER = b"SMART_TIMER_ESP12_LOCAL_V1"
V2_MARKER = b"OPEN_SMART_TIMER_MAGBOUND_V2_ESP32C3"
V2_FRAMEWORK_MARKER = b"OPENSMARTTIMER_FRAMEWORK_PROFILE=ost-esp32c3-v2"
BLE_PROTOCOL_MARKER = b"ost-ble/1"
BLE_CONTROL_MARKER = b"bonded_hmac"
PEER_SIZE_PROBE_MARKER = b"peer-size-probe"
V1_MAXIMUM_IMAGE_SIZE = 1_044_464
V2_MAXIMUM_IMAGE_SIZE = 0x140000
# Release failure mark: each image must leave this much of its OTA slot free
# so the next fix still fits. Raise it deliberately, never lower it to pass.
MINIMUM_FREE_BYTES = 30 * 1024
EXPECTED_EBOOT_SHA256 = "2b84c131751a030115773d73137caa22bcd0f867e89e826a9567788ce022793a"
EXPECTED_VENDOR_ALLOWLIST = {
    "Legacy030": (483_472, 0x4D66232B),
    "Legacy130": (493_136, 0xC7225497),
    "VendorRollback": (1_287_440, 0xBF421EF6),
}
EXPECTED_V2_PARTITIONS = {
    "nvs": (0x9000, 0x5000),
    "otadata": (0xE000, 0x2000),
    "app0": (0x10000, 0x140000),
    "app1": (0x150000, 0x140000),
    "spiffs": (0x290000, 0x160000),
    "coredump": (0x3F0000, 0x10000),
}


def generated_build_id(target: str) -> str:
    header = ROOT / ".pio/build" / target / "generated_build_identity.h"
    match = re.search(
        r'^#define SMART_TIMER_BUILD_ID "([0-9a-f]{24})"$',
        header.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    require(match is not None, f"missing build identity for {target}")
    return match.group(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def require_headroom(label: str, image: bytes, slot: int) -> None:
    free = slot - len(image)
    require(free >= MINIMUM_FREE_BYTES,
            f"{label} image leaves {free} bytes of its {slot}-byte OTA slot free; "
            f"the release minimum is {MINIMUM_FREE_BYTES}")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_partition_layout(data: bytes) -> dict[str, tuple[int, int]]:
    result: dict[str, tuple[int, int]] = {}
    for offset in range(0, len(data), 32):
        record = data[offset : offset + 32]
        if len(record) != 32:
            break
        magic, _type, _subtype, address, size, raw_name, _flags = struct.unpack("<HBBII16sI", record)
        if magic != 0x50AA:
            break
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        result[name] = (address, size)
    return result


def verify_v1(image: bytes) -> None:
    require(len(image) <= V1_MAXIMUM_IMAGE_SIZE, "V1 image exceeds 4M/2M OTA slot")
    require_headroom("V1", image, V1_MAXIMUM_IMAGE_SIZE)
    require(image[:4] == bytes.fromhex("e9 02 03 40"), "V1 outer header is not ESP8266 DOUT/4MB/40MHz")
    require(image[4096:4100] == bytes.fromhex("e9 05 03 40"), "V1 application header mismatch")
    require(image.count(V1_MARKER) == 1, "V1 product marker missing or duplicated")
    require(generated_build_id("v1/magbound_v1_esp8266").encode() in image,
            "V1 embedded build identity is missing")
    require(V2_MARKER not in image, "V2 marker leaked into V1 image")
    require(V2_FRAMEWORK_MARKER not in image,
            "V2 framework marker leaked into V1 image")
    require(BLE_PROTOCOL_MARKER not in image,
            "V2 BLE protocol leaked into V1 image")
    require(BLE_CONTROL_MARKER not in image,
            "V2 BLE control mode leaked into V1 image")
    require(b"OST-MGMT-v1" not in image,
            "V2 BLE management leaked into V1 image")
    require(PEER_SIZE_PROBE_MARKER not in image,
            "build-only BLE peer probe leaked into V1 image")
    require(hashlib.sha256(image[:4096]).hexdigest() == EXPECTED_EBOOT_SHA256,
            "V1 eboot differs from the recovered-firmware baseline")


def verify_v2(image: bytes) -> None:
    require(len(image) <= V2_MAXIMUM_IMAGE_SIZE, "V2 image exceeds 1.25 MiB OTA slot")
    require_headroom("V2", image, V2_MAXIMUM_IMAGE_SIZE)
    require(len(image) >= 16 and image[0] == 0xE9, "V2 ESP image magic is missing")
    require(image[2] == 0x02, "V2 image is not configured for DIO flash mode")
    require(image[3] == 0x2F, "V2 image is not configured for 4MB/80MHz flash")
    require(int.from_bytes(image[12:14], "little") == 5, "V2 image header does not identify ESP32-C3")
    require(image.count(V2_MARKER) == 1, "V2 product marker missing or duplicated")
    require(generated_build_id("v2/magbound_v2_esp32c3").encode() in image,
            "V2 embedded build identity is missing")
    require(image.count(V2_FRAMEWORK_MARKER) == 1,
            "V2 slim-framework marker missing or duplicated")
    require(BLE_PROTOCOL_MARKER in image,
            "V2 BLE protocol marker is missing")
    require(BLE_CONTROL_MARKER in image,
            "V2 authenticated-control marker is missing")
    require(PEER_SIZE_PROBE_MARKER not in image,
            "build-only BLE peer probe leaked into normal V2 image")
    require(V1_MARKER not in image, "V1 marker leaked into V2 image")
    require(V2_PARTITIONS.exists(), "V2 partition table was not built")
    require(parse_partition_layout(V2_PARTITIONS.read_bytes()) == EXPECTED_V2_PARTITIONS,
            "V2 partition table differs from the recovered vendor layout")


def method_body(source: str, name: str) -> str:
    match = re.search(r"^[^\n]*\b" + re.escape(name) + r"\([^;]*?\)\s*(?:const\s*)?\{(.*?)^\}",
                      source, re.MULTILINE | re.DOTALL)
    require(match is not None, f"missing safety-checked method: {name}")
    return match.group(1)


def verify_ble_management(application: str, management: str, protocol: str,
                          web_management: str, web: str) -> None:
    # Exact writable surface, not a blanket ban on the new transport. Only
    # management fragments may use WRITE_NO_RSP; control/approval still use ACKs.
    characteristics = re.findall(
        r"&([A-Za-z0-9_]+)\.u,.*?static_cast<ble_gatt_chr_flags>\((.*?)\)",
        application.split("ble_gatt_svc_def kServices", 1)[0], re.DOTALL)
    flags = {name: set(re.findall(r"BLE_GATT_CHR_F_[A-Z_]+", value))
             for name, value in characteristics}
    writable = {name: value for name, value in flags.items()
                if value & {"BLE_GATT_CHR_F_WRITE", "BLE_GATT_CHR_F_WRITE_NO_RSP"}}
    require(writable == {
        "kControlCommandUuid": {"BLE_GATT_CHR_F_WRITE", "BLE_GATT_CHR_F_WRITE_ENC"},
        "kPairingNicknameUuid": {"BLE_GATT_CHR_F_READ", "BLE_GATT_CHR_F_READ_ENC",
                                "BLE_GATT_CHR_F_WRITE", "BLE_GATT_CHR_F_WRITE_ENC"},
        "kManagementRequestUuid": {"BLE_GATT_CHR_F_WRITE", "BLE_GATT_CHR_F_WRITE_NO_RSP",
                                   "BLE_GATT_CHR_F_WRITE_ENC"},
    } and len(characteristics) == len(flags), "unexpected or unencrypted BLE writable characteristic")
    require(flags.get("kManagementResponseUuid") == {
        "BLE_GATT_CHR_F_READ", "BLE_GATT_CHR_F_READ_ENC", "BLE_GATT_CHR_F_NOTIFY"
    }, "management response must remain encrypted read/notify only")
    write = method_body(management, "BluetoothApplication::onManagementWrite")
    read = method_body(management, "BluetoothApplication::onManagementRead")
    process = method_body(management, "BluetoothApplication::processManagementRequest")
    for callback in (write, read):
        require(all(gate in callback for gate in (
            "!connected_", "connHandle != connectionHandle_", "!connectionEncrypted_",
            "!connectionBonded_", "!controlAuthenticated_", "BleAccessPolicy::authorizationError")),
            "management GATT access must require the authenticated encrypted bonded connection")
        require(not re.search(r"timer_\.|managementHost_->|Hardware::|Update\.", callback),
                "management GATT callback must not perform mutations")
    require("if (managementDisconnectAtMs_ != 0) return" in write and
            "managementReceiptReadable(connHandle, millis())" in read and
            "(managementDisconnectAtMs_ != 0 && !receipt)" in read and
            "attrHandle != managementResponseValueHandle_" in application and
            "status, body, sessionKey)" in process and
            "managementTerminalReplyReady_ = true" in management,
            "revoked BLE connection must only read its bounded signed terminal receipt")
    radio_off = process.split("Opcode::BluetoothOff", 1)[1].split("routeFor(request.opcode", 1)[0]
    require("!config_.setBluetoothEnabled(false)" in radio_off and
            radio_off.index("!config_.setBluetoothEnabled(false)") < radio_off.index("status = 202") and
            "settings_save_failed" in radio_off and
            "setRadioEnabled(false, bootId_)" not in method_body(application, "BluetoothApplication::loop"),
            "Bluetooth-off must persist before acknowledging and must not resave after the reply")
    for gate in ("constantTimeEqual(mac, request.mac, kMacBytes)",
                 "request.counter > previousCounter || exactRetry", "if (!macOk || !digestOk",
                 "pendingManagementGeneration_ = authorizationGeneration_",
                 "pendingManagementAppId_ = authenticatedAppId_", "pendingManagement_ = true"):
        require(gate in write, f"management request safeguard missing: {gate}")
    for gate in ("connectionEncrypted_ && connectionBonded_", "generation == authorizationGeneration_",
                 "config_.bluetoothApps().find(appId) == slot", "request.bootId != bootId_",
                 "managementHost_->manage(", "operation_id_conflict"):
        require(gate in process, f"management loop safeguard missing: {gate}")
    require("processManagementRequest(nowMs);" in application,
            "management dispatch must run from the application loop")
    for bound in ("kMaxFrameBytes = 512", "kMaxBodyBytes = 8192", "kMacBytes = 16",
                  "length > kMaxFrameBytes", "length_ + payload > kMaxFrameBytes"):
        require(bound in protocol, f"management frame bound missing: {bound}")
    begin = method_body(web_management, "WebApplication::routeBluetoothOtaBegin")
    for gate in ("bluetoothOtaActive_ || otaQuietActive(nowMs)", "!timer_.isOpen()",
                 "network_.scanInProgress()", "!otaArmed(nowMs)", "parseOtaMetadata(",
                 "otaArmedUntilMs_ = 0", "otaBeginImage(nowMs)"):
        require(gate in begin, f"BLE OTA admission safeguard missing: {gate}")
    require("otaAcceptBytes(data, length)" in method_body(web_management, "WebApplication::manageOtaChunk")
            and "otaCompleteImage();" in method_body(web_management, "WebApplication::routeBluetoothOtaFinish")
            and "otaAbortImage();" in method_body(web_management, "WebApplication::routeBluetoothOtaAbort"),
            "BLE OTA must reuse shared streaming validation and cancellation")
    upload = method_body(web, "WebApplication::handleOtaUpload")
    complete = method_body(web, "WebApplication::handleOtaComplete")
    require("httpOtaRejectedDuringBluetooth_ = bluetoothOtaActive_;" in upload and
            upload.index("if (httpOtaRejectedDuringBluetooth_)") < upload.index("otaResetUploadState();") and
            complete.index("if (httpOtaRejectedDuringBluetooth_)") < complete.index("if (!otaAllowed_"),
            "HTTP uploads must not reset or finalize an active BLE OTA")


def verify_shared_source() -> None:
    require(
        PLATFORMIO.count(
            'SMART_TIMER_VERSION=\\"${opensmarttimer.firmware_version}\\"'
        ) == 2,
        "both targets must consume the centralized firmware version",
    )
    require(PLATFORMIO.count("pre:scripts/build/build_identity.py") == 2,
            "both targets must generate an embedded build identity")
    require(
        "OpenSmartTimer-${opensmarttimer.firmware_version}-MagBoundV1-ESP8266"
        in PLATFORMIO
        and "OpenSmartTimer-${opensmarttimer.firmware_version}-MagBoundV2-ESP32C3"
        in PLATFORMIO,
        "artifact names must consume the centralized firmware version",
    )
    require('#include "OtaImageValidation.h"' in WEB_SOURCE and
            "OtaImageValidation::matchesTarget" in WEB_SOURCE,
            "OTA does not use the shared target-header validator")
    require("verifyRollbackLater()" in MAIN_SOURCE and
            "esp_ota_mark_app_valid_cancel_rollback" in MAIN_SOURCE and
            "kBootHealthStableMs = 5000" in MAIN_SOURCE,
            "V2 delayed OTA confirmation health gate is missing")
    require("failOpenInitialized && configStorageReady" in MAIN_SOURCE and
            "recoveryNetworkReady && webApplication.ready()" in MAIN_SOURCE,
            "V2 OTA confirmation does not require all recovery-health gates")
    require("gpio_set_level(static_cast<gpio_num_t>(kLockPinA), 0)" in HARDWARE_HEADER and
            "gpio_set_level(static_cast<gpio_num_t>(kLockIndicatorPin), 0)" in HARDWARE_HEADER and
            "failOpenInitialized = Hardware::failOpenInitializationSucceeded();" in MAIN_SOURCE,
            "V2 fail-open latches are not directly preloaded and health-gated")
    require("if (status == WL_WRONG_PASSWORD)" in LOCAL_NETWORK_SOURCE and
            "static_cast<int>(status) == 6" not in LOCAL_NETWORK_SOURCE,
            "Wi-Fi status 6 must remain target-specific (wrong password on V1, disconnected on V2)")
    require("OPENSMARTTIMER_FRAMEWORK_PROFILE" in MAIN_SOURCE and
            "framework-arduinoespressif32-libs @ symlink://.framework/esp32c3-libs"
            in PLATFORMIO,
            "V2 build is not pinned to the local verified framework package")
    require("-<BleControlProtocol.cpp>" in PLATFORMIO,
            "V1 build no longer explicitly excludes the V2 BLE command parser")
    require("kBluetoothAutoStartDelayMs = 15000" in MAIN_SOURCE and
            "configStorageReady && deviceConfig.bluetoothEnabled()" in MAIN_SOURCE and
            "bluetoothApplication.begin(webApplication.bootId())" in MAIN_SOURCE and
            '"/api/v1/bluetooth/enable"' in WEB_SOURCE and
            "bluetooth->setRadioEnabled(true, bootId_)" in WEB_SOURCE,
            "V2 BLE persistent preference or delayed recovery-first startup is missing")
    require("BLE_GATT_CHR_F_NOTIFY" in BLUETOOTH_SOURCE and
            "BLE_GATT_CHR_F_READ" in BLUETOOTH_SOURCE,
            "V2 BLE read/notify service is missing")
    require("kSystemInfoFrameSize = 44" in BLUETOOTH_HEADER and
            "kSystemInfoUuid" in BLUETOOTH_SOURCE and
            "f00df299-464f-4921-9285-7749593b1144" in
            (ROOT / "docs/BLUETOOTH.md").read_text(encoding="utf-8") and
            "updateSystemInfoFrame(nowMs)" in BLUETOOTH_SOURCE and
            "controlAuthenticated_" in BLUETOOTH_SOURCE.split(
                "int BluetoothApplication::onGattAccess", 1)[1].split(
                    "bool BluetoothApplication::startAdvertising", 1)[0],
            "authenticated V2 BLE system diagnostics are missing")
    verify_ble_management(BLUETOOTH_SOURCE, BLE_MANAGEMENT_SOURCE,
                          BLE_MANAGEMENT_HEADER, WEB_MANAGEMENT_SOURCE, WEB_SOURCE)
    require("BlePairingPolicy::nicknameWritable(pendingReadable(), pairingLabelExplicit_)" in BLUETOOTH_SOURCE
            and "attrHandle == pairingNicknameValueHandle_" in BLUETOOTH_SOURCE,
            "pairing nickname must remain physically gated and respect an owner-selected label")
    require("processControlCommand(nowMs, mutationsAllowed)" in BLUETOOTH_SOURCE and
            "controlProtocol_.execute" in BLUETOOTH_SOURCE and
            "timer_." not in BLUETOOTH_SOURCE.split(
                "int BluetoothApplication::onGattAccess", 1)[1].split(
                    "bool BluetoothApplication::startAdvertising", 1)[0],
            "BLE GATT callback must queue mutations for the main loop")
    require("mutationTransitionActive(nowMs)" in MAIN_SOURCE and
            "SystemTransition" in BLE_CONTROL_SOURCE and
            "mutationsAllowed" in BLE_CONTROL_SOURCE,
            "REST and BLE do not share the system-transition mutation gate")
    require("OST-AUTH-v1" in BLUETOOTH_SOURCE and
            "OST-SESSION-v1" in BLUETOOTH_SOURCE and
            "OST-CMD-v1" in BLUETOOTH_SOURCE and
            "mbedtls_sha256" in BLUETOOTH_SOURCE and
            "kControlFrameSize" in BLUETOOTH_HEADER and
            "kControlMacSize = 8" in BLUETOOTH_HEADER and
            "kMaximumAuthenticationFailures = 5" in BLUETOOTH_HEADER and
            "kAuthenticationRetryMs = 500" in BLUETOOTH_HEADER,
            "BLE application authentication or command-integrity safeguards are missing")
    require("BLE_GATT_CHR_F_READ_ENC" in BLUETOOTH_SOURCE and
            "BLE_GATT_CHR_F_WRITE_ENC" in BLUETOOTH_SOURCE and
            "ble_hs_cfg.sm_bonding = 1" in BLUETOOTH_SOURCE and
            "ble_store_config_init" in BLUETOOTH_SOURCE and
            "ble_store_clear()" in BLUETOOTH_SOURCE,
            "BLE encryption, bonding, persistence, or revocation is missing")
    require("pairingSecretValueHandle_" in BLUETOOTH_SOURCE and
            "kBluetoothPairingHoldMinMs" in MAIN_SOURCE and
            "confirmPhysicalPairing" in MAIN_SOURCE,
            "physical no-Wi-Fi Bluetooth pairing is missing")
    pairing_read = BLUETOOTH_SOURCE.split(
        "if (attrHandle == pairingSecretValueHandle_)", 1)[1].split(
            "if (attrHandle == controlChallengeValueHandle_)", 1)[0]
    require("saveBluetoothPairing" not in pairing_read and
            "PairingState::WaitingForCommit" in pairing_read and
            "OST-PAIR-COMMIT-v1" in BLUETOOTH_SOURCE and
            "processPairingCommit(nowMs)" in BLUETOOTH_SOURCE and
            "saveBluetoothApp(pairingAppSlot_, pendingPairingKey_" in BLUETOOTH_SOURCE,
            "BLE pairing does not use pending-key proof and main-loop commit")
    require("kRecentCapacity = 8" in (ROOT / "include/BleControlProtocol.h").read_text(encoding="utf-8") and
            "kSemanticCommandSize = 20" in (ROOT / "include/BleControlProtocol.h").read_text(encoding="utf-8") and
            "OperationConflict" in BLE_CONTROL_SOURCE and
            "command.boot = TimerPrecondition::of(requestedBootId)" in BLE_CONTROL_SOURCE and
            "command.session = TimerPrecondition::of(requestedSession)" in BLE_CONTROL_SOURCE and
            "authority_.execute(command, nowMs)" in BLE_CONTROL_SOURCE and
            "!matches(command.boot, bootId_)" in TIMER_AUTHORITY_SOURCE and
            "!matches(command.session, timer_.sessionSequence())" in TIMER_AUTHORITY_SOURCE and
            "command.session.value == 0" in TIMER_AUTHORITY_SOURCE,
            "BLE command idempotency or boot/session preconditions are missing")
    clear_connection = BLUETOOTH_SOURCE.split(
        "void BluetoothApplication::clearConnectionAuthorization()", 1)[1].split(
            "void BluetoothApplication::failAuthentication", 1)[0]
    require("controlAuthenticationFailureCount_" not in clear_connection and
            "pairingCommitConfirmed_" in BLUETOOTH_SOURCE and
            "connectionBonded_" in BLUETOOTH_SOURCE.split(
                "bool BluetoothApplication::authenticateControl", 1)[1].split(
                    "bool BluetoothApplication::authenticateCommand", 1)[0],
            "BLE reconnect throttling, pairing confirmation, or bond authorization regressed")
    timer_header = (ROOT / "include/TimerController.h").read_text(encoding="utf-8")
    require("TimerState state;" in timer_header and
            "uint32_t sessionSequence;" in timer_header and
            "event.sessionSequence" in BLUETOOTH_SOURCE,
            "event replay no longer carries event-time state/session metadata")
    bleak_client = (ROOT / "examples/bleak_control.py").read_text(encoding="utf-8")
    require("challenge[1] & 0x04" in bleak_client and
            "await authenticate(client, key)" in bleak_client and
            "recover_pending_key" in bleak_client,
            "reference client no longer proves or recovers Pairing Commit")
    require("ble_gap_disc(" not in BLUETOOTH_SOURCE and
            "ble_hs_adv_parse_fields" not in BLUETOOTH_SOURCE and
            "kPeerScanDurationMs" not in BLUETOOTH_HEADER and
            "kPeerCapacity" not in BLUETOOTH_HEADER,
            "unfinished BLE peer discovery leaked into the RC build")
    require("ble_gap_connect(" not in BLUETOOTH_SOURCE and
            "ble_gattc_write" not in BLUETOOTH_SOURCE and
            "ble_gattc_read" not in BLUETOOTH_SOURCE,
            "V2 BLE discovery must not connect to or access a peer GATT server")
    require('"/api/v1/bluetooth/peers"' not in WEB_SOURCE and
            '"/api/v1/bluetooth/peers/scan"' not in WEB_SOURCE,
            "unfinished BLE peer discovery endpoints leaked into the RC API")
    require('"/api/v1/bluetooth/disable"' in WEB_SOURCE and
            '"/api/v1/bluetooth/pairing/start"' in WEB_SOURCE and
            '"/api/v1/bluetooth/pairing/cancel"' in WEB_SOURCE and
            '"/api/v1/bluetooth/pairing/revoke"' in WEB_SOURCE and
            "sendBluetoothStatus()" in WEB_SOURCE and
            "return startPhysicalPairing(nowMs, bootId);" in BLUETOOTH_SOURCE and
            '"access_key"' not in WEB_SOURCE,
            "V2 BLE radio/pairing endpoints or physical-confirmation pairing are missing")
    require("connectionPeerIsBonded" in BLUETOOTH_SOURCE and
            "acceptedCount_ > pairingAcceptAfterCount_" in BLUETOOTH_SOURCE and
            "kAuthenticationBackoffMs = 30000" in BLUETOOTH_HEADER and
            "disableControl();" not in BLUETOOTH_SOURCE.split(
                "bool BluetoothApplication::authenticateControl", 1)[1].split(
                    "void BluetoothApplication::processControlCommand", 1)[0],
            "BLE connection admission or per-connection auth backoff is unsafe")
    require("clearConnectionAuthorization();" in BLUETOOTH_SOURCE and
            "clearSessionKey();" in BLUETOOTH_SOURCE and
            "bondCleanupPending_" in BLUETOOTH_SOURCE,
            "BLE disconnect key wiping or retryable bond cleanup is missing")
    require("nextEventNotificationId_" in BLUETOOTH_SOURCE and
            "eventReplayRequested_" in BLUETOOTH_SOURCE and
            "truncated" in BLUETOOTH_SOURCE and
            "lastNotifiedEventRevision_" not in BLUETOOTH_SOURCE,
            "BLE event replay still collapses or loses ring events")
    require("Boot = 0" in (ROOT / "include/TimerController.h").read_text(encoding="utf-8") and
            "kOperationCapabilities = 0xff" in BLUETOOTH_HEADER and
            "kSecurityCapabilities = 0xff" in BLUETOOTH_HEADER and
            "kRequiredControlMtu = 35" in BLUETOOTH_HEADER,
            "BLE wire event IDs, capability masks, or MTU contract are not stable")
    require("capabilitiesFrame_[23] = kAuthenticationMacSize;" in BLUETOOTH_SOURCE and
            BLUETOOTH_SOURCE.count("constantTimeEqual(digest, frame + 6, kAuthenticationMacSize)") == 2 and
            "kAuthenticationMacSize == 14" in BLUETOOTH_HEADER,
            "BLE capability tag size must match both authentication and pairing commit")
    require("bluetooth_peer_control\\\":false" in WEB_SOURCE,
            "capabilities must not advertise unimplemented BLE peer control")
    require("kEventStreamClientLimit = 3" in WEB_HEADER, "SSE client limit is no longer fixed at three")
    require("acceptEventStream" in WEB_SOURCE and "Transfer-Encoding: chunked" in WEB_SOURCE,
            "bounded SSE handoff/chunk framing is missing")
    require("kEventStreamMaximumHeaderBytes" in WEB_SOURCE and "kEventStreamWriteTimeoutMs" in WEB_SOURCE,
            "SSE header/write bounds are missing")
    require('\\"default_enabled\\":true' in WEB_SOURCE and '\\"rest_fallback\\":true' in WEB_SOURCE,
            "capabilities no longer report automatic SSE with REST fallback")
    require('"/api/v1/timer/release"' in WEB_SOURCE and
            "command.session = commandAuthority_.sessionFromText(" in WEB_SOURCE and
            "command.boot = commandAuthority_.bootFromText(" in WEB_SOURCE and
            "commandAuthority_.execute(command, millis())" in WEB_SOURCE and
            "operations_.match(" in WEB_SOURCE and "operations_.remember(" in WEB_SOURCE and
            "kCapacity = 8" in TIMER_AUTHORITY_HEADER,
            "reliable mutation safeguards are missing")
    require("otaUnverifiedAllowed_ = true" in WEB_SOURCE and
            "otaUploadUnverifiedAllowed_ = otaAllowed_ && unverifiedAllowed" in WEB_SOURCE,
            "physical one-upload override is missing")
    require("deviceConfig.factoryReset();" in MAIN_SOURCE and "kFactoryResetHoldMs" in MAIN_SOURCE and
            "kFactoryResetHoldMs = 10UL * 1000UL" in HARDWARE_HEADER,
            "ten-second physical factory reset is missing")
    require('id="liveUpdates"' not in WEB_HTML and "connectLiveUpdates();" in WEB_APP,
            "browser SSE is not automatic or still exposes a toggle")
    require("localStorage" not in WEB_APP and "sessionStorage.setItem('smartTimerTheme'" in WEB_APP,
            "appearance preferences must remain browser-session-local")

    factory_reset = re.search(r"bool DeviceConfig::factoryReset\(\)\s*\{(.*?)\n\}", DEVICE_CONFIG_SOURCE, re.DOTALL)
    require(factory_reset is not None, "physical factory reset implementation is missing")
    factory_body = factory_reset.group(1)
    for step in ("createDefaults();", "createIdentityDefaults();", "createAccessPointDefaults();", "createBluetoothDefaults();"):
        require(step in factory_body, f"factory reset omits {step}")
    require(factory_body.count("commitStorage()") == 1,
            "factory reset must commit prepared records together")
    require('EEPROMClass openSmartTimerStorage("opensmarttimer");' in DEVICE_CONFIG_SOURCE,
            "V2 configuration must use the project-specific NVS namespace")
    require("bluetoothApplication.clearBondsForFactoryReset" in MAIN_SOURCE,
            "V2 factory reset does not clear Bluetooth bonds")
    require("EEPROMClass openSmartTimerStorage;" in DEVICE_CONFIG_SOURCE,
            "V1 configuration must retain the fixed EEPROM-sector backend")
    require(PLATFORMIO.count("-D NO_GLOBAL_EEPROM") == 2,
            "both targets must disable the unused generic EEPROM object")
    require(not re.search(r"\bEEPROM\.(?:begin|get|put|commit)\s*\(",
                          DEVICE_CONFIG_SOURCE),
            "configuration must not use the generic global EEPROM object")

    for fragment in (
        "constexpr uint8_t kLockPinA = 3;",
        "constexpr uint8_t kLockIndicatorPin = 6;",
        "constexpr uint8_t kStatusPin = 5;",
        "constexpr uint8_t kEmergencyPin = 0;",
        "constexpr uint8_t kConfigPin = 4;",
        "constexpr uint8_t kAuxiliaryInputPin = 1;",
    ):
        require(fragment in V2_PROFILE, f"V2 hardware contract missing: {fragment}")
    require('kChipFamily = "ESP32-C3";' in V2_PROFILE and 'kChipFamily = "ESP8266";' in V1_PROFILE
            and 'return Board::Profile::kChipFamily;' in PLATFORM_HEADER,
            "platform identity fields are incomplete")

    for name, (size, checksum) in EXPECTED_VENDOR_ALLOWLIST.items():
        size_match = re.search(rf"k{name}Size\s*=\s*(\d+)", WEB_SOURCE)
        crc_match = re.search(rf"k{name}Crc32\s*=\s*0x([0-9A-Fa-f]+)", WEB_SOURCE)
        require(size_match is not None and int(size_match.group(1)) == size,
                f"{name} allowlisted size differs from pinned metadata")
        require(crc_match is not None and int(crc_match.group(1), 16) == checksum,
                f"{name} allowlisted CRC32 differs from pinned metadata")
    allowlist_names = set(re.findall(r"k([A-Za-z0-9]+)Crc32\s*=", WEB_SOURCE))
    require(allowlist_names == set(EXPECTED_VENDOR_ALLOWLIST),
            "runtime OTA allowlist must contain vendor rollback images only")


def describe(label: str, image: bytes, marker: bytes, slot: int) -> None:
    print(f"{label}: size={len(image)} free={slot - len(image)} crc32={crc32(image):08x} "
          f"sha256={hashlib.sha256(image).hexdigest()} marker_offset=0x{image.index(marker):x}")


def main() -> int:
    framework_check = subprocess.run(
        [sys.executable, str(ROOT / "scripts/framework/verify_framework_esp32c3.py")],
        cwd=ROOT,
        check=False,
    )
    require(framework_check.returncode == 0,
            "installed V2 framework package failed verification")
    require(V1_FIRMWARE.exists() and V2_FIRMWARE.exists(),
            "build both targets first with: ./scripts/build_firmware.sh all")
    v1_image = V1_FIRMWARE.read_bytes()
    v2_image = V2_FIRMWARE.read_bytes()
    verify_v1(v1_image)
    verify_v2(v2_image)
    verify_shared_source()
    print("dual-target release verification passed")
    describe("V1", v1_image, V1_MARKER, V1_MAXIMUM_IMAGE_SIZE)
    describe("V2", v2_image, V2_MARKER, V2_MAXIMUM_IMAGE_SIZE)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"release verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
