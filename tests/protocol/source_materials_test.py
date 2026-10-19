#!/usr/bin/env python3
"""Source bundles bind actual dependency bytes and exclude ignored bench state."""
import hashlib
import importlib.util
import io
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('source_materials', ROOT / 'scripts/release/package_source_materials.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class LockedRuntimeSourcesTest(unittest.TestCase):
    def test_managed_components_use_versioned_archives_not_generated_downloads(self):
        catalog = json.loads((ROOT / 'third-party/source-inputs.json').read_text())['archives']
        # The web download endpoint regenerates ZIP timestamps on every call.
        # Registry API metadata instead points at these immutable archives.
        for record in catalog:
            if record['name'] in {'mdns', 'network_provisioning', 'esp_diagnostics'}:
                name, version = record['name'], record['version']
                self.assertEqual(record['url'],
                                 f'https://components-file.espressif.com/components/espressif/{name}/{version}/espressif__{name}-v{version}.zip')

    def test_each_locked_relay_runtime_has_an_exact_preferred_source_input(self):
        runtime = json.loads((ROOT / 'third-party/inventory.json').read_text())['relay_runtime']
        catalog = json.loads((ROOT / 'third-party/source-inputs.json').read_text())['archives']
        sources = [r for r in catalog if r.get('group') == 'relay-runtime']
        self.assertEqual({(r['name'], r['version']) for r in sources},
                         {(r['name'], r['version']) for r in runtime})
        self.assertEqual(len(sources), len(runtime), 'no duplicate runtime source records')
        self.assertEqual(len({r['file'] for r in catalog}), len(catalog))
        for r in sources:
            self.assertTrue(r['url'].startswith('https://files.pythonhosted.org/'))
            self.assertRegex(r['sha256'], r'^[0-9a-f]{64}$')
            self.assertGreater(r['size_bytes'], 0)


class SourceMaterialsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        def write(name, data):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
            return path
        self.write = write
        write('.gitignore', 'tmp/\ndist/\n.pio-core/\n')
        write('platformio.ini', '[opensmarttimer]\nfirmware_version = 1.0.0-rc.2\n')
        write('LICENSE', 'Project license fixture\n')
        notice = write('third-party/notices/core/LICENSE', 'Preserved copyright\n')
        write('third-party/inventory.json', json.dumps({'notice_files': [{'file': 'third-party/notices/core/LICENSE', 'sha256': tool.sha256(notice)}]}))
        framework = write('dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz', 'SDK fixture')
        write('framework/esp32c3/lock.json', json.dumps({'archive_sha256': tool.sha256(framework), 'source_date_epoch': 1753085401}))
        source = write('dist/source-cache/idf.tar.gz', 'Pinned upstream fixture')
        self.record = {'file': source.name, 'size_bytes': source.stat().st_size, 'sha256': tool.sha256(source), 'url': 'https://codeload.github.com/test/source/tar.gz/commit'}
        write('third-party/source-inputs.json', json.dumps({'archives': [self.record]}))
        for variant, core, version in [('v1', 'framework-arduinoespressif8266', '3.30102.0'), ('v2', 'framework-arduinoespressif32', '3.3.0')]:
            prefix = f'.pio-core/{variant}/packages/{core}/'
            write(prefix + 'package.json', json.dumps({'version': version}))
            write(prefix + 'cores/core.c', 'void core(void) {}\n')
            write(prefix + '.piopm', 'private package state')
            write(prefix + 'tools/helper.exe', 'binary executable')
        write('tmp/private-bench.json', 'must remain private')
        subprocess.run(['git', 'init', '-q'], cwd=self.root, check=True)
        subprocess.run(['git', 'add', '.'], cwd=self.root, check=True)
        subprocess.run(['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'Reviewed source'], cwd=self.root, check=True)

    def package(self):
        return tool.package(self.root, self.root / 'dist/source-materials', self.root / 'dist/source-cache', True)

    def test_bundle_contains_source_not_ignored_secrets_and_binds_materials(self):
        bundle = self.package()
        with tarfile.open(bundle) as archive:
            names = archive.getnames()
            self.assertFalse(any('private-bench' in name or '.piopm' in name or '.exe' in name for name in names))
            project = next(name for name in names if name.endswith('/project.tar'))
            with tarfile.open(fileobj=io.BytesIO(archive.extractfile(project).read())) as original:
                self.assertIn('LICENSE', original.getnames())
                self.assertFalse(any(name.startswith(('tmp/', '.pio-core/', 'dist/')) for name in original.getnames()))
            manifest = json.load(archive.extractfile(next(name for name in names if name.endswith('/MATERIALS.json'))))
            for record in manifest['files']:
                payload = archive.extractfile(record['file']).read()
                self.assertEqual(len(payload), record['size_bytes'])
                self.assertEqual(hashlib.sha256(payload).hexdigest(), record['sha256'])
        self.assertEqual((bundle.parent / 'SOURCE-SHA256SUMS').read_text(), f'{tool.sha256(bundle)}  {bundle.name}\n')

    def test_corrupt_and_symlinked_inputs_are_rejected(self):
        source = self.root / 'dist/source-cache/idf.tar.gz'
        source.write_text('different bytes')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.package()
        source.unlink()
        source.symlink_to(self.root / 'LICENSE')
        with self.assertRaisesRegex(ValueError, 'symlinked source input'):
            self.package()

    def test_dirty_project_and_changed_notices_are_rejected(self):
        self.write('LICENSE', 'Uncommitted source')
        with self.assertRaisesRegex(ValueError, 'working tree is dirty'):
            self.package()
        self.write('third-party/notices/core/LICENSE', 'Missing copyright')
        with self.assertRaisesRegex(ValueError, 'notice checksum mismatch'):
            self.package()

    def test_unsafe_download_and_wrong_core_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'download origin'):
            tool.source_input({**self.record, 'url': 'https://untrusted.invalid/archive'}, self.root, True)
        with self.assertRaisesRegex(ValueError, 'filename'):
            tool.source_input({**self.record, 'file': '../secret.tar.gz'}, self.root, True)
        # The official file host still requires the pinned size and SHA-256.
        record = {**self.record, 'url': 'https://components-file.espressif.com/components/example/source/1.0/source.zip'}
        self.assertTrue(tool.source_input(record, self.root / 'dist/source-cache', True).is_file())
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            tool.source_input({**record, 'sha256': '0' * 64}, self.root / 'dist/source-cache', True)
        with self.assertRaisesRegex(ValueError, 'download origin'):
            tool.source_input({**record, 'url': 'https://components.espressif.com/api/downloads/'}, self.root, True)
        self.write('.pio-core/v2/packages/framework-arduinoespressif32/package.json', '{"version":"other"}')
        with self.assertRaisesRegex(ValueError, 'pinned version'):
            self.package()


if __name__ == '__main__':
    unittest.main()
