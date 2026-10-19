"""Regression tests for the exact BLE management release policy (no SDK needed)."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_gate', ROOT / 'scripts/release/verify_release.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
sources = [gate.BLUETOOTH_SOURCE, gate.BLE_MANAGEMENT_SOURCE,
           gate.BLE_MANAGEMENT_HEADER, gate.WEB_MANAGEMENT_SOURCE, gate.WEB_SOURCE]
gate.verify_ble_management(*sources)

mutations = [
    (1, '!config_.setBluetoothEnabled(false)', 'false'),
    (0, 'disable();  // The setting was saved', 'setRadioEnabled(false, bootId_);  // The setting was saved'),
    (1, 'if (managementDisconnectAtMs_ != 0) return', 'if (false) return'),
    (1, '(managementDisconnectAtMs_ != 0 && !receipt)', 'false'),
    (1, 'status, body, sessionKey)', 'status, body)'),
    (0, 'attrHandle != managementResponseValueHandle_', 'false'),
    (0, '&kManagementRequestUuid.u', '&kUnexpectedWriteUuid.u'),
    (0, 'BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |\n                                        BLE_GATT_CHR_F_WRITE_ENC',
        'BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP'),
    (0, 'BLE_GATT_CHR_F_WRITE |\n                                        BLE_GATT_CHR_F_WRITE_ENC',
        'BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC'),
    (1, '!connectionBonded_ ||', ''),
    (1, 'constantTimeEqual(mac, request.mac, kMacBytes)', 'true'),
    (1, 'request.counter > previousCounter || exactRetry', 'true'),
    (1, 'generation == authorizationGeneration_', 'true'),
    (1, 'request.bootId != bootId_', 'false'),
    (1, 'uint16_t copied = 0;', 'timer_.release(); uint16_t copied = 0;'),
    (2, 'kMaxFrameBytes = 512', 'kMaxFrameBytes = 65536'),
    (2, 'kMacBytes = 16', 'kMacBytes = 4'),
    (3, '!otaArmed(nowMs)', 'false'),
    (3, '!timer_.isOpen()', 'false'),
    (3, 'otaCompleteImage();', 'Update.end(true);'),
    (4, 'httpOtaRejectedDuringBluetooth_ = bluetoothOtaActive_;',
        'httpOtaRejectedDuringBluetooth_ = false;'),
]
for index, old, new in mutations:
    assert old in sources[index], old
    changed = sources.copy()
    changed[index] = changed[index].replace(old, new)
    try:
        gate.verify_ble_management(*changed)
    except RuntimeError:
        pass
    else:
        raise AssertionError(f'release gate accepted removed safeguard: {old}')
print(f'BLE management release policy passed; {len(mutations)} unsafe source mutations rejected')

# The common timer authority now owns the boot/session checks. The release
# policy must follow that refactor without merely dropping the old safeguards.
gate.verify_shared_source()
authority_mutations = [
    ('TIMER_AUTHORITY_SOURCE', '!matches(command.boot, bootId_)', 'false'),
    ('TIMER_AUTHORITY_SOURCE', '!matches(command.session, timer_.sessionSequence())', 'false'),
    ('TIMER_AUTHORITY_SOURCE', 'command.session.value == 0', 'true'),
    ('BLE_CONTROL_SOURCE', 'command.boot = TimerPrecondition::of(requestedBootId)',
     'command.boot = TimerPrecondition::absent()'),
    ('BLE_CONTROL_SOURCE', 'command.session = TimerPrecondition::of(requestedSession)',
     'command.session = TimerPrecondition::absent()'),
    ('BLE_CONTROL_SOURCE', 'authority_.execute(command, nowMs)', 'unsafeExecute(command, nowMs)'),
    ('WEB_SOURCE', 'command.boot = commandAuthority_.bootFromText(', 'command.boot = ignoreBoot('),
    ('WEB_SOURCE', 'command.session = commandAuthority_.sessionFromText(', 'command.session = ignoreSession('),
    ('WEB_SOURCE', 'commandAuthority_.execute(command, millis())', 'unsafeExecute(command, millis())'),
    ('WEB_SOURCE', 'operations_.match(', 'ignoreReplay('),
    ('WEB_SOURCE', 'operations_.remember(', 'ignoreRemember('),
    ('TIMER_AUTHORITY_HEADER', 'kCapacity = 8', 'kCapacity = 0'),
]
for name, old, new in authority_mutations:
    original = getattr(gate, name)
    assert old in original, old
    setattr(gate, name, original.replace(old, new))
    try:
        try:
            gate.verify_shared_source()
        except RuntimeError:
            pass
        else:
            raise AssertionError(f'release gate accepted removed authority safeguard: {old}')
    finally:
        setattr(gate, name, original)
print(f'Shared REST/BLE authority release policy passed; {len(authority_mutations)} unsafe mutations rejected')
