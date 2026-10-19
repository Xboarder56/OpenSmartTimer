"""Structural parse of wire capability tokens.

The relay never verifies the HMAC (only the timer holds
capability_key), so these tests exercise only the parse + reject
paths that the session-mint endpoint depends on.
"""

from __future__ import annotations

import base64
import json

import pytest


def test_parse_ok_returns_payload(wire_token_factory):
    from ostrelay.session import parse_wire_token
    wire, _ = wire_token_factory(scopes=["view", "pause_resume"])
    payload = parse_wire_token(wire)
    assert payload["aud"] == "ost-relay/1"
    assert payload["sc"] == ["view", "pause_resume"]


def test_parse_rejects_wrong_prefix():
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as exc:
        parse_wire_token("ost-cap.v1.aaaa.bbbb")
    assert exc.value.detail == "malformed_token"


def test_parse_rejects_wrong_segment_count():
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as exc:
        parse_wire_token("ost-cap.v1.only-two-segments")
    assert exc.value.detail == "malformed_token"


def test_parse_rejects_bad_base64(wire_token_factory):
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    with pytest.raises(HTTPException):
        parse_wire_token("ost-cap.v1.$$$.$$$")


def test_parse_rejects_wrong_audience():
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    payload = {"aud": "other", "tid": "x", "fp": _b64(b"\x00" * 16),
               "sc": ["view"], "boot": "aa", "exp_up_ms": 1, "mint_up_ms": 0}
    body = base64.urlsafe_b64encode(
        json.dumps(payload).encode()).rstrip(b"=").decode()
    wire = f"ost-cap.v1.{body}.{_b64(b'\\x00' * 16)}"
    with pytest.raises(HTTPException):
        parse_wire_token(wire)


def test_parse_rejects_empty_scope_array():
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    payload = {"aud": "ost-relay/1", "tid": "x", "fp": _b64(b"\x00" * 16),
               "sc": [], "boot": "aa", "exp_up_ms": 1, "mint_up_ms": 0}
    body = base64.urlsafe_b64encode(
        json.dumps(payload).encode()).rstrip(b"=").decode()
    wire = f"ost-cap.v1.{body}.{_b64(b'\\x00' * 16)}"
    with pytest.raises(HTTPException):
        parse_wire_token(wire)


def test_parse_requires_view_scope():
    from ostrelay.session import parse_wire_token
    from fastapi import HTTPException
    payload = {"aud": "ost-relay/1", "tid": "x", "fp": _b64(b"\x00" * 16),
               "sc": ["pause_resume"], "boot": "aa",
               "exp_up_ms": 1, "mint_up_ms": 0}
    body = base64.urlsafe_b64encode(
        json.dumps(payload).encode()).rstrip(b"=").decode()
    wire = f"ost-cap.v1.{body}.{_b64(b'\\x00' * 16)}"
    with pytest.raises(HTTPException):
        parse_wire_token(wire)


def test_fingerprint_hash_is_deterministic(wire_token_factory):
    from ostrelay.session import fingerprint_hash
    _, fp_hash1 = wire_token_factory(scopes=["view"])
    _, fp_hash2 = wire_token_factory(scopes=["view"])
    # Different fp bytes → different hashes.
    assert fp_hash1 != fp_hash2
    # Same input → same hash.
    import base64, hashlib
    fp = base64.urlsafe_b64decode("AAAAAAAAAAAAAAAAAAAAAA==")
    expected = base64.urlsafe_b64encode(
        hashlib.sha256(fp).digest()[:16]).rstrip(b"=").decode()
    assert fingerprint_hash("AAAAAAAAAAAAAAAAAAAAAA") == expected


def _b64(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()
