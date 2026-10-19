"""Bound request bodies before FastAPI parses them (including chunked bodies)."""
import asyncio
from urllib.parse import urlsplit

from starlette.responses import JSONResponse

from .config import settings
from .wire import loads


class RequestLimits:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        headers = dict(scope.get("headers", []))
        if scope["method"] in ("POST", "PUT", "PATCH", "DELETE"):
            origin = headers.get(b"origin")
            public = urlsplit(settings().public_url)
            allowed = {f"{public.scheme}://{public.netloc}".encode()}
            if scope["path"] in ("/api/v1/session","/api/v1/command","/api/v1/logout") and settings().partner_origin:
                allowed.add(settings().partner_origin.encode())
            if scope["path"].startswith("/admin/"):
                # The admin page runs on the local admin port, so its own
                # origin differs from the public URL. Admin calls carry a
                # bearer token (never cookies), so same-origin is enough.
                host = headers.get(b"host", b"")
                allowed |= {b"http://" + host, b"https://" + host}
            if origin is not None and origin not in allowed:
                return await JSONResponse({"detail": "origin_rejected"}, status_code=403)(scope, receive, send)
            body = bytearray()
            # A slow client may not hold a worker: the whole body must arrive
            # within http_body_timeout_s (bodies are at most a few KiB).
            deadline = asyncio.get_running_loop().time() + settings().http_body_timeout_s
            while True:
                try:
                    event = await asyncio.wait_for(receive(), timeout=max(0.0, deadline - asyncio.get_running_loop().time()))
                except asyncio.TimeoutError:
                    return await JSONResponse({"detail": "request_timeout"}, status_code=408)(scope, receive, send)
                if event["type"] == "http.disconnect":
                    return
                body.extend(event.get("body", b""))
                if len(body) > settings().max_http_body_bytes:
                    return await JSONResponse({"detail": "body_too_large"}, status_code=413)(scope, receive, send)
                if not event.get("more_body", False):
                    break
            if body:
                try:
                    loads(bytes(body), settings().max_http_body_bytes)
                except (ValueError, UnicodeError, RecursionError):
                    return await JSONResponse({"detail": "malformed_json"}, status_code=400)(scope, receive, send)
            delivered = False
            async def bounded_receive():
                nonlocal delivered
                if not delivered:
                    delivered = True
                    return {"type": "http.request", "body": bytes(body), "more_body": False}
                return await receive()
            return await self.app(scope, bounded_receive, send)
        return await self.app(scope, receive, send)
