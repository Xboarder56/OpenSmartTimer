#!/usr/bin/env python3
"""Reference ost-ble/1 client for an OpenSmartTimer V2.

Install Bleak in a virtual environment. Enable Bluetooth in the timer UI or
hold Reset for 3–5 seconds while the timer is open. Pairing keys are prompted
for by default so they do not need to appear in shell history.
"""

from __future__ import annotations

import argparse
import asyncio
import getpass
import hashlib
import hmac
import json
import os
import secrets
import struct
from pathlib import Path

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "70ba26f4-2f3d-44df-9b73-bfb3d3625961"
PROTOCOL_INFO_UUID = "8d826f5b-7282-499f-bbde-fe361c19a8ed"
CAPABILITIES_UUID = "6fb27438-e619-4291-ae73-448a1e973c5a"
SYSTEM_INFO_UUID = "f00df299-464f-4921-9285-7749593b1144"
LIVE_STATUS_UUID = "d1a93d5e-d4fe-434e-acc6-42d86d79ab63"
EVENT_UUID = "917421bb-67a0-4b34-92a5-5219427a670c"
CONTROL_CHALLENGE_UUID = "eab30710-b1bb-42eb-ba07-115d18d91f64"
CONTROL_COMMAND_UUID = "1fa0980f-7224-4d51-a315-c84fde9cbdc5"
CONTROL_RESULT_UUID = "2df3b9a1-f2ad-4660-97f5-14325d9eb6db"
PAIRING_SECRET_UUID = "546d8ce1-3570-4f2b-a1d8-c5632cb4f091"
PAIRING_NICKNAME_UUID = "63af724d-0379-4191-b785-4e6baa457612"

PROTOCOL_VERSION = 1
OPCODES = {
    "start": 0x10, "random": 0x10, "release": 0x11,
    "pause": 0x12, "resume": 0x13, "adjust": 0x14,
    "lock-controls": 0x15, "identify": 0x16,
}
RESULTS = {
    0: "applied", 1: "invalid_frame", 2: "unauthorized",
    3: "stale_boot", 4: "stale_session", 5: "invalid_state",
    6: "invalid_duration", 7: "emergency_input_active",
    8: "controls_locked", 9: "maximum_duration_reached",
    10: "unsupported_opcode", 11: "operation_conflict",
    12: "system_transition",
}
STATES = {0: "open", 1: "running", 2: "paused"}
RELEASE_REASONS = {
    0: "boot", 1: "expired", 2: "user", 3: "emergency",
    4: "adjusted_to_zero", 5: "share_link_expired",
}

RESET_REASONS = {
    0: "unknown", 1: "power_on", 2: "external", 3: "software",
    4: "panic", 5: "interrupt_watchdog", 6: "task_watchdog",
    7: "other_watchdog", 8: "deep_sleep", 9: "brownout", 10: "sdio",
}
WIFI_STATES = {
    0: "ap", 1: "connecting", 2: "connected", 3: "failed",
    4: "disconnected",
}


def decode_capabilities(frame: bytes) -> dict[str, int]:
    if len(frame) != 24 or frame[0] != PROTOCOL_VERSION:
        raise ValueError("invalid ost-ble/1 capabilities frame")
    expected = {
        "operation_bitmap": 0xFF,
        "security_bitmap": 0xFF,
        "command_frame_bytes": 32,
        "result_frame_bytes": 20,
        "status_frame_bytes": 20,
        "event_frame_bytes": 20,
        "system_information_frame_bytes": 44,
        "command_mac_bytes": 8,
        "authentication_mac_bytes": 14,
    }
    decoded = {
        "operation_bitmap": frame[1],
        "security_bitmap": frame[2],
        "connection_limit": frame[3],
        "maximum_duration_ms": struct.unpack_from("<I", frame, 4)[0],
        "status_notification_ms": struct.unpack_from("<I", frame, 8)[0],
        "pairing_window_ms": struct.unpack_from("<I", frame, 12)[0],
        "command_frame_bytes": frame[16],
        "result_frame_bytes": frame[17],
        "status_frame_bytes": frame[18],
        "event_history_entries": frame[19],
        "event_frame_bytes": frame[20],
        "system_information_frame_bytes": frame[21],
        "command_mac_bytes": frame[22],
        "authentication_mac_bytes": frame[23],
    }
    if any(decoded[name] != value for name, value in expected.items()):
        raise ValueError("incompatible ost-ble/1 capability contract")
    return decoded


