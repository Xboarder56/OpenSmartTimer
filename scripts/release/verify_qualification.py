#!/usr/bin/env python3
"""Validate public qualification records and optionally the exact local bundle.

Records may describe a partial candidate. Drafting accepts honest pending checks;
publication verification requires all checks passed and exact artifact hashes.
This validates recorded evidence, not the truth of a human hardware attestation.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    'ESP8266': ('MagBound Smart Timer V1', 'MagBoundV1-ESP8266'),
    'ESP32-C3': ('MagBound Smart Timer V2', 'MagBoundV2-ESP32C3'),
}
COMMON_CHECKS = {'rest_sse', 'signed_http_ota', 'mixed_transport_soak',
                 'physical_safety', 'network_recovery', 'ota_recovery_vendor_rollback'}
V2_CHECKS = {'bluetooth_approval_recovery', 'signed_bluetooth_ota', 'relay_qualification'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(record, *, allow_pending=False, bundle=None, release_candidate=False, compare_source=True):
    require(isinstance(record, dict) and record.get('schema') == 1, 'Unsupported qualification schema')
    version = record.get('firmware_version', '')
    require(isinstance(version, str) and re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?', version), 'Invalid firmware version')
    require(record.get('qualification') in {'partial', 'hardware-qualified'}, 'Invalid qualification label')
    require(isinstance(record.get('recorded_utc'), str) and re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', record['recorded_utc']), 'Missing recorded UTC timestamp')
    source = record.get('source')
    require(source is None or (isinstance(source, dict) and re.fullmatch(r'[0-9a-f]{40}', source.get('revision', ''))
                              and re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', source.get('repository', ''))), 'Invalid source provenance')
    artifacts = record.get('artifacts')
    require(isinstance(artifacts, list) and len(artifacts) == 2, 'Exactly one V1 and one V2 record required')
    seen = set()
    pending = []

    def checks(values, required, label):
        require(isinstance(values, dict) and required <= values.keys(), f'{label}: required checks missing')
        for name, check in values.items():
            require(isinstance(check, dict) and check.get('status') in {'passed', 'pending', 'failed'}, f'{label}/{name}: invalid status')
            require(isinstance(check.get('summary'), str) and check['summary'].strip(), f'{label}/{name}: missing summary')
            for digest in check.get('evidence_sha256', []):
                require(isinstance(digest, str) and re.fullmatch(r'[0-9a-f]{64}', digest), f'{label}/{name}: invalid evidence digest')
            if check['status'] != 'passed':
                pending.append(f'{label}/{name}: {check["status"]}')
            elif name == 'mixed_transport_soak':
                duration = check.get('duration_seconds')
                require(isinstance(duration, (int, float)) and not isinstance(duration, bool) and duration >= 7200,
                        f'{label}: a passed release soak needs at least two hours')

    for image in artifacts:
        require(isinstance(image, dict), 'Invalid artifact record')
        chip = image.get('chip_family')
        require(chip in TARGETS and chip not in seen, 'Duplicate or unknown hardware target')
        seen.add(chip)
        model, suffix = TARGETS[chip]
        require(image.get('hardware_model') == model, 'Crossed hardware model/chip')
        require(image.get('file') == f'OpenSmartTimer-{version}-{suffix}.bin', 'Wrong artifact name/version/model')
        require(isinstance(image.get('build_id'), str) and re.fullmatch(r'[0-9a-f]{24}', image['build_id']), 'Invalid build ID')
        require(isinstance(image.get('sha256'), str) and re.fullmatch(r'[0-9a-f]{64}', image['sha256']), 'Invalid SHA-256')
        require(isinstance(image.get('crc32'), str) and re.fullmatch(r'[0-9a-f]{8}', image['crc32']), 'Invalid CRC32')
        require(type(image.get('size_bytes')) is int and image['size_bytes'] > 0, 'Invalid image size')
        checks(image.get('checks'), COMMON_CHECKS | (V2_CHECKS if chip == 'ESP32-C3' else set()), chip)
    checks(record.get('checks'), {'companion_phone_pwa'}, 'release')
    require(record['qualification'] != 'hardware-qualified' or not pending, 'A partial/failed record cannot claim hardware qualification')
    if release_candidate:
        # A prerelease reaches every timer: other checks may be pending, but the
        # physical emergency check must have passed and nothing may have failed.
        failed = [item for item in pending if item.endswith(': failed')]
        require(not failed, 'Release candidate has failed checks: ' + '; '.join(failed))
        for image in artifacts:
            require(image['checks']['physical_safety']['status'] == 'passed',
                    f"{image['chip_family']}: physical_safety must pass before a release candidate")
    elif not allow_pending:
        require(not pending and record['qualification'] == 'hardware-qualified', 'Qualification incomplete: ' + '; '.join(pending))
    if bundle is not None:
        build = json.loads((bundle / 'BUILD.json').read_text())
        require(build.get('firmware_version') == version, 'Bundle version does not match qualification')
        if compare_source:
            require(build.get('source') == source, 'Bundle source does not match qualification')
        actual = {entry['chip_family']: entry for entry in build['artifacts']}
        require(set(actual) == set(TARGETS), 'Bundle needs both hardware targets')
        for image in artifacts:
            packaged = actual[image['chip_family']]
            for field in ('file', 'build_id', 'sha256', 'crc32', 'size_bytes', 'hardware_model'):
                require(packaged.get(field) == image[field], f'Bundle {field} differs from qualified bytes')
            raw = (bundle / image['file']).read_bytes()
            require(len(raw) == image['size_bytes'], 'Bundle image size mismatch')
            require(hashlib.sha256(raw).hexdigest() == image['sha256'], 'Bundle image SHA-256 mismatch')
            require(f'{zlib.crc32(raw) & 0xffffffff:08x}' == image['crc32'], 'Bundle image CRC32 mismatch')
    return pending


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path, help='Qualification JSON; defaults to current firmware version')
    images = parser.add_mutually_exclusive_group()
    images.add_argument('--bundle', type=Path, help='Frozen/downloaded BUILD.json and actual image bytes')
    parser.add_argument('--allow-pending', action='store_true', help='Validate an honest partial record for source/draft checks')
    parser.add_argument('--release-candidate', action='store_true',
                        help='Prerelease: pending checks allowed, but physical_safety must pass and nothing may fail')
    images.add_argument('--images', type=Path,
                        help='Compare these images (e.g. a CI draft) byte for byte; their source commit may differ')
    args = parser.parse_args()
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(ROOT / 'platformio.ini')
    version = ini['opensmarttimer']['firmware_version']
    path = args.record or ROOT / 'docs/qualification' / f'{version}.json'
    record = json.loads(path.read_text())
    require(record.get('firmware_version') == version, 'Record does not match configured firmware version')
    pending = validate(record, allow_pending=args.allow_pending, bundle=args.bundle or args.images,
                       release_candidate=args.release_candidate, compare_source=args.images is None)
    if pending:
        print('Valid partial qualification record; NOT ready for publication.')
        for item in pending:
            print('PENDING:', item)
    else:
        checked = args.bundle or args.images
        print('Recorded hardware qualification complete' + (
            '; exact images verified.' if checked else '; supply --bundle or --images before publication.'))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (KeyError, OSError, ValueError, TypeError) as error:
        print(f'Qualification verification failed: {error}', file=sys.stderr)
        raise SystemExit(1)
