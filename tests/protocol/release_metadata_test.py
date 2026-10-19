"""Synthetic package tests; fixtures are not flashable or hardware-test evidence."""
import copy
import fnmatch
import importlib.util
import io
import json
import shlex
import shutil
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'scripts/release/{name}.py')
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


release = module('release_metadata')
packager = module('package_firmware')
VERSION = '1.0.0-rc.1'
TAG = 'v' + VERSION
REPOSITORY = 'example/smart-timer'
REVISION = 'a' * 40


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / 'bundle'
        (self.root / 'platformio.ini').write_text('[opensmarttimer]\nfirmware_version = ' + VERSION)
        self.changelog = (
            '# Changelog\n\n## [Unreleased]\n\n- Not shipped.\n\n'
            f'## [{VERSION}] - 2026-09-27\n\n### Added\n\n- Candidate features.\n\n'
            '## [0.9.0]\n\n- Previous release.\n'
        )
        (self.root / 'CHANGELOG.md').write_text(self.changelog)
        inputs = []
        for target, suffix, model, chip in release.TARGETS:
            build = self.root / target
            build.mkdir()
            image = build / f'OpenSmartTimer-{VERSION}-{suffix}.bin'
            image.write_bytes(b'Synthetic unit fixture: ' + target.encode())
            image.with_suffix('.elf').write_bytes(b'\x7fELF synthetic symbols: ' + target.encode())
            (build / 'generated_build_identity.h').write_text(
                '#define SMART_TIMER_BUILD_ID "' + 'b' * 24 + '"\n')
            inputs.append((model, chip, image))
        with patch.object(packager, 'ARTIFACTS', inputs), patch.object(packager, 'VERSION', VERSION), \
                patch('sys.argv', ['package_firmware.py', '--output', str(self.bundle),
                                   '--repository', REPOSITORY, '--revision', REVISION]), redirect_stdout(io.StringIO()):
            self.assertEqual(packager.main(), 0)
        self.build = json.loads((self.bundle / 'BUILD.json').read_text())

    def test_packaging_refuses_build_paths(self):
        checkout = Path('/example/checkout')
        for leak in (b'/example/checkout', b'/example/checkout/src/main.cpp'):
            for suffix in ('.bin', '.elf'):
                build = self.root / ('leak' + suffix.replace('.', '-') + str(len(leak)))
                build.mkdir()
                image = build / 'image.bin'
                image.write_bytes(b'Synthetic image')
                image.with_suffix('.elf').write_bytes(b'\x7fELF synthetic symbols')
                image.with_suffix(suffix).write_bytes(b'prefix ' + leak + b' suffix')
                with patch.object(packager, 'ROOT', checkout), \
                        patch.object(packager, 'ARTIFACTS', [('Model', 'Chip', image)]), \
                        patch('sys.argv', ['package_firmware.py', '--output', str(self.root / 'out')]), \
                        self.assertRaisesRegex(RuntimeError, 'build path'):
                    packager.main()
        with patch.object(packager, 'ROOT', checkout):
            self.assertIsNone(packager.leaked_path(b'./src/main.cpp ~/x /platformio/packages'))
            home = str(Path.home()).encode()
            if len(home.strip(b'/')) > 1:
                self.assertIsNotNone(packager.leaked_path(home + b'/.platformio/x'))

    def manifest(self):
        return release.manifest_for(self.bundle, TAG, REPOSITORY, REVISION, self.root)

    def test_complete_bundle_and_notes(self):
        version, notes = release.release_notes(TAG, self.root)
        self.assertEqual(version, VERSION)
        self.assertEqual(notes, '### Added\n\n- Candidate features.\n')
        manifest = self.manifest()
        self.assertTrue(manifest['prerelease'])
        self.assertEqual(manifest['qualification'], 'software-verified-only')
        self.assertEqual(manifest['source']['revision'], REVISION)
        self.assertEqual(len(manifest['artifacts']), 2)
        for item in manifest['artifacts']:
            self.assertEqual(item['kind'], 'application')
            self.assertIn(f'/releases/download/{TAG}/', item['url'])
        (self.bundle / 'release.json').write_text(json.dumps(manifest))
        release.verify_bundle(self.bundle, TAG, REPOSITORY, REVISION, self.root)
        manifest['artifacts'][0]['url'] = 'https://untrusted.invalid/image.bin'
        (self.bundle / 'release.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'release.json'):
            release.verify_bundle(self.bundle, TAG, REPOSITORY, REVISION, self.root)

    def test_published_notes_link_to_the_release_source(self):
        notes = ('[Install](docs/INSTALLATION.md#recovery) [Guide](README.md) '
                 '[External](https://example.com/guide) [Section](#limits) '
                 '[Parent](../outside.md)')
        rendered = release.published_notes(notes, REPOSITORY, REVISION)
        base = f'https://github.com/{REPOSITORY}/blob/{REVISION}/'
        self.assertIn(f'[Install]({base}docs/INSTALLATION.md#recovery)', rendered)
        self.assertIn(f'[Guide]({base}README.md)', rendered)
        for unchanged in ('[External](https://example.com/guide)', '[Section](#limits)',
                          '[Parent](../outside.md)'):
            self.assertIn(unchanged, rendered)

    def test_crc_known_vector(self):
        path = self.root / 'vector.bin'
        path.write_bytes(b'123456789')
        self.assertEqual(packager.digest(path)['crc32'], 'cbf43926')

    def test_pages_download_selects_a_verifiable_release_bundle(self):
        # Exercise the actual Pages download selectors against a complete
        # synthetic published release. Omitting symbols must fail the same
        # verification that runs before deployment.
        workflow = (ROOT / '.github/workflows/pages.yml').read_text()
        command = workflow.split('gh release download ', 1)[1].split('\n      - name:', 1)[0]
        words = shlex.split(command.replace('\\\n', ' '))
        patterns = [words[index + 1] for index, word in enumerate(words) if word == '--pattern']
        self.assertTrue(patterns)
        (self.bundle / 'release.json').write_text(json.dumps(self.manifest()))
        with tempfile.TemporaryDirectory() as directory:
            downloaded = Path(directory)
            for source in self.bundle.iterdir():
                if any(fnmatch.fnmatchcase(source.name, pattern) for pattern in patterns):
                    shutil.copy2(source, downloaded / source.name)
            release.verify_bundle(downloaded, TAG, REPOSITORY, REVISION, self.root)

    def test_tag_and_changelog_must_match(self):
        for tag in ('main', '1.0.0-rc.1', 'v1.0.0', 'v01.0.0', 'v1.0.0-rc..1', 'v1.0.0-rc.01'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.release_notes(tag, self.root)
        for content in ('## [Unreleased]\n- Only work in progress.\n',
                        self.changelog + f'\n## [{VERSION}]\n- Duplicate.\n',
                        f'## [{VERSION}]\n\n### Added\n'):
            (self.root / 'CHANGELOG.md').write_text(content)
            with self.assertRaises(ValueError):
                release.release_notes(TAG, self.root)
        (self.root / 'platformio.ini').write_text('[opensmarttimer]\nfirmware_version = 1.0.0-dev')
        with self.assertRaisesRegex(ValueError, 'development builds'):
            release.release_notes('v1.0.0-dev', self.root)

    def test_metadata_corruption_is_rejected(self):
        variants = []
        for key, value in [('schema', 2), ('source', None), ('firmware_version', '1.0.0'),
                           ('qualification', 'hardware-tested')]:
            bad = copy.deepcopy(self.build)
            bad[key] = value
            variants.append(bad)
        for key, value in [('sha256', '0' * 64), ('crc32', '00000000'), ('size_bytes', 1),
                           ('build_id', 'unknown'), ('hardware_model', 'Wrong model'),
                           ('file', '../escape.bin')]:
            bad = copy.deepcopy(self.build)
            bad['artifacts'][0][key] = value
            variants.append(bad)
        bad = copy.deepcopy(self.build)
        bad['artifacts'][1] = bad['artifacts'][0]
        variants.append(bad)
        for bad in variants:
            (self.bundle / 'BUILD.json').write_text(json.dumps(bad))
            with self.assertRaises(ValueError):
                self.manifest()

    def test_wrong_source_is_rejected(self):
        for repo, revision in [(REPOSITORY, 'b' * 40), ('different/repo', REVISION),
                               (REPOSITORY, 'main'), ('bad/repo/extra', REVISION)]:
            with self.assertRaises(ValueError):
                release.manifest_for(self.bundle, TAG, repo, revision, self.root)

    def test_files_and_sums_are_verified(self):
        for name in ['SHA256SUMS', 'CRC32SUMS', self.build['artifacts'][0]['file'],
                     self.build['artifacts'][1]['symbols']['file']]:
            path = self.bundle / name
            original = path.read_bytes()
            path.write_bytes(original + b'corruption')
            with self.assertRaises(ValueError):
                self.manifest()
            path.write_bytes(original)
        unknown = self.bundle / 'bootloader.bin'
        unknown.write_bytes(b'Not an application image')
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            self.manifest()
        unknown.unlink()
        # Symbols are required and must be ELF, not a renamed image.
        symbols = self.bundle / self.build['artifacts'][0]['symbols']['file']
        original = symbols.read_bytes()
        symbols.unlink()
        with self.assertRaises((ValueError, OSError)):
            self.manifest()
        symbols.write_bytes(b'Not an ELF file')
        with self.assertRaisesRegex(ValueError, 'symbols'):
            self.manifest()
        symbols.write_bytes(original)
        manifest = self.manifest()
        for item in manifest['artifacts']:
            self.assertTrue(item['symbols']['file'].endswith('.elf'))
            self.assertIn(f'/releases/download/{TAG}/', item['symbols']['url'])
            self.assertNotIn(item['symbols']['file'], (self.bundle / 'SHA256SUMS').read_text())
        image = self.bundle / self.build['artifacts'][0]['file']
        real = self.root / 'elsewhere.bin'
        image.rename(real)
        image.symlink_to(real)
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            self.manifest()


if __name__ == '__main__':
    unittest.main()
