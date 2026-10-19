"""Opaque browser routing sessions. Capabilities and channel keys stay at endpoints."""
from __future__ import annotations
import hashlib,json,re,secrets,time
from collections import deque
from typing import Any
from cryptography.hazmat.primitives.asymmetric import ec
from fastapi import APIRouter,Depends,HTTPException,Request,Response
from . import db,ws
from .config import settings
from .models import SessionRequest,SessionResponse
from .e2e import validate_frame
from .wire import b64decode as _b64url_decode,b64encode as _b64url_encode,integer,loads,scopes_valid
router=APIRouter()
_create_buckets:dict[str,deque[float]]={}
def parse_wire_token(token: str) -> dict[str, Any]:
    """Strict structural parse; exact timer-registered digest authenticates it."""
    try:
        if not isinstance(token, str) or len(token) > 512:
            raise ValueError()
        prefix, version, encoded, tag = token.split(".")
        if prefix != "ost-cap" or version != "v1":
            raise ValueError()
        _b64url_decode(tag, 16)
        payload = loads(_b64url_decode(encoded), 512)
        if set(payload) != {"aud", "tid", "fp", "sc", "boot", "exp_up_ms", "mint_up_ms", "vk"}:
            raise ValueError()
        if payload["aud"] != "ost-relay/1" or not scopes_valid(payload["sc"]):
            raise ValueError()
        if not isinstance(payload["tid"], str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", payload["tid"]):
            raise ValueError()
        if not isinstance(payload["boot"], str) or not re.fullmatch(r"[0-9a-f]{1,16}", payload["boot"]):
            raise ValueError()
        _b64url_decode(payload["fp"], 16)
        verify_key = _b64url_decode(payload["vk"], 33)
        # ECDSA P-256 compressed point; from_encoded_point rejects off-curve keys.
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), verify_key)
        if not integer(payload["mint_up_ms"]) or not integer(payload["exp_up_ms"], 1):
            raise ValueError()
        if payload["mint_up_ms"] >= payload["exp_up_ms"]:
            raise ValueError()
        return payload
    except (ValueError, TypeError, KeyError, UnicodeError, RecursionError):
        raise HTTPException(400, "malformed_token") from None


def fingerprint_hash(fp_b64: str) -> str:
    return _b64url_encode(hashlib.sha256(_b64url_decode(fp_b64, 16)).digest()[:16])


def token_digest(token: str) -> str:
    return _b64url_encode(hashlib.sha256(token.encode("ascii")).digest())


def resolve_session(session_id: str) -> dict[str, Any]:
    row=db.connect().execute("SELECT s.*,t.timer_id,t.boot_id,t.revoked FROM sessions s "
        "JOIN tokens t ON s.fp_hash=t.fp_hash WHERE s.session_id=?",(session_id,)).fetchone()
    if row is None or row["expires_at"]<=time.time(): raise HTTPException(401,"expired_session")
    if row["revoked"]: raise HTTPException(401,"token_revoked")
    live=ws.registry.get(row["timer_id"])
    if live and live.boot_id!=row["boot_id"]: raise HTTPException(401,"expired_session")
    return dict(row)


async def require_session(request: Request) -> dict[str, Any]:
    handle=request.headers.get("x-ost-session") or request.query_params.get("session_handle")
    if not handle or not re.fullmatch(r"[A-Za-z0-9_-]{22}",handle): raise HTTPException(401,"no_session")
    return resolve_session(handle)


def _limit_session_creation(request: Request) -> None:
    now = time.monotonic()
    for key, bucket in list(_create_buckets.items()):
        while bucket and bucket[0] <= now - 60:
            bucket.popleft()
        if not bucket:
            del _create_buckets[key]
    # ASGI peer address. uvicorn rewrites it from X-Forwarded-For only for
    # proxies named in OSTRELAY_TRUSTED_PROXY_IPS, never for arbitrary clients.
    ip = request.client.host if request.client else "unknown"
    if ip not in _create_buckets and len(_create_buckets) >= settings().max_rate_limit_keys:
        raise HTTPException(429, "rate_limit_capacity")
    bucket = _create_buckets.setdefault(ip, deque())
    if len(bucket) >= settings().rate_session_creates_per_ip_1min:
        raise HTTPException(429, "rate_limited_session_creation")
    bucket.append(now)


@router.post("/api/v1/session")
async def create_session(body: SessionRequest,request: Request,response: Response) -> SessionResponse:
    _limit_session_creation(request)
    row=db.connect().execute("SELECT * FROM tokens WHERE fp_hash=?",(body.fp_hash,)).fetchone()
    if row is None or row["revoked"] or row["boot_id"]!=body.boot_id: raise HTTPException(401,"invalid_or_expired")
    live=ws.registry.get(row["timer_id"])
    if live is None: raise HTTPException(503,"timer_not_connected")
    if live.boot_id!=body.boot_id: raise HTTPException(401,"invalid_or_expired")
    frame={"type":"e2e_open",**body.model_dump()}
    try:
        validate_frame(frame)
    except ValueError:
        raise HTTPException(400,"invalid_encrypted_frame") from None
    # Admission is reserved before awaiting the device, and rolled back on failure.
    handle=secrets.token_urlsafe(16)
    with db.transaction() as tx:
        tx.execute("DELETE FROM sessions WHERE expires_at<=?",(db.now_unix(),))
        total=tx.execute("SELECT count(*) n FROM sessions").fetchone()["n"]
        count=tx.execute("SELECT count(*) n FROM sessions WHERE fp_hash=?",(body.fp_hash,)).fetchone()["n"]
        if total>=settings().max_sessions or count>=settings().max_sessions_per_token: raise HTTPException(429,"session_capacity")
        tx.execute("INSERT INTO sessions VALUES(?,?,?,?,?)",(handle,body.fp_hash,body.client_id,db.now_unix(),db.now_unix()+settings().session_max_age_s))
    try:
        accepted=await ws.send_opaque(live,frame)
    except BaseException:
        db.connect().execute("DELETE FROM sessions WHERE session_id=?",(handle,))
        raise
    response.headers["Cache-Control"]="no-store"
    return SessionResponse(session_handle=handle,timer_id=row["timer_id"],boot_id=body.boot_id,frame=accepted)


@router.get("/api/v1/session")
async def resume_session(response: Response,row: dict=Depends(require_session)):
    db.connect().execute("UPDATE sessions SET expires_at=? WHERE session_id=?",(db.now_unix()+settings().session_max_age_s,row["session_id"]))
    response.headers["Cache-Control"]="no-store"
    return {"session_handle":row["session_id"],"timer_id":row["timer_id"],"boot_id":row["boot_id"]}


@router.delete("/api/v1/session")
@router.post("/api/v1/logout")
async def logout(row:dict=Depends(require_session)):
    db.connect().execute("DELETE FROM sessions WHERE session_id=?",(row["session_id"],))
    return {"ok":True}
