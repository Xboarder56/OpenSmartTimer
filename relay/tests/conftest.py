"""Shared pytest fixtures for the ostrelay test suite.

Every test gets a fresh in-memory SQLite database and a fresh
in-process WsRegistry, so ordering never leaks state between tests.

The ``client`` fixture returns a synchronous FastAPI TestClient
suitable for HTTP endpoints. WebSocket-driven tests use
``ws_client`` (also from TestClient — the httpx integration handles
WSS handshake, minus TLS).
"""

from __future__ import annotations

import os
import secrets
import sys
from pathlib import Path

import pytest

# Make the src/ layout importable without pip install -e.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))
sys.path.insert(0, str(_HERE.parent / "mock-timer"))

# Test processes must never inherit an operator database or credentials.
# This edits only pytest's environment, not the caller's shell environment.
for name in list(os.environ):
    if name.startswith("OSTRELAY_"):
        del os.environ[name]
os.environ.update(OSTRELAY_ADMIN_TOKEN="test-admin-token-" + "0" * 24,
                  OSTRELAY_COOKIE_SECRET="00" * 32,
                  OSTRELAY_PUBLIC_URL="http://testserver",
                  OSTRELAY_DB_PATH=":memory:", OSTRELAY_DEV_MODE="true")


@pytest.fixture()
def fresh_settings():
    """Clear any cached Settings so each test picks up env changes."""
    from ostrelay import config
    config._settings = config.Settings(_env_file=None)
    from ostrelay.session import _create_buckets
    from ostrelay.command import _reset_buckets_for_tests
    _create_buckets.clear()
    _reset_buckets_for_tests()
    from ostrelay import enroll
    enroll._enroll_buckets.clear()
    enroll._open_buckets.clear()
    yield
    config._settings = None


@pytest.fixture()
def fresh_db(fresh_settings, monkeypatch):
    """Fresh in-memory SQLite."""
    from ostrelay import db as db_mod
    # Force a new connection.
    monkeypatch.setattr(db_mod, "_conn", None)
    db_mod.connect()
    yield db_mod
    db_mod.connect().close()


@pytest.fixture()
def fresh_registry(monkeypatch):
    """Empty in-process WsRegistry per test."""
    from ostrelay import ws as ws_mod
    monkeypatch.setattr(ws_mod, "registry", ws_mod.WsRegistry())
    yield ws_mod.registry


@pytest.fixture()
def app(fresh_db, fresh_registry):
    from ostrelay.main import create_app
    return create_app()


@pytest.fixture()
def client(app):
    from fastapi.testclient import TestClient
    # https:// so the Secure cookie set on /api/v1/session is honored
    # by the underlying httpx cookie jar. Real deployments always
    # terminate TLS in front (OPERATOR.md).
    with TestClient(app, base_url="https://testserver") as c:
        yield c


@pytest.fixture()
def admin_headers():
    return {"Authorization": f"Bearer {os.environ['OSTRELAY_ADMIN_TOKEN']}"}


# ---------------------------------------------------------------------------
# Helpers reused by multiple tests
# ---------------------------------------------------------------------------

@pytest.fixture()
def wire_token_factory():
    """Build a plausible wire token for tests that only exercise parse
    or session flow. Not signed — the timer is the only party that
    signs — but shape-valid.
    """
    import base64
    import hashlib
    import json

    def make(scopes: list[str], boot_id: str = "aabbccdd",
             timer_id: str = "timer_test_a",
             exp_up_ms: int = 3_600_000,
             fp: bytes | None = None,
             tag: bytes | None = None) -> tuple[str, str]:
        if fp is None:
            fp = secrets.token_bytes(16)
        if tag is None:
            tag = secrets.token_bytes(16)
        payload = {
            "aud": "ost-relay/1",
            "tid": timer_id,
            "fp": _b64url(fp),
            "sc": scopes,
            "boot": boot_id,
            "exp_up_ms": exp_up_ms,
            "mint_up_ms": 0,
            "vk": _b64url(bytes.fromhex("036b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296")),  # P-256 generator
        }
        payload_bytes = json.dumps(payload, separators=(",", ":")).encode()
        wire = f"ost-cap.v1.{_b64url(payload_bytes)}.{_b64url(tag)}"
        fp_hash = _b64url(hashlib.sha256(fp).digest()[:16])
        return wire, fp_hash

    def _b64url(b: bytes) -> str:
        return base64.urlsafe_b64encode(b).rstrip(b"=").decode()

    return make