def decode_live(frame: bytes) -> dict[str, int | bool | str]:
    if len(frame) != 20 or frame[0] != PROTOCOL_VERSION:
        raise ValueError("invalid ost-ble/1 live-status frame")
    remaining, duration, revision, session = struct.unpack_from("<IIII", frame, 4)
    return {
        "state": STATES.get(frame[1], f"unknown_{frame[1]}"),
        "controls_locked": bool(frame[2] & 0x01),
        "emergency_input_active": bool(frame[2] & 0x02),
        "lock_output_energized": bool(frame[2] & 0x04),
        "release_reason": RELEASE_REASONS.get(frame[3], f"unknown_{frame[3]}"),
        "remaining_ms": remaining, "session_duration_ms": duration,
        "event_revision": revision, "session_sequence": session,
    }


def decode_system_info(frame: bytes) -> dict[str, int | bool | str]:
    if len(frame) != 44 or frame[0] != PROTOCOL_VERSION:
        raise ValueError("invalid ost-ble/1 system-information frame")
    (uptime, free_heap, largest_block, minimum_heap, sketch_size,
     ota_space, flash_size) = struct.unpack_from("<IIIIIII", frame, 4)
    flags = frame[3]
    station_connected = bool(flags & 0x01)
    station_ip = ".".join(str(value) for value in frame[32:36])
    return {
        "reset_reason": RESET_REASONS.get(frame[1], f"unknown_{frame[1]}"),
        "wifi_state": WIFI_STATES.get(frame[2], f"unknown_{frame[2]}"),
        "station_connected": station_connected,
        "ap_running": bool(flags & 0x02),
        "standalone_mode": bool(flags & 0x04),
        "station_retry_paused": bool(flags & 0x08),
        "bluetooth_connected": bool(flags & 0x10),
        "bluetooth_authenticated": bool(flags & 0x20),
        "uptime_ms": uptime,
        "free_heap_bytes": free_heap,
        "largest_free_block_bytes": largest_block,
        "minimum_free_heap_bytes": minimum_heap,
        "sketch_size_bytes": sketch_size,
        "free_sketch_space_bytes": ota_space,
        "flash_size_bytes": flash_size,
        "station_ip": station_ip if station_connected else "",
        "wifi_rssi_dbm": struct.unpack_from("<b", frame, 36)[0],
        "ap_clients": frame[37],
        "ap_client_limit": frame[38],
        "heap_fragmentation_percent": frame[39],
        "bluetooth_mtu": struct.unpack_from("<H", frame, 40)[0],
        "bluetooth_rssi_dbm": struct.unpack_from("<b", frame, 42)[0],
    }


def decode_event(frame: bytes) -> dict[str, int | bool | str]:
    if len(frame) != 20 or frame[0] != PROTOCOL_VERSION:
        raise ValueError("invalid ost-ble/1 event frame")
    event_id, at_ms, value_ms, session = struct.unpack_from("<IIiI", frame, 4)
    return {
        "event_type": frame[1],
        "state": STATES.get(frame[2], f"unknown_{frame[2]}"),
        "controls_locked": bool(frame[3] & 0x01),
        "emergency_input_active": bool(frame[3] & 0x02),
        "history_truncated": bool(frame[3] & 0x04),
        "event_id": event_id, "at_ms": at_ms, "value_ms": value_ms,
        "session_sequence": session,
    }


def decode_result(frame: bytes) -> dict[str, int | bool | str]:
    if len(frame) != 20 or frame[0] != PROTOCOL_VERSION:
        raise ValueError("invalid ost-ble/1 control-result frame")
    boot_id, session, remaining = struct.unpack_from("<III", frame, 8)
    return {
        "opcode": frame[1], "request_id": struct.unpack_from("<H", frame, 2)[0],
        "result": RESULTS.get(frame[4], f"unknown_{frame[4]}"),
        "changed": bool(frame[5] & 0x01), "duplicate": bool(frame[5] & 0x02),
        "state": STATES.get(frame[6], f"unknown_{frame[6]}"),
        "release_reason": RELEASE_REASONS.get(frame[7], f"unknown_{frame[7]}"),
        "boot_id": boot_id, "session_sequence": session,
        "remaining_ms": remaining,
    }


