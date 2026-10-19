"""Owner codes: reusable, revocable per-owner enrollment codes.

``OSTRELAY_ENROLLMENT`` selects how timers may enroll (comma-separated):

* ``owner_codes`` (default) -- each owner has a reusable code and enrolls any
  of their timers themselves. The first owner is created on first start, so a
  self-hosted (homelab) relay needs no admin step at all; a shared relay's
  operator invites more owners and can rotate or revoke each one.
* ``open`` -- self-enrollment: the relay publishes its setup code at
  ``/setup`` (the browser's HTTPS check makes the key pin shown there
  trustworthy). Bounded by ``OSTRELAY_MAX_ENROLLED_TIMERS`` and a per-IP
  rate limit.

Single-use operator codes (``/admin/enroll-code``) always work.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import re
import secrets
from typing import Any

from . import db
from .config import settings

OWNER_CODE_PREFIX = "o1."
OPEN_ENROLLMENT_CODE = "open-enrollment"
FIRST_OWNER_NAME = "owner"
_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._-]{0,39}$")
# o1.<8-char owner id>.<43-char secret>, base64url; anything else is refused
# before hashing (never a server error).
_OWNER_CODE = re.compile(r"^o1\.([A-Za-z0-9_-]{8})\.([A-Za-z0-9_-]{43})$")


def _b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _secret_hash(secret: str) -> str:
    # The secret is 256 random bits, so a plain SHA-256 is enough at rest.
    return _b64(hashlib.sha256(secret.encode("ascii")).digest())


def valid_name(name: str) -> bool:
    return bool(_NAME.match(name or ""))


def _new_code(owner_id: str) -> tuple[str, str]:
    secret = _b64(secrets.token_bytes(32))
    return f"{OWNER_CODE_PREFIX}{owner_id}.{secret}", _secret_hash(secret)


def create_owner(name: str) -> str:
    """Create an owner and return their code (shown once; rotate to reissue)."""
    if not valid_name(name):
        raise ValueError("owner names are 1-40 letters, digits, spaces, '.', '_' or '-'")
    owner_id = _b64(secrets.token_bytes(6))
    code, digest = _new_code(owner_id)
    with db.transaction() as tx:
        if tx.execute("SELECT 1 FROM owners WHERE name=?", (name,)).fetchone():
            raise ValueError("an owner with that name already exists")
        if tx.execute("SELECT count(*) AS n FROM owners").fetchone()["n"] >= settings().max_owners:
            raise ValueError("owner_capacity")
        tx.execute("INSERT INTO owners (owner_id, name, secret_hash, created_at) VALUES (?,?,?,?)",
                   (owner_id, name, digest, db.now_unix()))
    return code


def rotate_owner(name: str) -> str:
    """Issue a new code; the old one stops enrolling. Enrolled timers stay."""
    with db.transaction() as tx:
        row = tx.execute("SELECT owner_id FROM owners WHERE name=? AND revoked=0", (name,)).fetchone()
        if row is None:
            raise ValueError("no active owner with that name")
        code, digest = _new_code(row["owner_id"])
        tx.execute("UPDATE owners SET secret_hash=? WHERE owner_id=?", (digest, row["owner_id"]))
    return code


def revoke_owner(name: str) -> list[str]:
    """Revoke an owner: their code stops working and their timers are removed
    (with their share links and sessions). Returns the removed timer IDs."""
    with db.transaction() as tx:
        row = tx.execute("SELECT owner_id FROM owners WHERE name=?", (name,)).fetchone()
        if row is None:
            raise ValueError("no owner with that name")
        timers = [r["timer_id"] for r in tx.execute(
            "SELECT timer_id FROM timers WHERE owner_id=?", (row["owner_id"],)).fetchall()]
        tx.execute("DELETE FROM timers WHERE owner_id=?", (row["owner_id"],))
        tx.execute("UPDATE owners SET revoked=1 WHERE owner_id=?", (row["owner_id"],))
    return timers


def list_owners() -> list[dict[str, Any]]:
    rows = db.connect().execute(
        "SELECT o.name, o.created_at, o.revoked, o.oidc_subject, o.email, "
        "(SELECT count(*) FROM timers t WHERE t.owner_id=o.owner_id) AS timers "
        "FROM owners o ORDER BY o.created_at").fetchall()
    return [{"name": r["name"], "created_at": r["created_at"], "revoked": bool(r["revoked"]),
             "timers": r["timers"], "signs_in": r["oidc_subject"] is not None, "email": r["email"]}
            for r in rows]


def _unique_name(tx: Any, wanted: str) -> str:
    base = re.sub(r"[^A-Za-z0-9 ._-]", "", wanted or "").strip(" ._-")[:32] or "owner"
    if not base[0].isalnum():
        base = "owner"
    name, n = base, 1
    while tx.execute("SELECT 1 FROM owners WHERE name=?", (name,)).fetchone():
        n += 1
        name = f"{base}-{n}"
    return name


def owner_for_sign_in(subject: str, email: str, display: str) -> dict[str, Any]:
    """The owner for a verified OIDC identity (``<issuer>|<sub>``), created on
    first sign-in without a usable code: the portal issues one on request."""
    with db.transaction() as tx:
        row = tx.execute("SELECT owner_id, name, revoked FROM owners WHERE oidc_subject=?", (subject,)).fetchone()
        if row is None:
            if tx.execute("SELECT count(*) AS n FROM owners").fetchone()["n"] >= settings().max_owners:
                raise ValueError("owner_capacity")
            owner_id = _b64(secrets.token_bytes(6))
            _code, digest = _new_code(owner_id)  # placeholder secret; never shown
            name = _unique_name(tx, display or email.split("@")[0])
            tx.execute("INSERT INTO owners (owner_id, name, secret_hash, created_at, oidc_subject, email) "
                       "VALUES (?,?,?,?,?,?)", (owner_id, name, digest, db.now_unix(), subject, email[:254]))
            return {"owner_id": owner_id, "name": name, "revoked": False}
        tx.execute("UPDATE owners SET email=? WHERE owner_id=?", (email[:254], row["owner_id"]))
        return {"owner_id": row["owner_id"], "name": row["name"], "revoked": bool(row["revoked"])}


def rotate_owner_id(owner_id: str) -> str:
    """New code for an owner by ID (the portal); refuses revoked owners."""
    with db.transaction() as tx:
        if tx.execute("SELECT 1 FROM owners WHERE owner_id=? AND revoked=0", (owner_id,)).fetchone() is None:
            raise ValueError("owner revoked")
        code, digest = _new_code(owner_id)
        tx.execute("UPDATE owners SET secret_hash=? WHERE owner_id=?", (digest, owner_id))
    return code


def owner_for_code(code: str, conn: Any = None) -> str | None:
    """Owner ID for a valid, unrevoked owner code; constant-time compare.

    Pass the enrollment transaction as ``conn`` so a concurrent revoke or
    rotate cannot land between this check and the new timer row."""
    match = _OWNER_CODE.fullmatch(code)
    if match is None:
        return None
    owner_id, secret = match.groups()
    row = (conn or db.connect()).execute(
        "SELECT owner_id, secret_hash FROM owners WHERE owner_id=? AND revoked=0", (owner_id,)).fetchone()
    expected = row["secret_hash"] if row else _secret_hash("x")
    if not hmac.compare_digest(_secret_hash(secret), expected) or row is None:
        return None
    return row["owner_id"]


def ensure_first_owner() -> str | None:
    """On a fresh relay with owner codes enabled, create the first owner and
    return their code (printed once at startup); otherwise None."""
    if "owner_codes" not in settings().enrollment_modes():
        return None
    if db.connect().execute("SELECT 1 FROM owners LIMIT 1").fetchone():
        return None
    return create_owner(FIRST_OWNER_NAME)
