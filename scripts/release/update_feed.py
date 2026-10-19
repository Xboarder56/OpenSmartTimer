#!/usr/bin/env python3
"""Provision a public trust root, sign exact release images, or stage a Pages feed.

Private keys belong outside this repository and in the UPDATE_SIGNING_KEY GitHub
Actions secret. This tool never publishes, commits, or overwrites a signing key.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import zlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {"magbound_v1_esp8266", "magbound_v2_esp32c3"}
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?"


def public_hex(key):
    return key.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint).hex()


def config(root=ROOT):
    text = (root / "include/ReleaseUpdateConfig.h").read_text()
    def value(name):
        found = re.findall(r'^#define SMART_TIMER_UPDATE_' + name + r' "([^"\n]*)"$', text, re.M)
        if len(found) != 1:
            raise ValueError("missing/ambiguous update configuration: " + name)
        return found[0]
    repo, base, key = value("REPOSITORY"), value("BASE_URL"), value("PUBLIC_KEY")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("configure the GitHub update repository before releasing")
    owner, project = repo.split("/")
    if base != f"https://{owner.lower()}.github.io/{project}/":
        raise ValueError("update base URL must be the configured repository's GitHub Pages URL")
    if not re.fullmatch(r"04[0-9a-f]{128}", key):
        raise ValueError("public key must be the canonical uncompressed P-256 point")
    public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(key))
    return repo, base, public


def canonical(record):
    if record.get("target") not in TARGETS or not re.fullmatch(VERSION, record.get("release_version", "")):
        raise ValueError("invalid release target/version")
    version = record["release_version"]
    if len(version) >= 48 or any(int(n) > 0xffffffff for n in version.split('-', 1)[0].split('.')) or ("-" in version and any(
            not p or (p.isdigit() and len(p) > 1 and p.startswith("0")) for p in version.split("-", 1)[1].split("."))):
        raise ValueError("invalid release version")
    if type(record.get("image_size")) is not int or not 24 <= record["image_size"] <= 4194304:
        raise ValueError("invalid image size")
    if not re.fullmatch(r"[0-9a-f]{8}", record.get("image_crc32", "")) or not re.fullmatch(r"[0-9a-f]{64}", record.get("image_sha256", "")):
        raise ValueError("invalid image digest")
    return (f"OpenSmartTimer-Update-v1\n{record['target']}\n{version}\n"
            f"{record['image_size']}\n{record['image_crc32']}\n{record['image_sha256']}\n").encode("ascii")


def verify_record(record, public):
    if not re.fullmatch(r"[0-9a-f]{128}", record.get("release_signature", "")):
        raise ValueError("invalid release signature encoding")
    signature = bytes.fromhex(record.get("release_signature", ""))
    if len(signature) != 64:
        raise ValueError("invalid release signature length")
    public.verify(utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big")),
                  canonical(record), ec.ECDSA(hashes.SHA256()))
    expected_suffix = "MagBoundV1-ESP8266" if record["target"] == "magbound_v1_esp8266" else "MagBoundV2-ESP32C3"
    if record.get("file") != f"OpenSmartTimer-{record['release_version']}-{expected_suffix}.bin":
        raise ValueError("unexpected image filename")


def records(bundle, public):
    document = json.loads((bundle / "updates.json").read_bytes())
    feeds = document.get("artifacts", [])
    if document.get("schema") != 1 or len(feeds) != 2 or {r.get("target") for r in feeds} != TARGETS:
        raise ValueError("update manifest needs exactly one image per target")
    for record in feeds:
        verify_record(record, public)
        image = bundle / record["file"]
        if image.is_symlink() or not image.is_file():
            raise ValueError("missing or unsafe update image")
        data = image.read_bytes()
        if len(data) != record["image_size"] or hashlib.sha256(data).hexdigest() != record["image_sha256"] or f"{zlib.crc32(data):08x}" != record["image_crc32"]:
            raise ValueError("update image does not match its signed manifest")
    return feeds


def sign(bundle, private_path):
    repo, _, public = config()
    key = serialization.load_pem_private_key(private_path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1) or public_hex(key.public_key()) != public_hex(public):
        raise ValueError("signing key does not match the firmware's P-256 public key")
    release = json.loads((bundle / "release.json").read_bytes())
    # CI records github.repository, which keeps the account's display case.
    if release["source"]["repository"].lower() != repo.lower():
        raise ValueError("release repository differs from updater configuration")
    if release.get("qualification") != "software-verified-only" or 'dev' in release['version'].split('-', 1)[-1].split('.'):
        raise ValueError("only verified RC/stable release bundles may be signed")
    if len(release['artifacts']) != 2 or {a.get('target') for a in release['artifacts']} != TARGETS:
        raise ValueError("release needs exactly one image per model")
    feeds = []
    for artifact in release["artifacts"]:
        record = {"target": artifact["target"], "release_version": release["version"],
                  "image_size": artifact["size_bytes"], "image_crc32": artifact["crc32"],
                  "image_sha256": artifact["sha256"], "file": artifact["file"]}
        message = canonical(record)
        suffix = 'MagBoundV1-ESP8266' if record['target'] == 'magbound_v1_esp8266' else 'MagBoundV2-ESP32C3'
        if record['file'] != f"OpenSmartTimer-{record['release_version']}-{suffix}.bin":
            raise ValueError('unexpected release filename')
        path = bundle / record['file']
        if path.is_symlink() or not path.is_file(): raise ValueError('unsafe release image')
        data = path.read_bytes()
        if len(data) != record['image_size'] or hashlib.sha256(data).hexdigest() != record['image_sha256'] or f'{zlib.crc32(data):08x}' != record['image_crc32']:
            raise ValueError('release image differs from verified metadata')
        r, s = utils.decode_dss_signature(key.sign(message, ec.ECDSA(hashes.SHA256())))
        record["release_signature"] = (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex()
        feeds.append(record)
    with (bundle / "updates.json").open("x") as output:
        json.dump({"schema": 1, "artifacts": feeds}, output, indent=2, sort_keys=True)
        output.write("\n")
    records(bundle, public)


def stage(bundle, output, public, notes="", expected_version=None):
    verified = records(bundle, public)
    if expected_version is not None and any(r['release_version'] != expected_version for r in verified):
        raise ValueError('signed image version differs from the published tag')
    for record in verified:
        updates = output / "updates"
        destination = updates / "images" / record["file"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and destination.read_bytes() != (bundle / record["file"]).read_bytes():
            raise ValueError("refusing to overwrite a versioned image")
        shutil.copyfile(bundle / record["file"], destination)
        record={**record,"release_notes":notes.encode('utf-8')[:8192].decode('utf-8','ignore')}
        (updates / f"{record['target']}.json").write_text(json.dumps(record, separators=(",", ":"), sort_keys=True,ensure_ascii=False) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("check-config")
    generate = sub.add_parser("generate-key")
    generate.add_argument("--private-key", type=Path, required=True)
    configure = sub.add_parser("configure")
    configure.add_argument("--repository", required=True)
    configure.add_argument("--public-key", required=True, help="130 lowercase hex characters, uncompressed P-256 point")
    for action in ("sign", "verify", "stage"):
        command = sub.add_parser(action)
        command.add_argument("--bundle", type=Path, required=True)
        if action == "sign": command.add_argument("--private-key", type=Path, required=True)
        if action == "stage":
            command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "check-config":
        config()
    elif args.action == "generate-key":
        key = ec.generate_private_key(ec.SECP256R1())
        fd = os.open(args.private_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        print("Public key: " + public_hex(key.public_key()))
        print("Private key created with mode 0600; back it up outside the repository.")
    elif args.action == "configure":
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) or not re.fullmatch(r"04[0-9a-f]{128}", args.public_key):
            raise ValueError("invalid repository/public key")
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(args.public_key))
        owner, project = args.repository.split("/")
        (ROOT / "include/ReleaseUpdateConfig.h").write_text(
            '#pragma once\n\n// Public release configuration. Private signing keys never belong in source.\n'
            f'#define SMART_TIMER_UPDATE_REPOSITORY "{args.repository}"\n'
            f'#define SMART_TIMER_UPDATE_BASE_URL "https://{owner.lower()}.github.io/{project}/"\n'
            f'#define SMART_TIMER_UPDATE_PUBLIC_KEY "{args.public_key}"\n')
    elif args.action == "sign": sign(args.bundle, args.private_key)
    else:
        _, _, public = config()
        if args.action == "verify": records(args.bundle, public)
        else: stage(args.bundle, args.output, public)
    print("Update " + args.action + " passed; nothing published or flashed.")


if __name__ == "__main__":
    main()