async def find_timer(name: str | None, address: str | None):
    if address:
        return address
    wanted = name.casefold() if name else None

    def matches(device, advertisement) -> bool:
        services = {value.casefold() for value in advertisement.service_uuids}
        visible_name = (advertisement.local_name or device.name or "").casefold()
        return SERVICE_UUID in services and (wanted is None or visible_name == wanted)

    device = await BleakScanner.find_device_by_filter(matches, timeout=12.0)
    if device is None:
        raise RuntimeError("OpenSmartTimer V2 not found; enable Bluetooth or use physical pairing")
    return device


async def request_link_pairing(client: BleakClient) -> None:
    pair = getattr(client, "pair", None)
    if pair is None:
        return
    try:
        await pair()
    except (NotImplementedError, AttributeError):
        # macOS pairs automatically when an encrypted characteristic is read.
        return


def persist_pending_key(path: Path, key: bytes) -> Path:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + ".pending")
    descriptor = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="ascii") as output:
        output.write(key.hex() + "\n")
        output.flush()
        os.fsync(output.fileno())
    return pending


def pending_key_path(path: Path) -> Path:
    path = path.expanduser().resolve()
    return path.with_name(path.name + ".pending")


def read_key_file(path: Path) -> bytes:
    try:
        key = bytes.fromhex(path.expanduser().read_text(encoding="ascii").strip())
    except (OSError, ValueError) as error:
        raise RuntimeError(f"invalid Bluetooth key file: {path}") from error
    if len(key) != 16:
        raise RuntimeError(f"Bluetooth key file is not 16 bytes: {path}")
    return key


def promote_pending_key(key_path: Path, pending_path: Path) -> None:
    destination = key_path.expanduser().resolve()
    os.replace(pending_path, destination)
    os.chmod(destination, 0o600)


async def provision_physical(client: BleakClient, key_path: Path, nickname: str | None = None) -> bytes:
    encoded_name = nickname.strip().encode("utf-8") if nickname is not None else None
    if encoded_name is not None and (not 1 <= len(encoded_name) <= 24 or
                                     any(b < 32 or b == 127 for b in encoded_name)):
        raise ValueError("nickname must be 1–24 UTF-8 bytes without control characters")
    await request_link_pairing(client)
    info = json.loads(bytes(await client.read_gatt_char(PROTOCOL_INFO_UUID)).decode())
    boot_id = int(info["boot_id"], 16)
    await asyncio.to_thread(
        input,
        "When the blue LED is pulsing and the app is connected, briefly "
        "press Reset, then press Enter here: ",
    )
    key = bytes(await client.read_gatt_char(PAIRING_SECRET_UUID))
    if len(key) != 16:
        raise RuntimeError("timer did not return a 16-byte pairing key")
    if encoded_name is not None:
        if not info.get("pairing_nickname"):
            raise RuntimeError("this firmware does not support pairing nicknames")
        record = bytes(await client.read_gatt_char(PAIRING_NICKNAME_UUID))
        if len(record) < 2 or record[0] != 1 or not record[1] & 2:
            raise RuntimeError("nickname pairing window is not open")
        if not record[1] & 1:  # An explicit name from local settings takes priority.
            await client.write_gatt_char(PAIRING_NICKNAME_UUID, encoded_name, response=True)
            accepted = bytes(await client.read_gatt_char(PAIRING_NICKNAME_UUID))
            if accepted[2:] != encoded_name:
                raise RuntimeError("timer did not accept the nickname")
    # Keep the prior controller file intact until the device accepts proof that
    # this key was durably stored. A crash leaves the .pending file available
    # for explicit recovery instead of silently destroying both credentials.
    pending_path = persist_pending_key(key_path, key)
    nonce = secrets.token_bytes(4)
    tag = hmac.new(
        key,
        b"OST-PAIR-COMMIT-v1" + struct.pack("<I", boot_id) + nonce,
        hashlib.sha256,
    ).digest()[:14]
    commit = bytes((PROTOCOL_VERSION, 0x02)) + nonce + tag
    await client.write_gatt_char(CONTROL_COMMAND_UUID, commit, response=True)
    for _ in range(30):
        challenge = bytes(await client.read_gatt_char(CONTROL_CHALLENGE_UUID))
        if len(challenge) == 20 and challenge[1] & 0x04:
            # A commit acknowledgement proves durable device storage, but not
            # that both sides derived the same authority. Authenticate with the
            # pending key before replacing the last known-good local key.
            await authenticate(client, key)
            promote_pending_key(key_path, pending_path)
            return key
        await asyncio.sleep(0.1)
    raise RuntimeError(
        f"timer did not confirm pairing; prior key is unchanged and pending key is at {pending_path}"
    )


