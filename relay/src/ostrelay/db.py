"""SQLite persistence for ost-relay/1.

Deliberately small. Rows the relay stores are:

* enrollment codes — one-shot, admin-created strings a timer trades for
  a `timer_id` + `enrollment_secret`;
* timers — one row per enrolled timer with the credentials the relay
  needs to authenticate the WSS `hello`;
* tokens — one row per capability token the timer minted (identified by
  ``fp_hash`` plus an exact wire-token digest);
* sessions — public browser routing handles, fingerprint hashes and expiry.

State that must be per-connection (queued command futures, subscriber
queues, pending public request IDs) is kept in-process in :mod:`ws.py`, not
here.
"""

from __future__ import annotations

import sqlite3
import os
from pathlib import Path
import time
import threading
from contextlib import contextmanager
from typing import Iterator

from .config import settings


SCHEMA = """
CREATE TABLE IF NOT EXISTS site_settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS enrollment_codes (
    code           TEXT PRIMARY KEY,
    created_at     INTEGER NOT NULL,
    consumed_at    INTEGER
);

CREATE TABLE IF NOT EXISTS owners (
    owner_id             TEXT PRIMARY KEY,
    name                 TEXT NOT NULL UNIQUE,
    secret_hash          TEXT NOT NULL,           -- base64url SHA-256 of the owner secret
    created_at           INTEGER NOT NULL,
    revoked              INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS owner_sessions (
    session_hash         TEXT PRIMARY KEY,        -- SHA-256 of the cookie value
    owner_id             TEXT NOT NULL REFERENCES owners(owner_id) ON DELETE CASCADE,
    created_at           INTEGER NOT NULL,
    expires_at           INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS timers (
    timer_id             TEXT PRIMARY KEY,
    enrollment_secret    BLOB NOT NULL,           -- 32 bytes; shared with timer
    hardware_model       TEXT NOT NULL,
    chip_family          TEXT NOT NULL,
    build_id             TEXT NOT NULL,
    spki_sha256_pin      TEXT NOT NULL,           -- what the timer will pin against us
    label                TEXT NOT NULL DEFAULT '',
    created_at           INTEGER NOT NULL,
    last_seen_at         INTEGER
);

CREATE TABLE IF NOT EXISTS tokens (
    -- Base64url SHA-256(fp)[:16]. Uniquely identifies a capability
    -- token in the registration table. Full tokens never reach this service.
    fp_hash              TEXT PRIMARY KEY,
    timer_id             TEXT NOT NULL REFERENCES timers(timer_id) ON DELETE CASCADE,
    token_digest         TEXT NOT NULL DEFAULT '',
    scope_json           TEXT NOT NULL,           -- JSON array of scope strings
    -- Expiry: {boot_id, exp_up_ms}. We compare boot_id and estimate
    -- monotonic elapsed time from the last status.uptime_ms we saw.
    boot_id              TEXT NOT NULL,
    exp_up_ms            INTEGER NOT NULL,
    label                TEXT NOT NULL DEFAULT '',
    minted_at            INTEGER NOT NULL,
    revoked              INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS ix_tokens_timer ON tokens(timer_id);

CREATE TABLE IF NOT EXISTS sessions (
    session_id           TEXT PRIMARY KEY,
    fp_hash              TEXT NOT NULL REFERENCES tokens(fp_hash) ON DELETE CASCADE,
    client_id           TEXT NOT NULL,           -- public browser routing handle
    created_at           INTEGER NOT NULL,
    expires_at           INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_sessions_fp ON sessions(fp_hash);
"""


_lock = threading.RLock()


class Result:
    """Materialize reads while the connection lock is held."""
    def __init__(self, cursor):
        self.rowcount = cursor.rowcount
        self._rows = list(cursor.fetchall()) if cursor.description else []
        cursor.close()

    def fetchone(self):
        return self._rows.pop(0) if self._rows else None

    def fetchall(self):
        rows, self._rows = self._rows, []
        return rows


class Database:
    def __init__(self, connection):
        self.connection = connection

    def execute(self, sql, params=()):
        with _lock:
            return Result(self.connection.execute(sql, params))

    def close(self):
        with _lock:
            self.connection.close()


_conn: Database | None = None


