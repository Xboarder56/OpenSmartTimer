"""The TLS key that timers see when they reach this relay.

A timer does not validate the relay's certificate against a certificate
authority. During enrollment it pins the key named in the owner's setup code.
After that it binds the SHA-256 of the leaf certificate's SubjectPublicKeyInfo
into every hello, and the relay accepts a hello only when that digest is one of
its own. The relay therefore has to know which key its reverse proxy serves:

- ``OSTRELAY_SPKI_PIN`` names it explicitly (several pins may be separated by
  commas). Required for a private or self-signed certificate.
- Otherwise the relay connects to its own public URL, validates the
  certificate like any HTTPS client, and uses the key it is shown. It repeats
  this periodically and whenever a hello arrives bound to an unknown key, so a
  renewed certificate needs nothing from the operator or from timer owners.

Nothing here is a secret: a certificate's public key is public.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import logging
import socket
import ssl
import time
from urllib.parse import urlsplit

from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from .config import settings

log = logging.getLogger(__name__)

# Binding value for a connection without TLS. Only an explicit loopback
# development relay (DEV_MODE with an http:// public URL) accepts it.
NO_TLS = bytes(32)

REFRESH_INTERVAL_S = 600.0
RETRY_INTERVAL_S = 15.0
MIN_PROBE_GAP_S = 30.0
PROBE_TIMEOUT_S = 5.0

_probed: bytes | None = None
_last_attempt = 0.0
_probing = False


def _decode(pin: str) -> bytes:
    raw = base64.urlsafe_b64decode(pin + "=" * (-len(pin) % 4))
    if len(raw) != 32:
        raise ValueError("an SPKI pin is the base64url SHA-256 of a public key (32 bytes)")
    return raw


def encode(digest: bytes) -> str:
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


def configured() -> list[bytes]:
    """Pins the operator named in OSTRELAY_SPKI_PIN, in the order given."""
    return [_decode(part.strip()) for part in settings().spki_pin.split(",") if part.strip()]


def follows_certificate() -> bool:
    """True when the relay learns its key from the certificate actually served."""
    return not configured() and settings().public_url.startswith("https://")


def pins() -> list[bytes]:
    """Every key digest a hello may be bound to. Empty means none is known yet."""
    named = configured()
    if named:
        return named
    if _probed is not None:
        return [_probed]
    cfg = settings()
    if cfg.dev_mode and cfg.public_url.startswith("http://"):
        return [NO_TLS]
    return []


def current_pin() -> str:
    """The pin to put in a setup code; empty while unknown or without TLS."""
    named = configured()
    if named:
        return encode(named[0])
    return encode(_probed) if _probed is not None else ""


def probe() -> bytes:
    """Connect to the public URL as a client and return its leaf key digest.

    The certificate chain and hostname are validated, so the relay only ever
    vouches for a key that a public certificate authority issued for its name.
    """
    cfg = settings()
    url = urlsplit(cfg.public_url)
    hostname = url.hostname or ""
    address = cfg.tls_probe_address or f"{hostname}:{url.port or 443}"
    host, _, port = address.rpartition(":")
    context = ssl.create_default_context()
    with socket.create_connection((host.strip("[]"), int(port)), timeout=PROBE_TIMEOUT_S) as raw:
        with context.wrap_socket(raw, server_hostname=hostname) as tls:
            der = tls.getpeercert(binary_form=True)
    if not der:
        raise ssl.SSLError("the server presented no certificate")
    key = x509.load_der_x509_certificate(der).public_key()
    return hashlib.sha256(key.public_bytes(Encoding.DER, PublicFormat.SubjectPublicKeyInfo)).digest()


async def refresh(*, force: bool = False) -> bool:
    """Probe once, at most every MIN_PROBE_GAP_S. Returns whether a key is known."""
    global _probed, _last_attempt, _probing
    if not follows_certificate():
        return bool(pins())
    now = time.monotonic()
    if _probing or (not force and _last_attempt and now - _last_attempt < MIN_PROBE_GAP_S):
        return _probed is not None
    _probing = True
    _last_attempt = now
    try:
        digest = await asyncio.to_thread(probe)
    except (OSError, ValueError) as error:  # ssl.SSLError is an OSError
        log.warning("ostrelay: could not read the TLS key served at %s (%s); "
                    "timers cannot connect until it is known", settings().public_url, error)
        return _probed is not None
    finally:
        _probing = False
    if digest != _probed:
        log.info("ostrelay: TLS key served at %s is now %s", settings().public_url, encode(digest))
        _probed = digest
    return True


async def refresh_loop() -> None:
    """Keep the probed key current for the life of the process."""
    if not follows_certificate():
        return
    while True:
        known = await refresh(force=True)
        await asyncio.sleep(REFRESH_INTERVAL_S if known else RETRY_INTERVAL_S)


_tasks: set[asyncio.Task[bool]] = set()


def request_refresh() -> None:
    """Look again soon, without making the caller wait for the network."""
    task = asyncio.get_running_loop().create_task(refresh())
    _tasks.add(task)
    task.add_done_callback(_tasks.discard)


def reset_for_tests() -> None:
    global _probed, _last_attempt, _probing
    _probed = None
    _last_attempt = 0.0
    _probing = False
