"""Bounded ciphertext forwarding. Command authority lives exclusively on the timer."""
from __future__ import annotations
import asyncio,time
from collections import defaultdict,deque
from fastapi import APIRouter,Depends,HTTPException
from . import ws
from .config import settings
from .models import CommandRequest
from .session import require_session
from .e2e import validate_frame
router=APIRouter()
_session_buckets: dict[str, deque[float]] = defaultdict(deque)
_timer_buckets: dict[str, deque[float]] = defaultdict(deque)
_bucket_lock = asyncio.Lock()


async def _rate_limit(session_id: str, timer_id: str) -> None:
    cfg = settings()
    async with _bucket_lock:
        now = time.monotonic()
        for buckets in (_session_buckets, _timer_buckets):
            for key, bucket in list(buckets.items()):
                while bucket and now - bucket[0] >= 300:
                    bucket.popleft()
                if not bucket:
                    del buckets[key]
            if len(buckets) >= cfg.max_rate_limit_keys:
                raise HTTPException(429, "rate_limit_capacity")
        for bucket in (_session_buckets[session_id], _timer_buckets[timer_id]):
            while bucket and now - bucket[0] > 300:
                bucket.popleft()
        if len(_session_buckets[session_id]) >= cfg.rate_per_session_5min:
            raise HTTPException(429, "rate_limited_session")
        if len(_timer_buckets[timer_id]) >= cfg.rate_per_timer_5min:
            raise HTTPException(429, "rate_limited_timer")
        _session_buckets[session_id].append(now)
        _timer_buckets[timer_id].append(now)


def _reset_buckets_for_tests() -> None:
    _session_buckets.clear()
    _timer_buckets.clear()


@router.post("/api/v1/command")
async def command(body:CommandRequest,session_row:dict=Depends(require_session)):
    try:
        validate_frame(body.model_dump())
    except ValueError:
        raise HTTPException(400,"invalid_encrypted_frame") from None
    if body.fp_hash!=session_row["fp_hash"] or body.client_id!=session_row["client_id"]:
        raise HTTPException(409,"session_mismatch")
    await _rate_limit(session_row["session_id"],session_row["timer_id"])
    live=ws.registry.get(session_row["timer_id"])
    if live is None: raise HTTPException(503,"timer_not_connected")
    return await ws.send_opaque(live,body.model_dump())
