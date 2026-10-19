"""Admin endpoints (operator-only).

Authentication: HTTP Bearer token that MUST match
``OSTRELAY_ADMIN_TOKEN``. All routes below live under ``/admin/*``
and MUST be exposed only on a trusted network — typically bound to a
localhost socket behind the reverse proxy, or gated by nginx
``allow`` rules. Do NOT publish the admin surface to the same
hostname the SPA serves from.

Actions covered:

* ``POST /admin/enroll-code`` — mint a one-shot enrollment code the
  owner enters into the local timer UI. Codes are single-use, expire
  after 15 minutes, and are consumed by
  :mod:`relay.src.ostrelay.enroll`.
* ``GET  /admin/timers`` — list enrolled timers (id, model, last-seen).
* ``DELETE /admin/timers/{timer_id}`` — remove a timer's enrollment.
  Cascades to its tokens and sessions via foreign keys.
* ``GET  /admin/tokens/{timer_id}`` — list live capability-token
  metadata for a timer (fingerprint hashes only; wire tokens are
  never returned).
* ``POST /admin/tokens/{fp_hash}/revoke`` — server-side revoke escape
  hatch for the operator. Timer-side revoke via the local UI is the
  normal path; this exists for compromised-partner scenarios where
  the owner cannot immediately reach the timer.
"""

from __future__ import annotations

import base64
import json
import secrets
import time
import hashlib
import asyncio
from typing import Any
from typing import Literal

from fastapi import APIRouter, Depends, Header, HTTPException, status
from fastapi.responses import HTMLResponse, Response
from pydantic import BaseModel, ConfigDict, Field, StrictBool

from . import db, owners, site_settings, tlsid, ws
from .admin_page import ADMIN_JS, ADMIN_PAGE, PAGE_CSP, PAGES_CSS
from .page_strings import PAGE_STRINGS_JS
from .config import settings
from .wire import ascii_equal

router = APIRouter(prefix="/admin")


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------

def require_admin(authorization: str | None = Header(default=None)) -> None:
    """Reject unless the Authorization header carries the admin token."""
    expected = f"Bearer {settings().admin_token}"
    if not authorization or len(authorization) > 263 or not ascii_equal(authorization, expected):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "admin_token_required")


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------

class EnrollCodeResponse(BaseModel):
    enrollment_code: str
    expires_at_unix: int
    # One string for the owner to paste into the timer page: the relay's
    # public origin, this code and the certificate's SPKI pin. Empty when the
    # pin is not configured (loopback development).
    setup_code: str = ""


SETUP_CODE_PREFIX = "ost-relay-setup:"


def setup_code_for(code: str) -> str:
    """One owner-reviewed bundle; partner origin is a suggestion, never relay authority."""
    cfg = settings()
    if not tlsid.current_pin():
        return ""
    payload = json.dumps({"url": cfg.public_url, "code": code, "pin": tlsid.current_pin(),
                          "partner_origin": cfg.partner_origin},
                         separators=(",", ":")).encode()
    return SETUP_CODE_PREFIX + base64.urlsafe_b64encode(payload).decode().rstrip("=")


class TimerRow(BaseModel):
    timer_id: str
    hardware_model: str
    chip_family: str
    build_id: str
    label: str
    created_at: int
    last_seen_at: int | None
    connected_now: bool
    owner: str = ""


class TokenRow(BaseModel):
    fp_hash: str
    scopes: list[str]
    boot_id: str
    exp_up_ms: int
    label: str
    minted_at: int
    revoked: bool


class RelabelTimerBody(BaseModel):
    label: str = Field(..., min_length=0, max_length=64)


# ---------------------------------------------------------------------------
# Enrollment codes
# ---------------------------------------------------------------------------

ENROLLMENT_CODE_TTL_S = 15 * 60


def create_enrollment_code() -> EnrollCodeResponse:
    """Mint a single-use code (shared by the admin API, the admin page and
    ``python -m ostrelay.setup_code``)."""
    now = db.now_unix()
    _prune_expired_codes(now)
    code = secrets.token_urlsafe(24)
    with db.transaction() as tx:
        tx.execute(
            "INSERT INTO enrollment_codes (code, created_at, consumed_at) "
            "VALUES (?, ?, NULL)",
            (code, now),
        )
    return EnrollCodeResponse(
        enrollment_code=code, expires_at_unix=now + ENROLLMENT_CODE_TTL_S,
        setup_code=setup_code_for(code),
    )


