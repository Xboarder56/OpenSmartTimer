#!/usr/bin/env python3
"""Create the clean, target-only PlatformIO framework package."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--lock", required=True, type=Path)
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    target = args.raw / "esp32c3"
    for required in ("package.json", "tools.json", "versions.txt"):
        if not (args.raw / required).is_file():
            raise RuntimeError(f"raw framework is missing {required}")
    for required in ("sdkconfig", "dependencies.lock", "pioarduino-build.py"):
        if not (target / required).is_file():
            raise RuntimeError(f"raw ESP32-C3 framework is missing {required}")

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    shutil.copy2(args.raw / "tools.json", args.output / "tools.json")
    shutil.copytree(target, args.output / "esp32c3")

    package = json.loads((args.raw / "package.json").read_text(encoding="utf-8"))
    package["version"] = lock["package_version"]
    (args.output / "package.json").write_text(
        json.dumps(package, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    sdkconfig_hash = sha256(args.output / "esp32c3/sdkconfig")
    dependency_hash = sha256(args.output / "esp32c3/dependencies.lock")
    profile = {
        "schema": lock["schema"],
        "profile": lock["profile"],
        "builder_commit": lock["builder_commit"],
        "arduino_commit": lock["arduino_commit"],
        "esp_idf_commit": lock["esp_idf_commit"],
        "tinyusb_commit": lock["tinyusb_commit"],
        "builder_image": lock["builder_image"],
        "source_date_epoch": lock["source_date_epoch"],
        "sdkconfig_sha256": sdkconfig_hash,
        "dependencies_lock_sha256": dependency_hash,
    }
    (args.output / "opensmarttimer-framework.json").write_text(
        json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "versions.txt").write_text(
        "\n".join(
            (
                f"profile: {lock['profile']}",
                f"lib-builder: {lock['builder_commit']}",
                f"arduino: {lock['arduino_commit']}",
                f"esp-idf: {lock['esp_idf_commit']}",
                f"tinyusb: {lock['tinyusb_commit']}",
                f"sdkconfig-sha256: {sdkconfig_hash}",
                f"dependencies-lock-sha256: {dependency_hash}",
                "",
            )
        ),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