async def recover_pending_key(target, key_path: Path) -> bool:
    """Prove a pending key after a lost commit response or disconnect."""
    pending_path = pending_key_path(key_path)
    if not pending_path.is_file():
        return False
    key = read_key_file(pending_path)
    # Two reconnect attempts tolerate a platform BLE race without consuming
    # the device's five-failure budget through an unbounded retry loop.
    for attempt in range(2):
        try:
            async with BleakClient(target) as client:
                await authenticate(client, key)
            promote_pending_key(key_path, pending_path)
            return True
        except Exception:
            if attempt == 0:
                await asyncio.sleep(0.75)
    return False


async def authenticate(client: BleakClient, key: bytes) -> tuple[int, bytes]:
    await request_link_pairing(client)
    challenge = bytes(await client.read_gatt_char(CONTROL_CHALLENGE_UUID))
    if len(challenge) != 20 or challenge[0] != PROTOCOL_VERSION:
        raise RuntimeError("timer returned an invalid control challenge")
    if not challenge[1] & 0x01:
        raise RuntimeError("no controller is paired with this timer")
    boot_id = struct.unpack_from("<I", challenge, 4)[0]
    client_nonce = secrets.token_bytes(4)
    context = struct.pack("<I", boot_id) + challenge[8:20] + client_nonce
    tag = hmac.new(key, b"OST-AUTH-v1" + context, hashlib.sha256).digest()[:14]
    auth_frame = bytes((PROTOCOL_VERSION, 0x01)) + client_nonce + tag
    await client.write_gatt_char(CONTROL_COMMAND_UUID, auth_frame, response=True)
    confirmed = bytes(await client.read_gatt_char(CONTROL_CHALLENGE_UUID))
    if len(confirmed) != 20 or not confirmed[1] & 0x02:
        raise RuntimeError("timer did not accept the Bluetooth pairing key")
    session_key = hmac.new(
        key, b"OST-SESSION-v1" + context, hashlib.sha256
    ).digest()
    return boot_id, session_key


def build_command(action: str, request_id: int, boot_id: int,
                  session: int, counter: int, args, session_key: bytes) -> bytes:
    value_a = 0
    value_b = 0
    if action == "start":
        value_a = args.duration_ms
        session = 0
    elif action == "random":
        value_a = args.min_ms
        value_b = args.max_ms
        session = 0
    elif action == "adjust":
        value_a = args.delta_ms
    prefix = struct.pack(
        "<BBHIIiiI", PROTOCOL_VERSION, OPCODES[action], request_id,
        boot_id, session, value_a, value_b, counter,
    )
    tag = hmac.new(session_key, b"OST-CMD-v1" + prefix, hashlib.sha256).digest()[:8]
    return prefix + tag


def read_key(args) -> bytes:
    key_text = args.key or os.environ.get("OST_BLE_KEY")
    if not key_text and args.key_file:
        key_text = Path(args.key_file).expanduser().read_text(encoding="ascii")
    if not key_text:
        key_text = getpass.getpass("Saved Bluetooth pairing key: ")
    try:
        key = bytes.fromhex(key_text.strip())
    except ValueError as error:
        raise SystemExit("pairing key must be hexadecimal") from error
    if len(key) != 16:
        raise SystemExit("pairing key must contain exactly 32 hexadecimal characters")
    return key