@router.get("", include_in_schema=False)
@router.get("/", include_in_schema=False)
def admin_page() -> HTMLResponse:
    """Operator page on the localhost-only admin port (Caddy never exposes
    /admin). It holds no secret: actions use the admin token the operator
    pastes, kept only in that browser tab."""
    return HTMLResponse(ADMIN_PAGE, headers={
        "Cache-Control": "no-store",
        "Content-Security-Policy": PAGE_CSP,
        "Referrer-Policy": "no-referrer",
    })


@router.get("/assets/admin.js", include_in_schema=False)
def admin_script() -> Response:
    return Response(ADMIN_JS, media_type="text/javascript", headers={"Cache-Control": "no-store"})


@router.get("/assets/pages.css", include_in_schema=False)
def admin_styles() -> Response:
    return Response(PAGES_CSS, media_type="text/css", headers={"Cache-Control": "no-store"})


@router.get("/assets/strings.js", include_in_schema=False)
def admin_strings() -> Response:
    return Response(PAGE_STRINGS_JS, media_type="text/javascript", headers={"Cache-Control": "no-store"})


@router.get("/overview", dependencies=[Depends(require_admin)])
def overview() -> dict[str, Any]:
    """What the admin page's status strip and enrollment cards show."""
    cfg = settings()
    modes = cfg.enrollment_modes()
    open_settings = site_settings.open_enrollment_settings()
    # The confirmation count and revision must describe the same inventory.
    with db.transaction() as tx:
        enrolled = tx.execute("SELECT count(*) AS n FROM timers").fetchone()["n"]
        inventory_revision = timer_inventory_revision(tx)
    return {
        "public_url": cfg.public_url,
        "enrollment": sorted(modes),
        "setup_page_url": f"{cfg.public_url}/setup" if open_settings["open_enrollment_enabled"] and tlsid.current_pin() else "",
        "spki_pin_set": bool(tlsid.current_pin()),
        "timers_enrolled": enrolled,
        "timers_connected": len(ws.registry._sessions),  # noqa: SLF001
        "max_enrolled_timers": cfg.max_enrolled_timers,
        "max_timers_per_owner": cfg.max_timers_per_owner,
        **site_settings.landing_page_settings(),
        **open_settings,
        "timer_inventory_revision": inventory_revision,
    }


class SiteSettingsBody(BaseModel):
    model_config = ConfigDict(extra="forbid")
    landing_page_visible: StrictBool


@router.get("/site-settings", dependencies=[Depends(require_admin)])
def get_site_settings() -> dict[str, bool]:
    return site_settings.landing_page_settings()


@router.put("/site-settings", dependencies=[Depends(require_admin)])
def update_site_settings(body: SiteSettingsBody) -> dict[str, bool]:
    if body.landing_page_visible and not settings().landing_page_enabled:
        raise HTTPException(409, "landing_page_disabled_by_environment")
    return site_settings.set_landing_page_visible(body.landing_page_visible)


class EnrollmentSettingsBody(BaseModel):
    model_config = ConfigDict(extra="forbid")
    open_enrollment_requested: StrictBool


@router.get("/enrollment-settings", dependencies=[Depends(require_admin)])
def get_enrollment_settings() -> dict[str, bool]:
    return site_settings.open_enrollment_settings()


@router.put("/enrollment-settings", dependencies=[Depends(require_admin)])
def update_enrollment_settings(body: EnrollmentSettingsBody) -> dict[str, bool]:
    if body.open_enrollment_requested and "open" not in settings().enrollment_modes():
        raise HTTPException(409, "open_enrollment_disabled_by_environment")
    return site_settings.set_open_enrollment_requested(body.open_enrollment_requested)


@router.post("/enroll-code", response_model=EnrollCodeResponse,
             dependencies=[Depends(require_admin)])
def mint_enrollment_code() -> EnrollCodeResponse:
    """Mint a single-use, 15-minute-window enrollment code.

    Owners paste the returned string into the timer's local UI
    (``/api/v1/relay/enroll``). The relay's :mod:`~relay.src.ostrelay.enroll`
    endpoint consumes the code on first use and refuses a second use
    with 410 ``code_consumed``. Unused codes older than
    ``ENROLLMENT_CODE_TTL_S`` are pruned by a background sweep — for
    now the sweep runs opportunistically on each mint, since the
    volume is expected to be tiny.
    """
    return create_enrollment_code()


