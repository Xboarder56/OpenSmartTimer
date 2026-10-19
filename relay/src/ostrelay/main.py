"""FastAPI app wiring for the ost-relay/1 reference relay."""

from __future__ import annotations

import logging
import asyncio
from pathlib import Path
from contextlib import asynccontextmanager
from typing import AsyncIterator

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse

from . import __version__, __protocol__, admin, command, db, enroll, owners, session, tlsid, watch, ws
from .setup_page import router as setup_page_router
from . import portal
from .limits import RequestLimits
from .security_headers import SecurityHeaders

log = logging.getLogger(__name__)

@asynccontextmanager
async def _lifespan(_app: FastAPI) -> AsyncIterator[None]:
    db.connect()   # eagerly apply schema
    log.info("ostrelay: db ready, protocol=ost-relay/1")
    from .config import settings
    log.info("ostrelay: enrollment=%s", ",".join(sorted(settings().enrollment_modes())))
    first_code = owners.ensure_first_owner()

    def announce_first_owner() -> None:
        # Printed once, on the relay's first start. Rotate to reissue:
        # python -m ostrelay.setup_code --rotate owner
        setup = admin.setup_code_for(first_code) or first_code
        log.warning("ostrelay: first owner created. Setup code for your timers "
                    "(reusable; shown only now):\n\n%s\n", setup)

    async def announce_when_key_known() -> None:
        # A setup code carries the pin of the served TLS key. When the relay
        # reads that key from its own public URL, the proxy may not be routing
        # to it yet: allow two minutes before printing the bare owner code.
        for _ in range(24):
            if tlsid.current_pin():
                break
            await asyncio.sleep(5)
        announce_first_owner()

    background = [asyncio.create_task(tlsid.refresh_loop())]
    if first_code and tlsid.follows_certificate():
        background.append(asyncio.create_task(announce_when_key_known()))
    elif first_code:
        announce_first_owner()
    async def cleanup_loop():
        from .config import settings
        while True:
            await asyncio.sleep(settings().cleanup_interval_s)
            db.cleanup()
            ws.registry.cleanup()
            await ws.registry.close_unenrolled()
    cleanup = asyncio.create_task(cleanup_loop())
    try:
        yield
    finally:
        for task in (cleanup, *background):
            task.cancel()
        await asyncio.gather(cleanup, *background, return_exceptions=True)


def create_app() -> FastAPI:
    app = FastAPI(
        title="OpenSmartTimer relay",
        summary="ost-relay/1 reference implementation",
        version=__version__,
        lifespan=_lifespan,
    )
    app.add_middleware(RequestLimits)
    app.include_router(enroll.router)
    app.include_router(session.router)
    app.include_router(watch.router)
    app.include_router(command.router)
    app.include_router(ws.router)
    from .config import settings
    if settings().admin_enabled:
        app.include_router(admin.router)
    app.include_router(setup_page_router)
    app.include_router(portal.router)
    if settings().oidc_enabled():
        # authlib keeps the login state (state, nonce, PKCE verifier) in this
        # short-lived signed cookie between /owner/login and /owner/callback.
        from starlette.middleware.sessions import SessionMiddleware
        app.add_middleware(SessionMiddleware, secret_key=portal.state_secret(),
                           session_cookie=portal.STATE_COOKIE, max_age=600,
                           same_site="lax", https_only=True)
    # Outermost user middleware also covers body/origin rejection responses.
    app.add_middleware(SecurityHeaders)

    @app.get("/api/v1/health")
    def health() -> dict[str, bool | str]:
        return {"ok": True, "protocol": __protocol__}

    # Only shared operator appearance assets. Never expose the partner app,
    # crypto module or shell from this origin.
    @app.get("/assets/{asset}")
    def operator_asset(asset: str):
        if asset not in {"appearance.js", "i18n.js", "timer-ui.css", "icon.svg"}:
            raise HTTPException(404, "not_found")
        directory = Path(settings().spa_dir) if settings().spa_dir else Path(__file__).resolve().parents[2] / "spa"
        return FileResponse(directory / asset)

    # Partner HTML and JavaScript are deliberately absent from this origin.
    from starlette.middleware.cors import CORSMiddleware
    if settings().partner_origin:
        app.add_middleware(CORSMiddleware,allow_origins=[settings().partner_origin],
            allow_methods=["GET","POST","DELETE"],allow_headers=["Content-Type","X-OST-Session"],
            allow_credentials=False,max_age=600)

    return app


app = create_app()


def run() -> None:
    """Console entrypoint (``ostrelay`` in pyproject scripts)."""
    import uvicorn
    from .config import settings
    cfg = settings()
    proxies = cfg.trusted_proxy_list()
    # uvicorn trusts 127.0.0.1 forwarding headers by default; only named proxies may.
    uvicorn.run(app, host=cfg.listen_host, port=cfg.listen_port,
                workers=1, ws_max_size=cfg.max_frame_bytes, ws_max_queue=16,
                # Admission cap on open connections (SSE and WSS included);
                # beyond it uvicorn answers 503 instead of queueing.
                limit_concurrency=cfg.max_concurrent_connections, timeout_keep_alive=5,
                proxy_headers=bool(proxies), forwarded_allow_ips=proxies or None)


if __name__ == "__main__":
    run()
