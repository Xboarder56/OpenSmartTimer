#!/usr/bin/env python3
"""Reference encoder and golden vectors for the ost-ble/1 management extension.

Run with --write to regenerate tests/vectors/ost_ble_mgmt_v1.json (needs the
`cryptography` package for AES-128-CTR). Without --write the published vectors
are checked: every HMAC with the standard library, and AES when `cryptography`
is installed. See docs/BLUETOOTH_MANAGEMENT.md.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VECTOR_PATH = ROOT / "tests" / "vectors" / "ost_ble_mgmt_v1.json"

SESSION_KEY = bytes(range(0x20, 0x40))
BOOT_ID = 0x7CA9F21B
REQUEST_LABEL = b"OST-MGMT-v1"
RESPONSE_LABEL = b"OST-MGMT-RSP-v1"
ENCRYPTION_LABEL = b"OST-MGMT-ENC-v1"
MAX_SEGMENT = 512 - 12 - 16


def mac(*parts: bytes) -> bytes:
    return hmac.new(SESSION_KEY, b"".join(parts), hashlib.sha256).digest()[:16]


def encryption_key() -> bytes:
    return hmac.new(SESSION_KEY, ENCRYPTION_LABEL, hashlib.sha256).digest()[:16]


def secret_iv(counter: int, request_id: int) -> bytes:
    return struct.pack("<IH", counter, request_id) + bytes(10)


def aes_ctr(key: bytes, iv: bytes, data: bytes) -> bytes:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    encryptor = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    return encryptor.update(data) + encryptor.finalize()


def request_frame(opcode: int, request_id: int, counter: int, body: bytes,
                  ciphertext: bytes = b"") -> bytes:
    signed = (bytes([1, opcode]) + struct.pack("<HIIHB", request_id, BOOT_ID, counter,
                                              len(body), len(ciphertext)) + body + ciphertext)
    return signed + mac(REQUEST_LABEL, signed)


def fragments(frame: bytes, payload: int) -> list[str]:
    pieces = [frame[i:i + payload] for i in range(0, len(frame), payload)]
    return [(bytes([index, 1 if index == len(pieces) - 1 else 0]) + piece).hex()
            for index, piece in enumerate(pieces)]


def response_segment(opcode: int, request_id: int, status: int, body: bytes,
                     offset: int, counter: int) -> bytes:
    segment = body[offset:offset + MAX_SEGMENT]
    value = bytes([1, opcode]) + struct.pack("<HHHHH", request_id, status, len(body),
                                             offset, len(segment)) + segment
    return value + mac(RESPONSE_LABEL, struct.pack("<I", counter), value)


def build(encrypt) -> dict:
    wifi_body = b'{"ssid":"Home Wi-Fi"}'
    password = b"correct horse battery staple"
    counter, request_id = 41, 0x1234
    iv = secret_iv(counter, request_id)
    ciphertext = encrypt(encryption_key(), iv, password)
    wifi_frame = request_frame(0x50, request_id, counter, wifi_body, ciphertext)

    status_body = json.dumps(
        {"state": "connected", "network_mode": "wifi", "saved_ssid": "Home Wi-Fi",
         "note": "x" * 600}, separators=(",", ":")).encode()
    read_counter, read_id = 42, 0x1235
    read_frame = request_frame(0x40, read_id, read_counter, b"{}")
    continue_counter = 43
    continue_body = json.dumps({"request_id": read_id, "offset": MAX_SEGMENT},
                               separators=(",", ":")).encode()
    continue_frame = request_frame(0x3F, 0x1236, continue_counter, continue_body)

    return {
        "session_key_hex": SESSION_KEY.hex(),
        "boot_id": BOOT_ID,
        "encryption_key_hex": encryption_key().hex(),
        "wifi_configure": {
            "opcode": 0x50, "request_id": request_id, "counter": counter,
            "body": wifi_body.decode(), "password": password.decode(),
            "iv_hex": iv.hex(), "secret_ciphertext_hex": ciphertext.hex(),
            "frame_hex": wifi_frame.hex(),
            "fragments_mtu23_hex": fragments(wifi_frame, 18),
            "fragments_mtu247_hex": fragments(wifi_frame, 242),
        },
        "wifi_status": {
            "opcode": 0x40, "request_id": read_id, "counter": read_counter,
            "frame_hex": read_frame.hex(), "status": 200, "body": status_body.decode(),
            "ready_hex": (bytes([1, 0x40]) + struct.pack("<HHH", read_id, 200, len(status_body))).hex(),
            "segment0_hex": response_segment(0x40, read_id, 200, status_body, 0, read_counter).hex(),
            "continue_counter": continue_counter,
            "continue_frame_hex": continue_frame.hex(),
            "segment1_hex": response_segment(0x40, read_id, 200, status_body, MAX_SEGMENT,
                                             continue_counter).hex(),
        },
    }


def main() -> int:
    if "--write" in sys.argv:
        VECTOR_PATH.write_text(json.dumps(build(aes_ctr), indent=2) + "\n", encoding="utf-8")
        print(f"wrote {VECTOR_PATH.relative_to(ROOT)}")
        return 0
    published = json.loads(VECTOR_PATH.read_text(encoding="utf-8"))
    ciphertext = bytes.fromhex(published["wifi_configure"]["secret_ciphertext_hex"])
    # Rebuild everything with the published ciphertext; MACs need only hashlib.
    rebuilt = build(lambda key, iv, data: ciphertext)
    assert rebuilt == published, "management vectors drifted from the reference encoder"
    try:
        wifi = published["wifi_configure"]
        assert aes_ctr(bytes.fromhex(published["encryption_key_hex"]), bytes.fromhex(wifi["iv_hex"]),
                       wifi["password"].encode()) == ciphertext
        aes_note = "AES-128-CTR checked"
    except ImportError:
        aes_note = "AES-128-CTR not checked (install cryptography)"
    print(f"ost-ble/1 management vectors passed; {aes_note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
