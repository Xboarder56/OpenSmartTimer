"""SSE fanout retaining the timer's original signed envelopes."""
from __future__ import annotations

import asyncio
import json
from typing import Any, AsyncIterator

from fastapi import APIRouter, Depends, HTTPException, Request
from sse_starlette.sse import EventSourceResponse

from . import ws
from .config import settings
from .session import require_session, resolve_session

router = APIRouter()
_RECHECK_INTERVAL_S = 1.0
_QUEUE_SIZE = 64


def _token_still_valid(session_row: dict[str, Any], ws_session: ws.WsSession) -> tuple[bool, str | None]:
    try:
        resolve_session(session_row["session_id"])
    except HTTPException as exc:
        return False, "token_revoked" if exc.detail == "token_revoked" else "token_expired"
    if ws.registry.get(ws_session.timer_id) is not ws_session:
        return False, "timer_gone"
    return True, None


async def _stream(session_row: dict[str, Any], request: Request, live: ws.WsSession,
                  q: asyncio.Queue[dict[str, Any]]) -> AsyncIterator[dict[str, str]]:
    # The slot (q) was reserved at admission; _ReservedEventSource releases it.
    try:
        valid, terminal = _token_still_valid(session_row, live)
        if not valid:
            yield {"event": terminal, "data": ""}
            return
        while True:
            if await request.is_disconnected():
                return
            valid, terminal = _token_still_valid(session_row, live)
            if not valid:
                yield {"event": terminal, "data": ""}
                return
            try:
                envelope = await asyncio.wait_for(q.get(), timeout=_RECHECK_INTERVAL_S)
            except asyncio.TimeoutError:
                continue
            # Check authorization again immediately before every delivered event.
            valid, terminal = _token_still_valid(session_row, live)
            if not valid:
                yield {"event": terminal, "data": ""}
                return
            etype = envelope.get("type")
            if etype == "e2e" and envelope.get("client_id")==session_row["client_id"] and envelope.get("fp_hash")==session_row["fp_hash"]:
                yield {"event": "e2e", "data": json.dumps(envelope, separators=(",", ":"))}
            elif etype in ("token_expired", "token_revoked", "timer_gone", "slow_consumer"):
                yield {"event": "timer_gone" if etype == "slow_consumer" else etype, "data": ""}
                return
    finally:
        live.subscribers.discard(q)


@router.get("/api/v1/watch")
async def watch(request: Request, session_row: dict[str, Any] = Depends(require_session)) -> EventSourceResponse:
    live = ws.registry.get(session_row["timer_id"])
    if live is None:
        raise HTTPException(503, "timer_not_connected")
    # Reserve the slot now (no await between check and add), release it when
    # the response ends however it ends.
    if len(live.subscribers) >= settings().max_subscribers_per_timer:
        raise HTTPException(429, "subscriber_capacity")
    q: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=_QUEUE_SIZE)
    live.subscribers.add(q)
    return _ReservedEventSource(live, q, _stream(session_row, request, live, q),
                                headers={"Cache-Control": "no-store"},
                                send_timeout=settings().sse_send_timeout_s)


class _ReservedEventSource(EventSourceResponse):
    """SSE response that owns one subscriber slot for its whole lifetime.
    ``send_timeout`` ends a response stuck writing to a slow client."""

    def __init__(self, live: Any, queue: Any, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._live, self._queue = live, queue

    async def __call__(self, scope: Any, receive: Any, send: Any) -> None:
        try:
            await super().__call__(scope, receive, send)
        finally:
            self._live.subscribers.discard(self._queue)
