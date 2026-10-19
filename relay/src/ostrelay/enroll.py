"""Timer enrollment (HTTPS).

The ``enrollment_code`` a timer sends is one of: an owner code (reusable,
``OSTRELAY_ENROLLMENT`` includes ``owner_codes``), the public open-enrollment
code (``open``), or a single-use operator code from ``/admin/enroll-code``
(``410 code_consumed`` on reuse). See ostrelay/owners.py.
"""

from __future__ import annotations

import base64
import hashlib
import secrets
import time
from collections import deque

from fastapi import APIRouter, HTTPException, Request

from . import db, owners, site_settings, tlsid
from .config import settings
from .models import EnrollRequest, EnrollResponse

router = APIRouter()

# Enrollment codes expire after this many seconds. Kept in sync with
# admin.ENROLLMENT_CODE_TTL_S; both modules ultimately want the same
# number.
ENROLLMENT_CODE_TTL_S = 15 * 60


def _spki_pin() -> str:
    """Pin of the TLS key this relay serves; empty while unknown or without TLS."""
    return tlsid.current_pin()


_open_buckets: dict[str, deque[float]] = {}
_enroll_buckets: dict[str, deque[float]] = {}


def _limit(buckets: dict[str, deque[float]], request: Request, per_hour: int, detail: str) -> None:
    now = time.monotonic()
    for key, bucket in list(buckets.items()):
        while bucket and bucket[0] <= now - 3600:
            bucket.popleft()
        if not bucket:
            del buckets[key]
    ip = request.client.host if request.client else "unknown"
    if ip not in buckets and len(buckets) >= settings().max_rate_limit_keys:
        raise HTTPException(429, "rate_limit_capacity")
    bucket = buckets.setdefault(ip, deque())
    if len(bucket) >= per_hour:
        raise HTTPException(429, detail)
    bucket.append(now)


@router.post("/api/v1/enroll", response_model=EnrollResponse)
def enroll(body: EnrollRequest, request: Request) -> EnrollResponse:
    cfg = settings()
    # Certificate discovery can still be pending after startup. Do not consume
    # a code or enrollment capacity for a reply that the timer cannot accept.
    # Snapshot once so a concurrent refresh cannot change the returned pin.
    pin = _spki_pin()
    if not pin and not (cfg.dev_mode and cfg.public_url.startswith("http://")):
        raise HTTPException(503, "tls_identity_unavailable", headers={"Retry-After": "15"})
    modes = cfg.enrollment_modes()
    code = body.enrollment_code
    now = db.now_unix()
    _limit(_enroll_buckets, request, cfg.rate_enrollments_per_ip_1h, "rate_limited_enrollment")
    if code.startswith(owners.OWNER_CODE_PREFIX):
        if "owner_codes" not in modes:
            raise HTTPException(403, "enrollment_denied")
        via = "owner"
    elif code == owners.OPEN_ENROLLMENT_CODE:
        if "open" not in modes:
            raise HTTPException(403, "enrollment_denied")
        _limit(_open_buckets, request, cfg.rate_open_enrollments_per_ip_1h, "rate_limited_enrollment")
        via = "open"
    else:
        via = "code"

    secret = secrets.token_bytes(32)
    with db.transaction() as tx:
        if via == "open" and not site_settings.open_enrollment_settings(tx)["open_enrollment_enabled"]:
            raise HTTPException(403, "enrollment_denied")
        owner_id = None
        if via == "owner":
            # Checked in the same transaction that writes the timer: a revoke
            # or rotate either happened before (refused) or waits for this.
            owner_id = owners.owner_for_code(code, tx)
            if owner_id is None:
                raise HTTPException(403, "enrollment_denied")
        if tx.execute("SELECT count(*) AS n FROM timers").fetchone()["n"] >= cfg.max_enrolled_timers:
            raise HTTPException(409, "relay_full")
        if owner_id is not None and tx.execute(
                "SELECT count(*) AS n FROM timers WHERE owner_id=?", (owner_id,)).fetchone()["n"] >= cfg.max_timers_per_owner:
            raise HTTPException(409, "owner_timer_limit")
        if via != "code":
            # A reusable code enrolls many timers: each gets a fresh identity.
            timer_id = base64.urlsafe_b64encode(secrets.token_bytes(12)).rstrip(b"=").decode("ascii")
        else:
            row = tx.execute(
                "SELECT code, created_at, consumed_at FROM enrollment_codes WHERE code = ?",
                (code,),
            ).fetchone()
            if row is None:
                raise HTTPException(403, "enrollment_denied")
            if row["consumed_at"] is not None:
                raise HTTPException(410, "code_consumed")
            # Enforce the TTL at consume time, not only by admin-mint pruning.
            if int(row["created_at"]) + ENROLLMENT_CODE_TTL_S < now:
                raise HTTPException(410, "code_expired")
            # Deterministic-ish timer_id: hash of the single-use code. Not a secret.
            timer_id = base64.urlsafe_b64encode(
                hashlib.sha256(code.encode("utf-8")).digest()[:12]
            ).rstrip(b"=").decode("ascii")
            tx.execute("UPDATE enrollment_codes SET consumed_at = ? WHERE code = ?", (now, code))
        tx.execute(
            """
            INSERT OR REPLACE INTO timers
            (timer_id, enrollment_secret, hardware_model, chip_family,
             build_id, spki_sha256_pin, label, created_at, owner_id, enrolled_via)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (timer_id, secret, body.hardware_model, body.chip_family,
             body.build_id, pin, body.device_name.strip(), now, owner_id, via),
        )

    return EnrollResponse(
        timer_id=timer_id,
        enrollment_secret=base64.urlsafe_b64encode(secret).rstrip(b"=").decode("ascii"),
        wss_endpoint=f"{'wss' if cfg.public_url.startswith('https:') else 'ws'}:{cfg.public_url.split(':', 1)[1]}/api/v1/timer",
        spki_sha256_pin=pin,
    )
