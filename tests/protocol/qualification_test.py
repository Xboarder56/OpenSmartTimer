#!/usr/bin/env python3
"""Qualification rejection cases; synthetic records, no timers or network."""
import copy
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
VERSION = re.search(r'(?m)^firmware_version\s*=\s*(\S+)$', (ROOT / 'platformio.ini').read_text())[1]
spec = importlib.util.spec_from_file_location('qualification', ROOT / 'scripts/release/verify_qualification.py')
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


def record():
    # The record for the configured version doubles as the fixture's shape.
    value = json.loads((ROOT / 'docs/qualification' / f'{VERSION}.json').read_text())
    for image in value['artifacts']:
        for check in image['checks'].values():
            check['status'] = 'passed'
        image['checks']['mixed_transport_soak']['duration_seconds'] = 7200
    value['checks']['companion_phone_pwa']['status'] = 'passed'
    value['qualification'] = 'hardware-qualified'
    return value


class QualificationTests(unittest.TestCase):
    def test_complete_record_is_accepted(self):
        self.assertEqual(qualification.validate(record()), [])

    def test_partial_record_allowed_for_draft_but_not_publication(self):
        value = record()
        value['qualification'] = 'partial'
        value['artifacts'][0]['checks']['physical_safety']['status'] = 'pending'
        self.assertTrue(qualification.validate(value, allow_pending=True))
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            qualification.validate(value)

    def test_failed_check_cannot_claim_qualification_even_in_draft_mode(self):
        value = record()
        value['artifacts'][1]['checks']['relay_qualification']['status'] = 'failed'
        with self.assertRaisesRegex(ValueError, 'cannot claim'):
            qualification.validate(value, allow_pending=True)

    def test_release_candidate_needs_physical_safety_and_no_failures(self):
        value = record()
        value['qualification'] = 'partial'
        value['checks']['companion_phone_pwa']['status'] = 'pending'
        self.assertTrue(qualification.validate(value, release_candidate=True))
        unchecked = copy.deepcopy(value)
        unchecked['artifacts'][1]['checks']['physical_safety']['status'] = 'pending'
        with self.assertRaisesRegex(ValueError, 'physical_safety must pass'):
            qualification.validate(unchecked, release_candidate=True)
        failed = copy.deepcopy(value)
        failed['artifacts'][0]['checks']['network_recovery']['status'] = 'failed'
        with self.assertRaisesRegex(ValueError, 'failed checks'):
            qualification.validate(failed, release_candidate=True)

    def test_short_soak_cannot_be_marked_passed(self):
        value = record()
        value['artifacts'][0]['checks']['mixed_transport_soak']['duration_seconds'] = 7199
        with self.assertRaisesRegex(ValueError, 'two hours'):
            qualification.validate(value, allow_pending=True)

    def test_missing_physical_check_is_not_silently_skipped(self):
        value = record()
        del value['artifacts'][0]['checks']['physical_safety']
        with self.assertRaisesRegex(ValueError, 'required checks missing'):
            qualification.validate(value)

    def test_duplicate_target_and_crossed_model_rejected(self):
        value = record()
        value['artifacts'][1] = copy.deepcopy(value['artifacts'][0])
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            qualification.validate(value)
        value = record()
        value['artifacts'][0]['hardware_model'] = 'MagBound Smart Timer V2'
        with self.assertRaisesRegex(ValueError, 'Crossed'):
            qualification.validate(value)

    def test_wrong_version_or_unsafe_artifact_name_rejected(self):
        value = record()
        value['artifacts'][0]['file'] = '../image.bin'
        with self.assertRaisesRegex(ValueError, 'artifact name'):
            qualification.validate(value)

    def make_bundle(self, directory):
        value = record()
        for index, image in enumerate(value['artifacts']):
            raw = b'synthetic non-flashable fixture' + bytes([index])
            image.update(size_bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                         crc32=f'{zlib.crc32(raw) & 0xffffffff:08x}')
            (directory / image['file']).write_bytes(raw)
        build = {'firmware_version': value['firmware_version'], 'source': value['source'],
                 'artifacts': [{k: v for k, v in image.items() if k != 'checks'} for image in value['artifacts']]}
        (directory / 'BUILD.json').write_text(json.dumps(build))
        return value, build

    def test_exact_bundle_and_actual_bytes_are_verified(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            value, _ = self.make_bundle(directory)
            self.assertEqual(qualification.validate(value, bundle=directory), [])
            image = directory / value['artifacts'][0]['file']
            raw = bytearray(image.read_bytes())
            raw[0] ^= 1
            image.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                qualification.validate(value, bundle=directory)

    def test_ci_images_compared_by_bytes_not_source_commit(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            value, build = self.make_bundle(directory)
            build['source'] = {'repository': 'example-owner/OpenSmartTimer', 'revision': 'a' * 40}
            (directory / 'BUILD.json').write_text(json.dumps(build))
            self.assertEqual(qualification.validate(value, bundle=directory, compare_source=False), [])
            image = directory / value['artifacts'][1]['file']
            image.write_bytes(image.read_bytes() + b'!')
            with self.assertRaisesRegex(ValueError, 'size mismatch|SHA-256 mismatch'):
                qualification.validate(value, bundle=directory, compare_source=False)

    def test_new_build_and_source_cannot_reuse_old_qualification(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            value, build = self.make_bundle(directory)
            build['source'] = {'repository': 'example/project', 'revision': 'a' * 40}
            (directory / 'BUILD.json').write_text(json.dumps(build))
            with self.assertRaisesRegex(ValueError, 'source'):
                qualification.validate(value, bundle=directory)
            build['source'] = None
            build['artifacts'][0]['build_id'] = 'b' * 24
            (directory / 'BUILD.json').write_text(json.dumps(build))
            with self.assertRaisesRegex(ValueError, 'build_id'):
                qualification.validate(value, bundle=directory)


if __name__ == '__main__':
    unittest.main()