def _prune_expired_codes(now: int) -> None:
    with db.transaction() as tx:
        tx.execute(
            "DELETE FROM enrollment_codes "
            "WHERE consumed_at IS NULL AND created_at + ? < ?",
            (ENROLLMENT_CODE_TTL_S, now),
        )


# ---------------------------------------------------------------------------
# Timer inventory
# ---------------------------------------------------------------------------

@router.get("/timers", response_model=list[TimerRow],
            dependencies=[Depends(require_admin)])
def list_timers() -> list[TimerRow]:
    rows = db.connect().execute(
        "SELECT t.*, COALESCE(o.name, '') AS owner_name FROM timers t "
        "LEFT JOIN owners o ON o.owner_id = t.owner_id ORDER BY t.created_at DESC"
    ).fetchall()
    return [
        TimerRow(
            timer_id=r["timer_id"],
            hardware_model=r["hardware_model"],
            chip_family=r["chip_family"],
            build_id=r["build_id"],
            label=r["label"],
            created_at=r["created_at"],
            last_seen_at=r["last_seen_at"],
            connected_now=ws.registry.get(r["timer_id"]) is not None,
            owner=r["owner_name"],
        )
        for r in rows
    ]


def timer_inventory_revision(tx=None) -> str:
    connection = tx if tx is not None else db.connect()
    identifiers = [row["timer_id"] for row in connection.execute("SELECT timer_id FROM timers ORDER BY timer_id").fetchall()]
    return hashlib.sha256(json.dumps(identifiers, separators=(",", ":")).encode()).hexdigest()


class ClearTimersBody(BaseModel):
    model_config = ConfigDict(extra="forbid")
    confirmation: Literal["clear_all_timers"]
    inventory_revision: str = Field(pattern=r"^[0-9a-f]{64}$")


@router.delete("/timers", dependencies=[Depends(require_admin)])
async def clear_timers(body: ClearTimersBody) -> dict[str, int]:
    """Remove exactly the reviewed inventory; preserve owners and site settings."""
    with db.transaction() as tx:
        if body.inventory_revision != timer_inventory_revision(tx):
            raise HTTPException(409, "timer_inventory_changed")
        identifiers = [row["timer_id"] for row in tx.execute("SELECT timer_id FROM timers").fetchall()]
        tx.execute("DELETE FROM timers")
    async def close_registration(identifier):
        session = ws.registry.get(identifier)
        if session is not None:
            try:
                await asyncio.wait_for(session.ws.close(code=4003, reason="enrollment_erased"),
                                       timeout=settings().ws_send_timeout_s)
            except Exception:
                # Enrollment and grant/session rows are already erased. New
                # hellos and partner requests are denied even if close fails.
                pass
    await asyncio.gather(*(close_registration(identifier) for identifier in identifiers))
    return {"timers_removed": len(identifiers)}


@router.delete("/timers/{timer_id}",
               dependencies=[Depends(require_admin)])
async def delete_timer(timer_id: str) -> dict[str, bool]:
    """Erase enrollment. Cascades to tokens and sessions."""
    with db.transaction() as tx:
        cur = tx.execute("DELETE FROM timers WHERE timer_id = ?", (timer_id,))
        if cur.rowcount == 0:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "not_found")
    # If a WSS is currently attached, close it. The timer will observe
    # 401 on its next hello and require re-enrollment.
    session = ws.registry.get(timer_id)
    if session is not None:
        await session.ws.close(code=4003, reason="enrollment_erased")
    return {"deleted": True}


@router.patch("/timers/{timer_id}",
              dependencies=[Depends(require_admin)])
def relabel_timer(timer_id: str, body: RelabelTimerBody) -> dict[str, bool]:
    with db.transaction() as tx:
        cur = tx.execute(
            "UPDATE timers SET label = ? WHERE timer_id = ?",
            (body.label, timer_id),
        )
        if cur.rowcount == 0:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "not_found")
    return {"updated": True}


# ---------------------------------------------------------------------------
# Token inventory
# ---------------------------------------------------------------------------

