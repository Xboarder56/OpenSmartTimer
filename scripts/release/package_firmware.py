#!/usr/bin/env python3
"""Stage verified OpenSmartTimer application images in the canonical dist tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
VERSION_MATCH = re.search(
    r"(?m)^firmware_version\s*=\s*([0-9A-Za-z][0-9A-Za-z.-]*)\s*$",
    PLATFORMIO,
)
if VERSION_MATCH is None:
    raise RuntimeError("platformio.ini must define firmware_version")
VERSION = VERSION_MATCH.group(1)

ARTIFACTS = (
    (
        "MagBound Smart Timer V1",
        "ESP8266",
        ROOT / ".pio/build/v1/magbound_v1_esp8266"
        / f"OpenSmartTimer-{VERSION}-MagBoundV1-ESP8266.bin",
    ),
    (
        "MagBound Smart Timer V2",
        "ESP32-C3",
        ROOT / ".pio/build/v2/magbound_v2_esp32c3"
        / f"OpenSmartTimer-{VERSION}-MagBoundV2-ESP32C3.bin",
    ),
)


def build_id_for(source: Path) -> str:
    header = source.parent / "generated_build_identity.h"
    match = re.search(
        r'^#define SMART_TIMER_BUILD_ID "([0-9a-f]{24})"$',
        header.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if match is None:
        raise RuntimeError(f"missing or malformed build identity: {header}")
    return match.group(1)


def builder_paths() -> list[bytes]:
    """This checkout's and this user's absolute paths.

    scripts/build/build_paths.py rewrites them at compile time; finding one in
    an image or symbol file means that rewrite did not apply, and the file
    would publish where it was built.
    """
    found = set()
    for path, suffix in ((ROOT, ""), (Path.home(), "/")):
        for variant in {str(path), str(path.resolve())}:
            if len(variant.strip("/")) > 1:
                found.add((variant + suffix).encode())
    return sorted(found)


def leaked_path(data: bytes) -> str | None:
    for path in builder_paths():
        if path in data:
            return path.decode(errors="replace")
    return None


def digest(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "file": path.name,
        "size_bytes": len(data),
        "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08x}",
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output", type=Path, default=ROOT / "dist/firmware",
        help="Canonical firmware output directory",
    )
    parser.add_argument("--repository", help="CI source owner/repository")
    parser.add_argument("--revision", help="CI source full commit SHA")
    args = parser.parse_args()
    if bool(args.repository) != bool(args.revision):
        parser.error("--repository and --revision must be supplied together")
    source = None
    if args.repository:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
            parser.error("invalid source repository")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", args.revision):
            parser.error("revision must be a full Git commit SHA")
        source = {"repository": args.repository, "revision": args.revision.lower()}

    missing = [candidate for _, _, path in ARTIFACTS
               for candidate in (path, path.with_suffix(".elf")) if not candidate.is_file()]
    if missing:
        raise RuntimeError(
            "missing built firmware: " + ", ".join(str(path) for path in missing)
        )

    for _, _, path in ARTIFACTS:
        for candidate in (path, path.with_suffix(".elf")):
            leaked = leaked_path(candidate.read_bytes())
            if leaked:
                raise RuntimeError(
                    f"{candidate.name} contains the build path {leaked}; check that "
                    "scripts/build/build_paths.py is in its extra_scripts"
                )

    output = args.output
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    records = []
    for model, chip, built_image in ARTIFACTS:
        target = output / built_image.name
        shutil.copy2(built_image, target)
        record = digest(target)
        # Symbols for decoding crash addresses from this exact image. Not
        # flashable; excluded from SHA256SUMS/CRC32SUMS and the Pages flasher.
        symbols = output / built_image.with_suffix(".elf").name
        shutil.copy2(built_image.with_suffix(".elf"), symbols)
        record.update({
            "hardware_model": model,
            "chip_family": chip,
            "build_id": build_id_for(built_image),
            "symbols": {key: value for key, value in digest(symbols).items() if key != "crc32"},
        })
        records.append(record)

    checksum_lines = [f"{record['sha256']}  {record['file']}" for record in records]
    (output / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="utf-8"
    )
    (output / "CRC32SUMS").write_text(
        "".join(f"{record['crc32']}  {record['file']}\n" for record in records),
        encoding="utf-8",
    )
    (output / "BUILD.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "firmware_version": VERSION,
                "qualification": "software-verified-only",
                "source": source,
                "artifacts": records,
            },
            indent=2,
            sort_keys=True,
        ) + "\n",
        encoding="utf-8",
    )
    for record in records:
        print(
            f"staged {record['file']}: size={record['size_bytes']} "
            f"crc32={record['crc32']} sha256={record['sha256']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
