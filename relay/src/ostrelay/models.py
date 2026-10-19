"""Pydantic wire models for ost-relay/1.

These mirror the spec (docs/RELAY_PROTOCOL.md) but do not carry
timer-authoritative fields (results, status snapshots) that we forward
verbatim as untyped JSON — the relay is not the shape-of-truth for
those; the timer is.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator


class WireModel(BaseModel):
    model_config = ConfigDict(strict=True, extra="forbid")


# ---------------------------------------------------------------------------
# HTTPS surface — enrollment
# ---------------------------------------------------------------------------

class EnrollRequest(WireModel):
    enrollment_code: str = Field(..., min_length=8, max_length=128)
    hardware_model: str = Field(..., max_length=64)
    chip_family: str = Field(..., max_length=32)
    build_id: str = Field(..., pattern=r"^[0-9a-f]{24}$")
    device_name: str = Field(default="", max_length=32, pattern=r"^[ -~]*$")


class EnrollResponse(WireModel):
    timer_id: str
    enrollment_secret: str       # base64url 32 bytes
    wss_endpoint: str
    spki_sha256_pin: str         # base64url 32 bytes


# ---------------------------------------------------------------------------
# HTTPS surface — partner browser session exchange
# ---------------------------------------------------------------------------

class SessionRequest(WireModel):
    fp_hash: str = Field(pattern=r"^[A-Za-z0-9_-]{22}$")
    client_id: str = Field(pattern=r"^[A-Za-z0-9_-]{22}$")
    boot_id: str = Field(pattern=r"^[0-9a-f]{1,16}$")
    client_key: str = Field(pattern=r"^[A-Za-z0-9_-]{87}$")
    tag: str = Field(pattern=r"^[A-Za-z0-9_-]{43}$")


class SessionResponse(WireModel):
    session_handle: str
    timer_id: str
    boot_id: str
    frame: dict[str, Any]


class CommandRequest(WireModel):
    type: Literal["e2e"]
    fp_hash: str = Field(pattern=r"^[A-Za-z0-9_-]{22}$")
    client_id: str = Field(pattern=r"^[A-Za-z0-9_-]{22}$")
    server_id: str = Field(pattern=r"^[A-Za-z0-9_-]{22}$")
    seq: int = Field(ge=1, le=(1<<53)-1)
    request_id: str = Field(pattern=r"^[!-~]{1,64}$")
    ciphertext: str = Field(min_length=22,max_length=2422)


# ---------------------------------------------------------------------------
# WSS frames — upstream (timer → relay)
# ---------------------------------------------------------------------------

class UpHello(WireModel):
    type: Literal["hello"]
    protocol: Literal["ost-relay/1"]
    timer_id: str
    boot_id: str
    uptime_ms: int
    resume_from_upstream_seq: int = 0
    resume_from_downstream_seq: int = 0
    hello_tag: str                              # base64url 16 bytes


class UpTokenMint(WireModel):
    type: Literal["token_mint"]
    upstream_seq: int
    fp_hash: str                                # base64url 16 bytes
    token_digest: str                           # base64url SHA256(wire), 32 bytes
    scopes: list[str]
    boot_id: str
    exp_up_ms: int
    label: str = ""


class UpTokenRevoke(WireModel):
    type: Literal["token_revoke"]
    upstream_seq: int
    fp_hash: str


class UpPong(WireModel):
    type: Literal["pong"]


# ---------------------------------------------------------------------------
# WSS frames — downstream (relay → timer)
# ---------------------------------------------------------------------------

class DownHelloAck(WireModel):
    type: Literal["hello_ack"] = "hello_ack"
    session_id: str
    resume_ok: bool
    server_time_unix: int
    ack_tag: str                                # base64url 16 bytes


class DownAck(WireModel):
    type: Literal["ack"] = "ack"
    acked_upstream_seq: int


class DownPing(WireModel):
    type: Literal["ping"] = "ping"
