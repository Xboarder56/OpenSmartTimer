"""Environment-driven relay settings."""

from __future__ import annotations

import re

from pydantic import Field, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Populated from OSTRELAY_* environment variables and an optional .env."""

    model_config = SettingsConfigDict(
        env_prefix="OSTRELAY_", env_file=".env", extra="ignore", hide_input_in_errors=True
    )

    # Public relay backend origin; the trusted control page is hosted separately.
    public_url: str = "http://localhost:8080"

    # base64url SHA-256 of the SPKI of the TLS certificate timers will see,
    # extracted per OPERATOR.md; several may be separated by commas. Leave
    # empty to follow the certificate actually served at public_url, which
    # must then be issued by a public certificate authority (see tlsid.py).
    spki_pin: str = ""
    # Optional host:port to read that certificate from, when the container
    # cannot reach its own public address (for example the proxy's service
    # name). The public hostname is still used for SNI and validation.
    tls_probe_address: str = ""
    dev_mode: bool = False

    # SPA static-file directory override. Empty means "use the default
    # inferred from installed package layout"; Docker sets this to
    # /opt/ostrelay/spa where the Dockerfile stages assets.
    spa_dir: str = ""
    # Independently hosted trusted partner origin; never serve its code here.
    partner_origin: str = ""

    # Hard gate for the public / landing page. The admin's persisted
    # visibility preference cannot override false. Does not gate /setup or /s/.
    landing_page_enabled: bool = True

    # Hard gate for all admin pages and APIs. Command-line setup stays available.
    admin_enabled: bool = True

    # Bearer token required for /admin/* endpoints. Operator-supplied.
    admin_token: str = Field(..., min_length=32, max_length=256, pattern=r"^[!-~]+$")

    # SQLite path. Set to :memory: for ephemeral tests.
    db_path: str = "./ostrelay.sqlite"

    # 32-byte hex secret used to sign session cookies.
    cookie_secret: str = Field(..., pattern=r"^[0-9a-fA-F]{64}$")

    # Session cookie lifetime cap. Bounded again by the token's expiry.
    session_max_age_s: int = 7200
    # A browser session outlives its link by this much, so a page that is
    # still open resumes if the owner extends (reactivates) the link.
    session_reactivation_grace_s: int = 900

    # How long we'll wait for a downstream command's result before
    # returning 504 to the browser.
    command_await_s: float = 5.0

    # WSS heartbeat cadence + timeout.
    ws_ping_interval_s: float = 30.0
    ws_ping_timeout_s: float = 90.0

    # Per-session command bucket (RELAY_PROTOCOL.md §10).
    rate_per_session_5min: int = 300
    rate_per_timer_5min: int = 3000
    rate_session_creates_per_ip_1min: int = 10

    max_sessions: int = Field(1000, ge=1)
    max_sessions_per_token: int = Field(8, ge=1)
    max_owner_sessions: int = Field(1000, ge=1)
    max_owner_sessions_per_owner: int = Field(8, ge=1)
    max_owners: int = Field(1000, ge=1)
    max_connected_timers: int = Field(100, ge=1)
    max_ws_handshakes: int = Field(32, ge=1)
    max_subscribers_per_timer: int = Field(16, ge=1)
    max_pending_per_timer: int = Field(32, ge=1)
    max_results_per_timer: int = Field(256, ge=1)
    result_retention_s: int = Field(300, ge=1)
    max_frame_bytes: int = Field(16384, ge=1024, le=1048576)
    max_http_body_bytes: int = Field(8192, ge=1024)
    max_rate_limit_keys: int = Field(4096, ge=1)
    max_tokens_per_timer: int = Field(128, ge=1)
    # How timers may enroll: "owner_codes", "open", or both (comma-separated).
    # See ostrelay/owners.py and OPERATOR.md.
    enrollment: str = "owner_codes"
    max_enrolled_timers: int = Field(100, ge=1)
    max_timers_per_owner: int = Field(10, ge=1)
    rate_open_enrollments_per_ip_1h: int = Field(5, ge=1)
    status_max_age_s: float = Field(15.0, gt=0)
    hello_timeout_s: float = Field(5.0, gt=0)
    # Internet-facing deadlines and budgets (slow clients must not hold
    # resources). SSE and WSS stay open; only individual writes are bounded.
    ws_send_timeout_s: float = Field(10.0, gt=0)
    sse_send_timeout_s: float = Field(15.0, gt=0)
    http_body_timeout_s: float = Field(10.0, gt=0)
    max_concurrent_connections: int = Field(1024, ge=16)
    rate_timer_frames_per_min: int = Field(1200, ge=60)
    rate_enrollments_per_ip_1h: int = Field(20, ge=1)
    # Owner self-service sign-in (optional). Unset = no /owner portal: the
    # operator hands out setup codes from the admin page (admin token).
    # Register <OSTRELAY_PUBLIC_URL>/owner/callback as the redirect URI.
    oidc_issuer: str = ""
    oidc_client_id: str = ""
    oidc_client_secret: str = ""
    oidc_scopes: str = "openid profile email"
    oidc_signing_alg: str = "RS256"
    oidc_groups_claim: str = "groups"
    # Comma-separated; empty lets any account at the provider become an owner.
    oidc_allowed_groups: str = ""
    oidc_provider_name: str = "single sign-on"
    owner_session_ttl_s: int = Field(12 * 3600, ge=300, le=7 * 86400)
    # Open enrollments that never connect are reclaimed after this long.
    open_enrollment_unused_ttl_s: int = Field(24 * 3600, ge=600)
    cleanup_interval_s: float = Field(60.0, gt=0)

    # Bind for uvicorn.
    listen_host: str = "127.0.0.1"
    listen_port: int = 8080

    # Comma-separated reverse-proxy IPs/CIDRs whose X-Forwarded-For is honored
    # for per-client rate limits. Empty disables forwarding headers entirely,
    # so every limit keys on the direct ASGI peer.
    trusted_proxy_ips: str = ""

    def oidc_enabled(self) -> bool:
        return bool(self.oidc_issuer)

    def oidc_allowed_group_set(self) -> set[str]:
        return {g.strip() for g in self.oidc_allowed_groups.split(",") if g.strip()}

    def enrollment_modes(self) -> set[str]:
        return {mode.strip() for mode in self.enrollment.split(",") if mode.strip()}

    @model_validator(mode="after")
    def deployment_trust(self):
        if not self.enrollment_modes() or not self.enrollment_modes() <= {"owner_codes", "open"}:
            raise ValueError('OSTRELAY_ENROLLMENT must be "owner_codes", "open" or "owner_codes,open"')
        from urllib.parse import urlsplit
        from .wire import b64decode
        url = urlsplit(self.public_url)
        if (url.scheme not in ("http", "https") or not url.hostname or url.username or url.password
                or url.query or url.fragment or url.path not in ("", "/")):
            raise ValueError("public URL must be an HTTP(S) origin without credentials or a path")
        self.public_url = self.public_url.rstrip("/")
        if self.partner_origin:
            partner=urlsplit(self.partner_origin)
            if (partner.scheme!="https" or not partner.hostname or partner.username or partner.password
                    or partner.path not in ("","/") or partner.query or partner.fragment):
                raise ValueError("partner origin must be an HTTPS origin")
            self.partner_origin=self.partner_origin.rstrip("/")
            if self.partner_origin==self.public_url: raise ValueError("partner origin must be independent of relay")

        for pin in self.spki_pin.split(","):
            if pin.strip():
                b64decode(pin.strip(), 32)
        if self.tls_probe_address and not re.fullmatch(r"[A-Za-z0-9.\-\[\]:]{1,255}:[0-9]{1,5}", self.tls_probe_address):
            raise ValueError("TLS probe address must be host:port")
        if not self.dev_mode and url.scheme != "https":
            raise ValueError("an HTTPS public URL is required; loopback mocks may explicitly set DEV_MODE")
        if self.dev_mode and url.hostname not in ("localhost", "127.0.0.1", "::1", "testserver"):
            raise ValueError("DEV_MODE is restricted to loopback/testserver development origins")
        if self.oidc_issuer:
            issuer = urlsplit(self.oidc_issuer)
            self.oidc_issuer = self.oidc_issuer.rstrip("/")
            if not self.oidc_client_id or not self.oidc_client_secret:
                raise ValueError("OSTRELAY_OIDC_CLIENT_ID and OSTRELAY_OIDC_CLIENT_SECRET are required with OSTRELAY_OIDC_ISSUER")
            if issuer.scheme != "https" and not self.dev_mode:
                raise ValueError("OSTRELAY_OIDC_ISSUER must be an https URL")
            if "openid" not in self.oidc_scopes.split():
                raise ValueError("OSTRELAY_OIDC_SCOPES must include openid")
            if "owner_codes" not in self.enrollment_modes():
                raise ValueError("owner sign-in issues owner codes: OSTRELAY_ENROLLMENT must include owner_codes")
            if self.oidc_signing_alg not in ("RS256", "RS384", "RS512", "PS256", "ES256", "ES384", "EdDSA"):
                raise ValueError("OSTRELAY_OIDC_SIGNING_ALG must be an asymmetric JWS algorithm")
        import ipaddress
        for entry in self.trusted_proxy_list():
            network = ipaddress.ip_network(entry, strict=False)
            if network.prefixlen == 0:
                raise ValueError("trusted proxy list must name specific proxies, not every address")
        return self

    def trusted_proxy_list(self) -> list[str]:
        return [entry.strip() for entry in self.trusted_proxy_ips.split(",") if entry.strip()]


_settings: Settings | None = None


def settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()  # type: ignore[call-arg]
    return _settings