@router.get("/tokens/{timer_id}", response_model=list[TokenRow],
            dependencies=[Depends(require_admin)])
def list_tokens(timer_id: str) -> list[TokenRow]:
    rows = db.connect().execute(
        "SELECT * FROM tokens WHERE timer_id = ? ORDER BY minted_at DESC",
        (timer_id,),
    ).fetchall()
    return [
        TokenRow(
            fp_hash=r["fp_hash"],
            scopes=json.loads(r["scope_json"]),
            boot_id=r["boot_id"],
            exp_up_ms=r["exp_up_ms"],
            label=r["label"],
            minted_at=r["minted_at"],
            revoked=bool(r["revoked"]),
        )
        for r in rows
    ]


@router.post("/tokens/{fp_hash}/revoke",
             dependencies=[Depends(require_admin)])
def revoke_token(fp_hash: str) -> dict[str, Any]:
    """Server-side revoke — for compromised-partner scenarios.

    The authoritative revoke path is timer-local (owner UI). This
    endpoint marks the row as revoked so :mod:`session` refuses to
    accept it and any live SSE stream is closed by the fanout, but
    the timer's own NVS table is unchanged until the next
    ``token_revoke`` upstream frame from that timer or the next
    reboot. In other words: this stops the relay from being used to
    carry the token further; it does not delete the token from the
    timer's own NVS. Owners should still hit the local panic if the
    partner is actively hostile.
    """
    with db.transaction() as tx:
        cur = tx.execute(
            "UPDATE tokens SET revoked = 1 WHERE fp_hash = ?", (fp_hash,)
        )
        if cur.rowcount == 0:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "not_found")
        # Also drop any active sessions bound to it so /watch closes.
        tx.execute("DELETE FROM sessions WHERE fp_hash = ?", (fp_hash,))

    # Each stream rechecks its own session row; a revoke must not terminate
    # unrelated tokens' streams on the same timer.
    return {"revoked": True}


# ---------------------------------------------------------------------------
# Health passthrough (public — moved here so operators can point their
# uptime monitors at /admin/health without touching the SPA surface)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Owners (OSTRELAY_ENROLLMENT includes owner_codes)
# ---------------------------------------------------------------------------

class OwnerBody(BaseModel):
    name: str = Field(..., min_length=1, max_length=40)


class OwnerCodeResponse(BaseModel):
    name: str
    owner_code: str
    # Reusable: the owner enrolls each of their timers with it.
    setup_code: str = ""


@router.get("/owners", dependencies=[Depends(require_admin)])
def list_owners() -> list[dict[str, Any]]:
    return owners.list_owners()


@router.post("/owners", response_model=OwnerCodeResponse, dependencies=[Depends(require_admin)])
def invite_owner(body: OwnerBody) -> OwnerCodeResponse:
    try:
        code = owners.create_owner(body.name)
    except ValueError as error:
        raise HTTPException(status.HTTP_409_CONFLICT, str(error)) from None
    return OwnerCodeResponse(name=body.name, owner_code=code, setup_code=setup_code_for(code))


@router.post("/owners/{name}/rotate", response_model=OwnerCodeResponse,
             dependencies=[Depends(require_admin)])
def rotate_owner(name: str) -> OwnerCodeResponse:
    try:
        code = owners.rotate_owner(name)
    except ValueError:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "not_found") from None
    return OwnerCodeResponse(name=name, owner_code=code, setup_code=setup_code_for(code))


@router.post("/owners/{name}/revoke", dependencies=[Depends(require_admin)])
async def revoke_owner(name: str) -> dict[str, Any]:
    """Stop the owner's code and remove their timers (links and sessions too)."""
    try:
        removed = owners.revoke_owner(name)
    except ValueError:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "not_found") from None
    for timer_id in removed:
        session = ws.registry.get(timer_id)
        if session is not None:
            await session.ws.close(code=4003, reason="enrollment_erased")
    return {"revoked": True, "timers_removed": len(removed)}


@router.get("/health")
def admin_health() -> dict[str, Any]:
    active = 0
    for _ in ws.registry._sessions.values():   # noqa: SLF001 — internal peek OK
        active += 1
    return {"ok": True, "protocol": "ost-relay/1",
            "timers_connected": active,
            "unix": int(time.time())}
