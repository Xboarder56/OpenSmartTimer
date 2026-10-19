"""Bounded, unambiguous wire decoding shared by the host prototype."""

import base64
import hmac
import json
import re
from typing import Any

SCOPES = {"view", "start", "release", "pause_resume", "adjust", "lock", "identify"}
MAX_UINT = (1 << 53) - 1  # Browser-safe integer range.


def ascii_equal(left: str, right: str) -> bool:
    """Constant-time comparison of valid ASCII credentials; reject malformed input."""
    try:
        return hmac.compare_digest(left.encode("ascii"), right.encode("ascii"))
    except UnicodeEncodeError:
        return False


def b64encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def b64decode(value: str, size: int | None = None) -> bytes:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise ValueError("invalid base64url")
    raw = base64.b64decode(value + "=" * (-len(value) % 4), altchars=b"-_", validate=True)
    if b64encode(raw) != value or (size is not None and len(raw) != size):
        raise ValueError("noncanonical base64url or length")
    return raw


def integer(value: Any, minimum: int = 0) -> bool:
    return type(value) is int and minimum <= value <= MAX_UINT


def scopes_valid(value: Any) -> bool:
    return (isinstance(value, list) and 1 <= len(value) <= len(SCOPES)
            and all(isinstance(s, str) and s in SCOPES for s in value)
            and len(set(value)) == len(value) and "view" in value)


def loads(raw: str | bytes, max_bytes: int) -> dict[str, Any]:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result

    def reject_constant(_):
        raise ValueError("nonfinite JSON number")

    if len(raw.encode("utf-8") if isinstance(raw, str) else raw) > max_bytes:
        raise ValueError("frame too large")
    obj = json.loads(raw, object_pairs_hook=unique, parse_constant=reject_constant)
    if not isinstance(obj, dict):
        raise ValueError("expected object")
    def check(value, depth=0):
        if depth > 16:
            raise ValueError("JSON nesting too deep")
        if isinstance(value, dict):
            for v in value.values():
                check(v, depth + 1)
        elif isinstance(value, list):
            for v in value:
                check(v, depth + 1)
        elif isinstance(value, float):
            import math
            if not math.isfinite(value):
                raise ValueError("nonfinite JSON number")
    check(obj)
    return obj
