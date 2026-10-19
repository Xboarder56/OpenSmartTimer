#!/usr/bin/env python3
"""Ensure modern firmware routes stay documented and represented by the mock."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SOURCE = "\n".join(path.read_text(encoding="utf-8") for path in sorted((ROOT / "src").glob("Web*.cpp")))
NETWORK_SOURCE = (ROOT / "src" / "LocalNetwork.cpp").read_text(encoding="utf-8")
PREVIEW = (ROOT / "tools" / "preview_ui.py").read_text(encoding="utf-8")
DOCS = (ROOT / "docs" / "API.md").read_text(encoding="utf-8")


def routes(text):
    return set(re.findall(r'(/api/v1/[a-z0-9_./-]+)', text))


def main():
    firmware_routes = routes(SOURCE)
    missing_docs = sorted(firmware_routes - routes(DOCS))
    missing_preview = sorted(firmware_routes - routes(PREVIEW))
    if missing_docs:
        print(f"ERROR: undocumented API routes: {', '.join(missing_docs)}")
    if missing_preview:
        print(f"ERROR: routes absent from preview mock: {', '.join(missing_preview)}")
    blocking_scans = re.findall(r"scanNetworks\s*\(\s*\)", NETWORK_SOURCE)
    if blocking_scans:
        print("ERROR: blocking Wi-Fi scan call present in LocalNetwork.cpp")
    if missing_docs or missing_preview or blocking_scans:
        return 1
    print(f"API documentation check passed: {len(firmware_routes)} modern routes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
