#!/usr/bin/env python3
"""Validate a release tag, extract its changelog and verify exact CI artifacts.

Does not compile, flash, publish, sign, or claim hardware qualification.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import zlib

ROOT = Path(__file__).resolve().parents[2]
VERSION_PATTERN = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?"
TARGETS = (
    ("magbound_v1_esp8266", "MagBoundV1-ESP8266", "MagBound Smart Timer V1", "ESP8266"),
    ("magbound_v2_esp32c3", "MagBoundV2-ESP32C3", "MagBound Smart Timer V2", "ESP32-C3"),
)


def release_notes(tag: str, root: Path = ROOT) -> tuple[str, str]:
    if not re.fullmatch("v" + VERSION_PATTERN, tag):
        raise ValueError("release tag must be v<major>.<minor>.<patch>[-prerelease]")
    version = tag[1:]
    if "-" in version:
        identifiers = version.split("-", 1)[1].split(".")
        if any(not part or (part.isdigit() and len(part) > 1 and part[0] == "0")
               for part in identifiers):
            raise ValueError("invalid prerelease identifiers")
        if "dev" in identifiers:
            raise ValueError("development builds must not become GitHub Releases; choose an RC or stable version")
    config = configparser.ConfigParser(interpolation=None)
    config.read(root / "platformio.ini")
    if config["opensmarttimer"]["firmware_version"] != version:
        raise ValueError("release tag does not match platformio.ini firmware_version")
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    headings = list(re.finditer(r"(?m)^## \[([^\]\n]+)\][^\n]*$", changelog))
    matches = [i for i, heading in enumerate(headings) if heading[1] == version]
    if len(matches) != 1:
        raise ValueError(f"CHANGELOG.md must contain exactly one ## [{version}] section")
    index = matches[0]
    end = headings[index + 1].start() if index + 1 < len(headings) else len(changelog)
    notes = changelog[headings[index].end():end].strip()
    if not re.search(r"(?m)^- \S", notes):
        raise ValueError("versioned changelog section must contain release notes, not only headings")
    return version, notes + "\n"


def read_regular(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing or unsafe release file: {path.name}")
    return path.read_bytes()


def published_notes(notes: str, repository: str, revision: str) -> str:
    """Release pages need absolute links to docs from their exact source commit."""
    def link(match: re.Match) -> str:
        target = match[2]
        # Only repository-relative file links; external and anchor links stay
        # as written. Root changelog links never need parent traversal.
        if re.fullmatch(r"(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+(?:#[A-Za-z0-9_.-]+)?", target) \
                and '..' not in target.split('/'):
            target = f"https://github.com/{repository}/blob/{revision}/{target}"
        return f"[{match[1]}]({target})"
    return re.sub(r"\[([^\]\n]+)\]\(([^)\s]+)\)", link, notes)


def manifest_for(bundle: Path, tag: str, repository: str, revision: str,
                 root: Path = ROOT) -> dict:
    version, _ = release_notes(tag, root)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be owner/repository")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", revision):
        raise ValueError("revision must be a full Git commit SHA")
    source = {"repository": repository, "revision": revision.lower()}
    build = json.loads(read_regular(bundle / "BUILD.json"))
    if not isinstance(build, dict) or type(build.get("schema")) is not int or \
            build.get("schema") != 1 or build.get("firmware_version") != version:
        raise ValueError("BUILD.json schema/version mismatch")
    if build.get("source") != source:
        raise ValueError("BUILD.json source does not match the release commit/repository")
    if build.get("qualification") != "software-verified-only":
        raise ValueError("unexpected qualification; this tool does not attest hardware testing")
    records = build.get("artifacts")
    if not isinstance(records, list) or len(records) != len(TARGETS):
        raise ValueError("release must contain exactly one V1 and one V2 image")
    artifacts = []
    for target, suffix, model, chip in TARGETS:
        name = f"OpenSmartTimer-{version}-{suffix}.bin"
        found = [record for record in records if isinstance(record, dict) and record.get("file") == name]
        if len(found) != 1:
            raise ValueError(f"missing or duplicate model image: {name}")
        record = found[0]
        if record.get("hardware_model") != model or record.get("chip_family") != chip:
            raise ValueError(f"model/chip mismatch: {name}")
        if not re.fullmatch(r"[0-9a-f]{24}", str(record.get("build_id", ""))):
            raise ValueError(f"invalid embedded build ID: {name}")
        data = read_regular(bundle / name)
        expected = {
            "size_bytes": len(data),
            "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08x}",
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        if not data or type(record.get("size_bytes")) is not int or any(
                record.get(key) != value for key, value in expected.items()):
            raise ValueError(f"image size/checksum mismatch: {name}")
        symbols_name = name[:-len(".bin")] + ".elf"
        symbols_data = read_regular(bundle / symbols_name)
        symbols = {
            "file": symbols_name,
            "size_bytes": len(symbols_data),
            "sha256": hashlib.sha256(symbols_data).hexdigest(),
        }
        if not symbols_data.startswith(b"\x7fELF") or record.get("symbols") != symbols:
            raise ValueError(f"symbols size/checksum mismatch: {symbols_name}")
        artifacts.append({
            "file": name, **expected, "build_id": record["build_id"],
            "target": target, "hardware_model": model, "chip_family": chip,
            "kind": "application",
            "url": f"https://github.com/{repository}/releases/download/{tag}/{name}",
            "symbols": {**symbols,
                        "url": f"https://github.com/{repository}/releases/download/{tag}/{symbols_name}"},
        })
    for filename, field in (("SHA256SUMS", "sha256"), ("CRC32SUMS", "crc32")):
        expected_text = "".join(f"{item[field]}  {item['file']}\n" for item in artifacts)
        if read_regular(bundle / filename).decode("utf-8") != expected_text:
            raise ValueError(f"{filename} does not match the images")
    allowed = {"BUILD.json", "SHA256SUMS", "CRC32SUMS", "release.json", "updates.json"}
    allowed.update(item["file"] for item in artifacts)
    allowed.update(item["symbols"]["file"] for item in artifacts)
    if any(path.name not in allowed or path.is_symlink() or not path.is_file()
           for path in bundle.iterdir()):
        raise ValueError("unexpected files/directories in release bundle")
    return {
        "schema": 1, "tag": tag, "version": version, "prerelease": "-" in version,
        "source": source, "qualification": "software-verified-only",
        "checksum_algorithms": {"crc32": "CRC-32/ISO-HDLC", "sha256": "SHA-256"},
        "artifacts": artifacts,
    }


def verify_bundle(bundle: Path, tag: str, repository: str, revision: str,
                  root: Path = ROOT) -> None:
    expected = manifest_for(bundle, tag, repository, revision, root)
    if json.loads(read_regular(bundle / "release.json")) != expected:
        raise ValueError("release.json does not match the exact images and release source")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "prepare", "verify"))
    parser.add_argument("--tag", required=True)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--repository")
    parser.add_argument("--revision")
    parser.add_argument("--notes", type=Path)
    args = parser.parse_args()
    try:
        version, notes = release_notes(args.tag)
        if args.action != "check":
            if not all((args.bundle, args.repository, args.revision)):
                parser.error("prepare/verify require --bundle, --repository and --revision")
            if args.action == "prepare":
                manifest = manifest_for(args.bundle, args.tag, args.repository, args.revision)
                # Never replace an existing candidate's metadata silently.
                with (args.bundle / "release.json").open("x", encoding="utf-8") as output:
                    output.write(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            verify_bundle(args.bundle, args.tag, args.repository, args.revision)
            notes = published_notes(notes, args.repository, args.revision)
        if args.notes:
            with args.notes.open("x", encoding="utf-8") as output:
                output.write(notes)
        print(f"Release {version}: {args.action} passed (not hardware qualification)")
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
