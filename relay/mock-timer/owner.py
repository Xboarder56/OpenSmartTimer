"""Loopback-only creator surface for the isolated mock timer, never relay-hosted."""
import hmac
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse

ROOT = Path(__file__).resolve().parents[1]


def create_owner_app(session, public_url: str, secret: str, port: int = 8081) -> FastAPI:
    app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
    origin = f"http://127.0.0.1:{port}"

    @app.middleware("http")
    async def boundaries(request: Request, call_next):
        from fastapi.responses import JSONResponse
        if request.headers.get("host") != f"127.0.0.1:{port}":
            return JSONResponse({"detail": "Invalid host"}, status_code=403)
        if request.url.path.startswith("/api/"):
            if not hmac.compare_digest(request.headers.get("x-ost-owner", "").encode(), secret.encode()):
                return JSONResponse({"detail": "Open the creator address printed by the mock timer"}, status_code=401)
            if request.headers.get("origin", origin) != origin:
                return JSONResponse({"detail": "Invalid origin"}, status_code=403)
        response = await call_next(request)
        response.headers.update({"Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
                                 "X-Content-Type-Options": "nosniff",
                                 "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"})
        return response

    @app.get("/")
    async def index():
        return FileResponse(ROOT / "mock-timer/creator/index.html")

    @app.get("/assets/{name}")
    async def asset(name: str):
        files = {"owner.js": ROOT / "mock-timer/creator/owner.js", "owner.css": ROOT / "mock-timer/creator/owner.css",
                 "timer-ui.css": ROOT / "spa/timer-ui.css", "app.css": ROOT / "spa/app.css"}
        if name not in files:
            raise HTTPException(404)
        return FileResponse(files[name])

    @app.get("/api/links")
    async def links():
        return {"connected": session.can_share(), "can_share": session.can_share(),
                "links": [session.link_info(r, public_url) for r in session.keys.tokens]}

    @app.post("/api/links", status_code=202)
    async def create(request: Request):
        # Bound streaming bodies too; Content-Length alone is insufficient.
        body = bytearray()
        async for chunk in request.stream():
            body.extend(chunk)
            if len(body) > 2048:
                raise HTTPException(413, "Request too large")
        import json
        try:
            data = json.loads(body)
            if not isinstance(data, dict) or set(data) != {"ttl_seconds", "label", "request_id"}:
                raise ValueError("Expected lifetime, label, and request ID")
            rec = await session.create_partner_link(**data)
        except (ValueError, TypeError) as exc:
            raise HTTPException(400, str(exc)) from None
        except ConnectionError as exc:
            raise HTTPException(503, str(exc)) from None
        except RuntimeError as exc:
            raise HTTPException(429, str(exc)) from None
        return session.link_info(rec, public_url)

    @app.delete("/api/links/{fp_hash}", status_code=202)
    async def revoke(fp_hash: str):
        rec = next((r for r in session.keys.tokens if r.fp_hash == fp_hash), None)
        if rec is None:
            raise HTTPException(404, "Link not found")
        await session.send_token_revoke(fp_hash)
        return session.link_info(rec, public_url)

    return app
