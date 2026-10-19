#!/usr/bin/env python3
"""Install the pinned target-only framework archive without path traversal."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    lock = json.loads((ROOT / "framework/esp32c3/lock.json").read_text())
    actual = hashlib.sha256(args.archive.read_bytes()).hexdigest()
    if actual != lock["archive_sha256"]:
        raise RuntimeError(f"framework archive hash mismatch: {actual}")
    target_root = ROOT / ".framework"
    package = target_root / "esp32c3-libs"
    if target_root.exists():
        shutil.rmtree(target_root)
    target_root.mkdir()
    with tarfile.open(args.archive, "r:gz") as archive:
        archive.extractall(target_root, filter="data")
    if not package.is_dir():
        raise RuntimeError("framework archive has no esp32c3-libs package")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, tarfile.TarError) as error:
        print(f"framework installation failed: {error}")
        raise SystemExit(1)
