#!/usr/bin/env python3
"""Stage only the public Pages site (home page and Bluetooth app), never the complete repository tree."""
import argparse
import configparser
import hashlib
from pathlib import Path
import re
from runpy import run_path

ROOT = Path(__file__).resolve().parents[2]
patch_update_policy = run_path(str(ROOT / 'scripts/build/update_source.py'))['patch_update_policy']
ASSETS = (
    'bluetooth-app/index.html', 'bluetooth-app/app.js', 'bluetooth-app/app.css',
    'bluetooth-app/ble-transport.mjs', 'bluetooth-app/ble-management.mjs', 'bluetooth-app/icon.svg',
    'bluetooth-app/manifest.webmanifest', 'web/styles.css', 'web/i18n.js', 'web/appearance.js', 'web/updates.js',
)


def stage(output, repository, revision):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository must be owner/repository')
    if not re.fullmatch(r'[0-9a-fA-F]{40}', revision):
        raise ValueError('revision must be a full Git commit SHA')
    revision = revision.lower()
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'platformio.ini')
    version = config['opensmarttimer']['firmware_version']
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?', version):
        raise ValueError('invalid firmware version for Pages build label')
    # Refuse nonempty destinations instead of recursively deleting user files.
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Pages destination must be empty')
    source_url = f'https://github.com/{repository}/tree/{revision}'
    digest = hashlib.sha256()
    for relative in ASSETS:
        source = ROOT / relative
        if source.is_symlink():
            raise ValueError(f'Pages asset cannot be a symlink: {relative}')
        data = source.read_bytes()
        if relative == 'bluetooth-app/index.html':
            data = patch_update_policy(data, ROOT)
            replacements = {
                b'href="../README.md" id="sourceLink"':
                    f'href="{source_url}" id="sourceLink"'.encode(),
                b'href="../SAFETY.md" id="safetyLink"':
                    f'href="https://github.com/{repository}/blob/{revision}/SAFETY.md" id="safetyLink"'.encode(),
                b'<span id="appBuild" data-i18n-literal>Development checkout</span>':
                    (f'<span id="appBuild" data-i18n-literal>{version} &middot; '
                     f'<a href="{source_url}" title="Commit {revision}">{revision[:12]}</a></span>').encode(),
            }
            for marker, replacement in replacements.items():
                if data.count(marker) != 1:
                    raise ValueError(f'Pages HTML marker missing or duplicated: {marker!r}')
                data = data.replace(marker, replacement)
        digest.update(relative.encode() + b'\0' + data)
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    worker = (ROOT / 'bluetooth-app/service-worker.js').read_text()
    digest.update(worker.encode())
    worker, count = re.subn(r"(?m)^const APP_VERSION = 'development-v[0-9]+';$",
                            f"const APP_VERSION = '{digest.hexdigest()[:24]}';", worker)
    if count != 1:
        raise ValueError('service worker build marker missing or duplicated')
    (output / 'bluetooth-app/service-worker.js').write_text(worker)
    # The Pages home: its own files, never embedded in timer firmware.
    links = {b'https://github.com/OWNER/REPOSITORY/tree/REVISION': source_url.encode(),
             b'https://github.com/OWNER/REPOSITORY/blob/REVISION/SAFETY.md':
                 f'https://github.com/{repository}/blob/{revision}/SAFETY.md'.encode(),
             b'https://github.com/OWNER/REPOSITORY/releases': f'https://github.com/{repository}/releases'.encode()}
    home = (ROOT / 'pages/index.html').read_bytes()
    for marker, replacement in links.items():
        if home.count(marker) != 1:
            raise ValueError(f'Pages home link marker missing or duplicated: {marker!r}')
        home = home.replace(marker, replacement)
    if b'OWNER/REPOSITORY' in home:
        raise ValueError('Pages home has an unreplaced link')
    (output / 'index.html').write_bytes(home)
    for name in ('home.css', 'home.js'):
        (output / name).write_bytes((ROOT / 'pages' / name).read_bytes())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/pages')
    parser.add_argument('--repository', required=True)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    stage(args.output, args.repository, args.revision)
    print(f'Curated Pages app staged at {args.output}')
