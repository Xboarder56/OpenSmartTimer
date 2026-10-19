#!/usr/bin/env python3
"""Public repository, workflow and version contracts; no network or hardware."""
import configparser
from pathlib import Path
import re
import tomllib
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


def workflow(name):
    # BaseLoader preserves the GitHub key "on" instead of YAML 1.1 booleans.
    return yaml.load((ROOT / '.github/workflows' / name).read_text(), Loader=yaml.BaseLoader)


class RepositoryContract(unittest.TestCase):
    def test_actions_are_immutable_and_workflow_permissions_are_scoped(self):
        for path in sorted((ROOT / '.github/workflows').glob('*.yml')):
            data = workflow(path.name)
            self.assertEqual(data['permissions'], {'contents': 'read'})
            for job in data['jobs'].values():
                for step in job.get('steps', []):
                    uses = step.get('uses', '')
                    if uses and not uses.startswith('./'):
                        self.assertRegex(uses, r'^[\w-]+/[\w-]+@[0-9a-f]{40}$')
                if job.get('permissions', {}).get('contents') == 'write':
                    self.assertEqual(path.name, 'release.yml')
                if job.get('permissions', {}).get('id-token') == 'write':
                    self.assertEqual(path.name, 'pages.yml')
                    self.assertEqual(job['environment']['name'], 'github-pages')

    def test_source_ci_and_release_qualification_are_separate(self):
        ci = workflow('ci.yml')
        source_steps = str(ci['jobs']['host-and-web-checks']['steps'])
        self.assertNotIn('verify_qualification.py', source_steps)
        relay = str(ci['jobs']['relay-checks']['steps'])
        for command in ('requirements-dev.lock', 'npm ci', './relay/tests/browser.sh', 'relay_e2e.sh --browser'):
            self.assertIn(command, relay)
        # CI and the full release gate share one browser list.
        self.assertIn('relay/tests/browser.sh', (ROOT / 'scripts/check_release.sh').read_text())
        browsers = (ROOT / 'relay/tests/browser.sh').read_text()
        for script in ('pages', 'appearance', 'timer_status', 'setup_approval', 'watch_reconnect', 'bluetooth_ui', 'ota'):
            self.assertIn(f'chromium {script}_browser.cjs', browsers)
        for script in ('pages', 'timer_status', 'watch_reconnect', 'bluetooth_ui'):
            self.assertIn(f'webkit {script}_browser.cjs', browsers)
        release = workflow('release.yml')['jobs']
        self.assertIn('verify_qualification.py --allow-pending --release-candidate', str(release['release-source']['steps']))
        # The draft refuses CI images that are not the bench-qualified bytes.
        self.assertIn('--images dist/release', str(release['draft']['steps']))
        self.assertEqual(release['verified-build']['needs'], 'release-source')
        self.assertEqual(release['draft']['needs'], 'verified-build')
        self.assertIn('--draft', str(release['draft']['steps']))

    def test_relay_and_firmware_share_first_public_version(self):
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(ROOT / 'platformio.ini')
        expected = ini['opensmarttimer']['firmware_version']
        project = tomllib.loads((ROOT / 'relay/pyproject.toml').read_text())['project']
        self.assertEqual(project['version'], expected)
        init = (ROOT / 'relay/src/ostrelay/__init__.py').read_text()
        self.assertIn(f'__version__ = "{expected}"', init)
        for compose, image in (('relay/docker-compose.yml', 'ostrelay'), ('relay/tests/compose.yml', 'ostrelay-test')):
            self.assertIn(f'image: {image}:{expected}\n', (ROOT / compose).read_text(), compose)
        self.assertIn('__protocol__ = "ost-relay/1"', init)
        self.assertIn('ost-ble/1', (ROOT / 'docs/BLUETOOTH.md').read_text())

    def test_maintained_docs_do_not_link_private_or_archived_material(self):
        roots = [ROOT / 'docs', ROOT / 'relay', ROOT / 'scripts', ROOT / 'tests', ROOT / 'bluetooth-app', ROOT / 'framework']
        paths = [ROOT / 'README.md', ROOT / 'SECURITY.md', ROOT / 'CONTRIBUTING.md', ROOT / 'CHANGELOG.md']
        for root in roots:
            paths.extend(p for p in root.rglob('*.md') if not any(part in {'.venv', 'node_modules', 'out', 'dev'} for part in p.parts))
        checked = 0
        for path in paths:
            for target in re.findall(r'\]\(([^\s)]+)(?:\s+"[^"]*")?\)', path.read_text()):
                if target.startswith(('http:', 'https:', 'mailto:', '#')):
                    continue
                target = target.split('#', 1)[0].replace('%20', ' ')
                self.assertNotIn('notes/', target, path)
                self.assertNotIn('tmp/', target, path)
                self.assertTrue((path.parent / target).exists(), f'{path.relative_to(ROOT)}: missing {target}')
                checked += 1
        self.assertGreater(checked, 100)


if __name__ == '__main__':
    unittest.main()