def connect() -> Database:
    """One serialized connection for the supported single-process deployment.

    FastAPI sync routes run in worker threads. Both whole transactions and
    individual reads use the same reentrant lock, including cursor fetches.
    """
    global _conn
    with _lock:
        if _conn is None:
            path = settings().db_path
            if path != ":memory:":
                # Enrollment secrets are plaintext in this file. Create it
                # privately before SQLite can create any WAL/SHM sidecars.
                fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
                try:
                    os.fchmod(fd, 0o600)
                finally:
                    os.close(fd)
                for suffix in ("-wal", "-shm", "-journal"):
                    sidecar = Path(path + suffix)
                    if sidecar.exists():
                        sidecar.chmod(0o600)
            raw = sqlite3.connect(settings().db_path, isolation_level=None,
                                  check_same_thread=False, timeout=5.0)
            raw.row_factory = sqlite3.Row
            raw.executescript("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;")
            raw.executescript(SCHEMA)
            session_columns = {r[1] for r in raw.execute("PRAGMA table_info(sessions)")}
            if "wire_token_enc" in session_columns:
                # Unreleased contract reset: do not retain old bearer blobs.
                raw.executescript("DROP TABLE sessions; CREATE TABLE sessions ("
                    "session_id TEXT PRIMARY KEY, fp_hash TEXT NOT NULL REFERENCES tokens(fp_hash) ON DELETE CASCADE, "
                    "client_id TEXT NOT NULL, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL); "
                    "CREATE INDEX ix_sessions_fp ON sessions(fp_hash);")
            timer_columns = {r[1] for r in raw.execute("PRAGMA table_info(timers)")}
            if "owner_id" not in timer_columns:
                # Timers enrolled before owner codes belong to no owner.
                raw.execute("ALTER TABLE timers ADD COLUMN owner_id TEXT")
            if "enrolled_via" not in timer_columns:
                # 'owner', 'open' or 'code'; older rows count as operator codes.
                raw.execute("ALTER TABLE timers ADD COLUMN enrolled_via TEXT NOT NULL DEFAULT 'code'")
            owner_columns = {r[1] for r in raw.execute("PRAGMA table_info(owners)")}
            if "oidc_subject" not in owner_columns:
                # "<issuer>|<sub>" for owners who sign in; NULL for invited owners.
                raw.execute("ALTER TABLE owners ADD COLUMN oidc_subject TEXT")
                raw.execute("ALTER TABLE owners ADD COLUMN email TEXT NOT NULL DEFAULT ''")
            raw.execute("CREATE UNIQUE INDEX IF NOT EXISTS owners_oidc_subject ON owners(oidc_subject) "
                        "WHERE oidc_subject IS NOT NULL")
            columns = {r[1] for r in raw.execute("PRAGMA table_info(tokens)")}
            if "token_digest" not in columns:
                # Old prototype tokens cannot authenticate sessions until reminted.
                raw.execute("ALTER TABLE tokens ADD COLUMN token_digest TEXT NOT NULL DEFAULT ''")
            _conn = Database(raw)
        return _conn


@contextmanager
def transaction() -> Iterator[Database]:
    with _lock:
        conn = connect()
        conn.execute("BEGIN IMMEDIATE")
        try:
            yield conn
        except BaseException:
            conn.execute("ROLLBACK")
            raise
        else:
            conn.execute("COMMIT")


def cleanup() -> None:
    now = now_unix()
    with transaction() as conn:
        conn.execute("DELETE FROM sessions WHERE expires_at <= ? OR fp_hash IN "
                     "(SELECT fp_hash FROM tokens WHERE revoked = 1)", (now,))
        conn.execute("DELETE FROM enrollment_codes WHERE created_at < ?", (now - 900,))
        conn.execute("DELETE FROM owner_sessions WHERE expires_at <= ?", (now,))
        # Open enrollments that never connected hold no device: reclaim the
        # slot. A timer that has connected once is never removed for being
        # offline.
        conn.execute("DELETE FROM timers WHERE enrolled_via = 'open' AND last_seen_at IS NULL "
                     "AND created_at < ?", (now - settings().open_enrollment_unused_ttl_s,))


def now_unix() -> int:
    return int(time.time())