async def watch_connection(client: BleakClient, seconds: float) -> None:
    """Keep a notification-only observer alive without changing the timer."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + max(0, seconds)
    while loop.time() < deadline:
        await asyncio.sleep(min(60, deadline - loop.time()))
        if not client.is_connected:
            raise RuntimeError("Bluetooth disconnected while watching; reconnect to confirm timer status")
        if loop.time() < deadline:
            await client.read_gatt_char(LIVE_STATUS_UUID)


async def run(args) -> None:
    target = await find_timer(args.name, args.address)
    if args.action == "pair":
        if not args.key_file:
            raise SystemExit("pair requires --key-file so the key is stored before activation")
        key_path = Path(args.key_file)
        pending_path = pending_key_path(key_path)
        if pending_path.is_file():
            if await recover_pending_key(target, key_path):
                print(f"Recovered completed pairing; key stored at {key_path.expanduser()}")
                return
            raise RuntimeError(
                f"unverified pending pairing key remains at {pending_path}; "
                "preserve or move it before starting a replacement pairing"
            )
        try:
            async with BleakClient(target) as client:
                await provision_physical(client, key_path, args.nickname)
        except Exception as error:
            if await recover_pending_key(target, key_path):
                print(f"Recovered completed pairing; key stored at {key_path.expanduser()}")
                return
            raise RuntimeError(
                f"pairing did not complete; the prior key is unchanged and any "
                f"pending key remains at {pending_key_path(key_path)}"
            ) from error
        print(f"Pairing complete; key stored at {key_path.expanduser()}")
        return

    async with BleakClient(target) as client:
        if args.action == "info":
            info = bytes(await client.read_gatt_char(PROTOCOL_INFO_UUID)).decode()
            capabilities = bytes(await client.read_gatt_char(CAPABILITIES_UUID))
            print(json.dumps(json.loads(info), indent=2))
            print("capabilities:", json.dumps(decode_capabilities(capabilities), indent=2))
            return
        key = read_key(args)
        boot_id, session_key = await authenticate(client, key)
        live = decode_live(bytes(await client.read_gatt_char(LIVE_STATUS_UUID)))
        print("status:", json.dumps(live, indent=2))
        if args.action == "status":
            system = decode_system_info(
                bytes(await client.read_gatt_char(SYSTEM_INFO_UUID))
            )
            print("system:", json.dumps(system, indent=2))
            return
        if args.action == "watch":
            def on_status(_characteristic, data: bytearray) -> None:
                print("status:", json.dumps(decode_live(bytes(data))))

            def on_event(_characteristic, data: bytearray) -> None:
                print("event:", json.dumps(decode_event(bytes(data))))

            await client.start_notify(LIVE_STATUS_UUID, on_status)
            await client.start_notify(EVENT_UUID, on_event)
            await watch_connection(client, args.seconds)
            return

        request_id = secrets.randbelow(65535) + 1
        command = build_command(
            args.action, request_id, boot_id,
            int(live["session_sequence"]), 1, args, session_key,
        )
        loop = asyncio.get_running_loop()
        response = loop.create_future()

        def on_result(_characteristic, data: bytearray) -> None:
            raw = bytes(data)
            if (len(raw) == 20 and
                    struct.unpack_from("<H", raw, 2)[0] == request_id and
                    not response.done()):
                response.set_result(raw)

        await client.start_notify(CONTROL_RESULT_UUID, on_result)
        await client.write_gatt_char(CONTROL_COMMAND_UUID, command, response=True)
        try:
            raw_result = await asyncio.wait_for(response, timeout=3.0)
        except TimeoutError:
            raw_result = bytes(await client.read_gatt_char(CONTROL_RESULT_UUID))
        finally:
            await client.stop_notify(CONTROL_RESULT_UUID)
        result = decode_result(raw_result)
        print("result:", json.dumps(result, indent=2))
        if result["request_id"] != request_id:
            raise RuntimeError("cached result does not match this request")
        if result["result"] != "applied":
            raise RuntimeError(f"timer rejected command: {result['result']}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Pair with or control an OpenSmartTimer V2 over BLE")
    target = result.add_mutually_exclusive_group()
    target.add_argument("--name", help="exact advertised name, such as OST-8481b0")
    target.add_argument("--address", help="platform-specific BLE address/identifier")
    result.add_argument("--key", help="32-hex saved pairing key; prompt or OST_BLE_KEY is safer")
    result.add_argument("--key-file", help="key file; required for pairing and accepted for control")
    commands = result.add_subparsers(dest="action", required=True)
    commands.add_parser("info", help="read public protocol and capability data")
    pair = commands.add_parser("pair", help="finish a Reset-button physical pairing")
    pair.add_argument("--nickname", help="label this phone/computer (up to 24 UTF-8 bytes)")
    commands.add_parser("status")
    watch = commands.add_parser("watch")
    watch.add_argument("--seconds", type=int, default=60)
    fixed = commands.add_parser("start")
    fixed.add_argument("--duration-ms", type=int, required=True)
    random_start = commands.add_parser("random")
    random_start.add_argument("--min-ms", type=int, required=True)
    random_start.add_argument("--max-ms", type=int, required=True)
    commands.add_parser("release")
    commands.add_parser("pause")
    commands.add_parser("resume")
    adjust = commands.add_parser("adjust")
    adjust.add_argument("--delta-ms", type=int, required=True)
    commands.add_parser("lock-controls")
    commands.add_parser("identify")
    return result


if __name__ == "__main__":
    asyncio.run(run(parser().parse_args()))
