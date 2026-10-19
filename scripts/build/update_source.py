"""Bind shipped HTML connection policies to the public firmware update source."""
from pathlib import Path
import re

POLICY_MARKER = b"connect-src 'self' https://*.github.io https://api.github.com;"


def patch_update_policy(data: bytes, root: Path) -> bytes:
    header = (root / 'include/ReleaseUpdateConfig.h').read_text()
    matches = re.findall(r'^#define SMART_TIMER_UPDATE_BASE_URL "([^"\n]*)"$', header, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError('missing or duplicated public update source')
    base = matches[0]
    if base and not re.fullmatch(r'https://[a-z0-9-]+\.github\.io/[A-Za-z0-9_.-]+/', base):
        raise ValueError('invalid public update source')
    if data.count(POLICY_MARKER) != 1:
        raise ValueError('missing or duplicated HTML update policy marker')
    policy = b"connect-src 'self'" + (b' ' + base.encode() if base else b'') + b' https://api.github.com;'
    return data.replace(POLICY_MARKER, policy)
