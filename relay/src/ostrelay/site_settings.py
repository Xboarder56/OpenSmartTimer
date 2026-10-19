"""Persisted operator preferences, bounded by environment policy."""

from . import db
from .config import settings


def landing_page_settings() -> dict[str, bool]:
    row = db.connect().execute(
        "SELECT value FROM site_settings WHERE key = ?", ("landing_page_visible",)
    ).fetchone()
    visible = row is None or row["value"] == "true"
    allowed = settings().landing_page_enabled
    return {"landing_page_visible": visible,
            "landing_page_allowed": allowed,
            "landing_page_enabled": allowed and visible}


def set_landing_page_visible(visible: bool) -> dict[str, bool]:
    with db.transaction() as tx:
        tx.execute("INSERT INTO site_settings (key, value) VALUES (?, ?) "
                   "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                   ("landing_page_visible", "true" if visible else "false"))
    return landing_page_settings()


def open_enrollment_settings(tx=None) -> dict[str, bool]:
    connection = tx if tx is not None else db.connect()
    row = connection.execute(
        "SELECT value FROM site_settings WHERE key = ?", ("open_enrollment_requested",)
    ).fetchone()
    requested = row is None or row["value"] == "true"
    allowed = "open" in settings().enrollment_modes()
    return {"open_enrollment_requested": requested,
            "open_enrollment_allowed": allowed,
            "open_enrollment_enabled": allowed and requested}


def set_open_enrollment_requested(requested: bool) -> dict[str, bool]:
    with db.transaction() as tx:
        tx.execute("INSERT INTO site_settings (key, value) VALUES (?, ?) "
                   "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                   ("open_enrollment_requested", "true" if requested else "false"))
    return open_enrollment_settings()
