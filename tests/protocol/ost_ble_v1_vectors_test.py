#!/usr/bin/env python3
"""Verify the published ost-ble/1 crypto and binary golden vectors."""

from __future__ import annotations

import hashlib
import hmac
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads(
    (ROOT / "tests" / "vectors" / "ost_ble_v1.json").read_text(encoding="utf-8")
)


def main() -> int:
    key = bytes.fromhex(VECTORS["app_key_hex"])
    boot = VECTORS["boot_id"]
    challenge = bytes.fromhex(VECTORS["challenge_hex"])
    nonce = bytes.fromhex(VECTORS["authentication_client_nonce_hex"])
    payload = struct.pack("<I", boot) + challenge + nonce

    authentication = bytes([1, 1]) + nonce + hmac.new(
        key, b"OST-AUTH-v1" + payload, hashlib.sha256
    ).digest()[:14]
    assert authentication.hex() == VECTORS["authentication_frame_hex"]

    session_key = hmac.new(
        key, b"OST-SESSION-v1" + payload, hashlib.sha256
    ).digest()
    assert session_key.hex() == VECTORS["session_key_hex"]

    command = bytearray.fromhex(VECTORS["start_command_frame_hex"])
    assert len(command) == 32
    assert command[:2] == bytes([1, 0x10])
    assert struct.unpack_from("<H", command, 2)[0] == 0x1234
    assert struct.unpack_from("<I", command, 4)[0] == boot
    expected_mac = hmac.new(
        session_key, b"OST-CMD-v1" + command[:24], hashlib.sha256
    ).digest()[:8]
    assert hmac.compare_digest(expected_mac, command[24:])

    pending_key = bytes.fromhex(VECTORS["pending_pairing_key_hex"])
    pairing_nonce = bytes.fromhex(VECTORS["pairing_commit_client_nonce_hex"])
    pairing_commit = bytes([1, 2]) + pairing_nonce + hmac.new(
        pending_key,
        b"OST-PAIR-COMMIT-v1" + struct.pack("<I", boot) + pairing_nonce,
        hashlib.sha256,
    ).digest()[:14]
    assert pairing_commit.hex() == VECTORS["pairing_commit_frame_hex"]

    capabilities = bytes.fromhex(VECTORS["capabilities_frame_hex"])
    assert len(capabilities) == 24
    assert capabilities[:4] == bytes([1, 0xFF, 0xFF, 1])
    assert struct.unpack_from("<I", capabilities, 4)[0] == 86_400_000
    assert capabilities[16:] == bytes([32, 20, 20, 16, 20, 44, 8, 14])

    event = bytes.fromhex(VECTORS["truncated_emergency_event_frame_hex"])
    assert event[1] == 10
    assert event[3] & 0x04

    print("ost-ble/1 golden vectors passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
