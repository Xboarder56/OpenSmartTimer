"""Browser protections enforced even when the ASGI app is used without Caddy."""
from starlette.datastructures import MutableHeaders

SPA_CSP = ("default-src 'self'; script-src 'self'; style-src 'self'; "
           "img-src 'self' data:; connect-src 'self'; base-uri 'none'; "
           "object-src 'none'; frame-ancestors 'none'; form-action 'self'")


class SecurityHeaders:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)

        async def protected_send(message):
            if message["type"] == "http.response.start":
                headers = MutableHeaders(scope=message)
                headers.setdefault("X-Frame-Options", "DENY")
                headers.setdefault("Referrer-Policy", "no-referrer")
                headers.setdefault("X-Content-Type-Options", "nosniff")
                headers.setdefault("Cache-Control", "no-store")
                # Operator/setup/owner pages supply their own CSP. The partner
                # shell needs an HTTP policy (frame-ancestors in meta is ignored).
                # Do not apply the SPA's script policy to FastAPI's internal docs.
                if ("text/html" in headers.get("content-type", "")
                        and (scope["path"] == "/s" or scope["path"].startswith("/s/"))):
                    headers.setdefault("Content-Security-Policy", SPA_CSP)
            await send(message)

        await self.app(scope, receive, protected_send)
